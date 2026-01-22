#include"Boss/Mod/ChannelCandidateInvestigator/Gumshoe.hpp"
#include"Boss/Mod/ChannelCandidateInvestigator/Janitor.hpp"
#include"Boss/Mod/ChannelCandidateInvestigator/Manager.hpp"
#include"Boss/Mod/ChannelCandidateInvestigator/Secretary.hpp"
#include"Boss/Mod/InternetConnectionMonitor.hpp"
#include"Boss/Msg/AmountSettings.hpp"
#include"Boss/Msg/ChannelCreateResult.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/PatronizeChannelCandidate.hpp"
#include"Boss/Msg/ProposeChannelCandidates.hpp"
#include"Boss/Msg/ProposePatronlessChannelCandidate.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/ProvideUnmanagement.hpp"
#include"Boss/Msg/SolicitChannelCandidates.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/SolicitUnmanagement.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/Msg/TimerTwiceDaily.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/map.hpp"
#include"Ln/Amount.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include<algorithm>
#include<iterator>
#include<random>

namespace {

/* If we are below this number of non-negative candidates,
 * solicit more.  */
auto const min_good_candidates = std::size_t(8);
/* If we have more than this number of total candidates,
 * drop by random.  */
auto const max_candidates = std::size_t(32);

/* Nodes below this score should just be dropped.  */
auto const min_score = std::int64_t(-3);
/* Nodes whose score would go above this score will saturate at this
 * score.
 */
auto const max_score = std::int64_t(24);

/* Number of candidates to investigate in parallel.  */
auto const max_investigation = std::size_t(8);

}

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator {

void Manager::start() {
	/* Give the gumshoe the report function.  */
	gumshoe.set_report_func([this](Ln::NodeId n, bool success) {
		return db.transact().then([this, n, success](Sqlite3::Tx tx) {
			secretary.update_score( tx, n
					      , success ? +1 : -1
					      , min_score
					      , max_score
					      );
			tx.commit();

			return Ev::lift();
		});
	});

	bus.subscribe< Msg::AmountSettings
		     >([this](Msg::AmountSettings const& r) {
		min_channel = r.min_channel;
		return Ev::lift();
	});

	bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
		db = init.db;

		/* Initialize the database.  */
		return db.transact().then([this](Sqlite3::Tx tx) {
			secretary.initialize(tx);

			/* We got the db just now, so handle
			 * any pending unmanagement.
			 */
			for (auto const& n : unmanaged)
				secretary.remove_candidate(tx, n);

			/* Get number of good candidates.  */
			auto good_candidates =
				secretary.get_nonnegative_candidates_count(tx);
			tx.commit();

			if (good_candidates < min_good_candidates)
				return Boss::concurrent(
					solicit_candidates(good_candidates)
				);

			return Ev::lift();
		});
	});
	/* Allow disabling management of opening to particular nodes.  */
	bus.subscribe< Msg::SolicitUnmanagement
		     >([this](Msg::SolicitUnmanagement const& _) {
		return bus.raise(Msg::ProvideUnmanagement{
			"open", [this](Ln::NodeId const& n, bool u) {
				if (u) {
					unmanaged.insert(n);
					/* This can happen at initialize,
					 * at which point we do not have
					 * access to the db yet.
					 * If so, just return; `Init`
					 * handling will handle unmanaged
					 * nodes when it gets the db.
					 */
					if (!db)
						return Ev::lift();
					return db.transact().then([this, n](Sqlite3::Tx tx) {
						secretary.remove_candidate(tx, n);
						tx.commit();
						return Ev::lift();
					});
				} else {
					unmanaged.erase(unmanaged.find(n));
					return Ev::lift();
				}
			}
		});
	});
	/* When a new candidate is proposed, add it, unless it is unmanaged.  */
	bus.subscribe<Msg::ProposeChannelCandidates
		     >([this](Msg::ProposeChannelCandidates const& c) {
		if (unmanaged.count(c.proposal) != 0)
			return Boss::log( bus, Info
					, "ChannelCandidateInvestigator: %s "
					  "proposed as channel cadidate, but it is unmanaged "
					  "by \"open\", will reject candidacy."
					, std::string(c.proposal).c_str()
					);

		/* Ask the janitor if this candidate is OK, if not,
		 * reject it outright.
		 */
		return janitor.check_acceptable( c.proposal
					       , c.patron
					       , min_channel
					       ).then([this, c](bool acceptable) {
			if (!acceptable)
				return Ev::lift();

			/* Insert it into db.  */
			return db.transact().then([this, c](Sqlite3::Tx tx) {
				secretary.add_candidate(tx, c);
				tx.commit();

				return Ev::lift();
			});
		});
	});
	/* When a new candidate without a patron is proposed,
	 * use existing candidates as guide for another module
	 * to figure out a good patron.
	 */
	bus.subscribe< Msg::ProposePatronlessChannelCandidate
		     >([ this
		       ](Msg::ProposePatronlessChannelCandidate const& m) {
		auto node = m.proposal;
		return db.transact().then([this, node](Sqlite3::Tx tx) {
			if (secretary.is_candidate(tx, node))
				return Ev::lift();

			/* Otherwise use channeling candidates as guide.  */
			auto cands = secretary.get_for_channeling(tx);
			auto guide = std::vector<Ln::NodeId>(cands.size());
			std::transform( cands.begin(), cands.end()
				      , guide.begin()
				      , [](Msg::ProposeChannelCandidates
						const& c) {
				return c.proposal;
			});

			tx.commit();

			/* Re-emit.  */
			return bus.raise(Msg::PatronizeChannelCandidate{
				node, std::move(guide)
			});
		});
	});

	/* Once a day, have janitor clean up our database.  */
	bus.subscribe<Msg::TimerTwiceDaily
		     >([this](Msg::TimerTwiceDaily const& _) {
		if (!db)
			return Ev::lift();
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto ptx = std::make_shared<Sqlite3::Tx>(std::move(tx));
			return janitor.clean_up(secretary, ptx, min_channel
					       ).then([ptx]() {
				ptx->commit();
				return Ev::lift();
			});
		});
	});

	bus.subscribe<Msg::TimerRandomHourly
		     >([this](Msg::TimerRandomHourly const& _) {
		/* Construct shared variables.  */
		/* Number of good candidates.  */
		auto good_candidates = std::make_shared<std::size_t>();
		/* What cases to give to the gumshoe.  */
		auto cases = std::make_shared<std::vector<Ln::NodeId>>();
		/* Human-readable text.  */
		auto report = std::make_shared<std::string>();

		return db.transact().then([=, this](Sqlite3::Tx tx) {
			auto act = Ev::lift();
			/* First, if there are too many candidates,
			 * delete one by random.
			 */
			auto all = secretary.get_all(tx);
			if (all.size() > max_candidates) {
				auto dist = std::uniform_int_distribution<std::size_t>(
					0, all.size() - 1
				);
				auto n = all[dist(Boss::random_engine)].first;
				secretary.remove_candidate(tx, n);
				act += Boss::log( bus, Info
						, "ChannelCandidateInvestigator: "
						  "Randomly dropping %s from "
						  "investigation, "
						  "we have too many "
						  "candidates."
						, std::string(n).c_str()
						);
			}

			/* Determine number of good candidates.  */
			*good_candidates =
				secretary.get_nonnegative_candidates_count(tx);
			/* Get some nodes for investigation.  */
			*cases = secretary.get_for_investigation(
				tx, max_investigation
			);
			/* Get a report.  */
			*report = secretary.report(tx);
			/* Finish up.  */
			tx.commit();

			/* Print the report.  */
			act += Boss::log( bus, Info
					, "ChannelCandidateInvestigator: %s"
					, report->c_str()
					);

			return act;
		}).then([=, this]() {
			/* If we are online, continue.  */
			if (imon.is_online())
				return Ev::lift();

			/* We are offline, suppress investigation.  */
			cases->clear();
			return Boss::log( bus, Info
					, "ChannelCandidateInvestigator: "
					  "Offline.  Not investigating."
					);
		}).then([=, this]() {
			/* If too few candidates, get more.  */
			if (*good_candidates < min_good_candidates)
				return solicit_candidates(*good_candidates);
			/* Even if we have sufficient, have a chance to
			 * get more.
			 */
			auto dist = std::uniform_int_distribution<std::size_t>(
				0, (*good_candidates) + (*good_candidates) / 2
			);
			if (dist(Boss::random_engine) == 0)
				return solicit_candidates(*good_candidates);

			return Ev::lift();
		}).then([=, this]() {
			/* Hand over possible cases to the gumshoe.  */
			auto to_gumshoe = [this](Ln::NodeId n) {
				return gumshoe.investigate(n).then([]() {
					return Ev::lift<int>(0);
				});
			};
			return Ev::map(to_gumshoe, std::move(*cases));
		}).then([](std::vector<int>) {

			return Ev::lift();
		});
	});

	/* Remove candidates we already have a channel with.  */
	bus.subscribe<Msg::ListpeersAnalyzedResult
		     >([this](Msg::ListpeersAnalyzedResult const& l) {
		/* If this is the initial one, we might not have a
		 * db yet.
		 */
		if (!db)
			return Ev::lift();

		/* Remove the peers already listed as channeled.  */
		auto to_remove = std::make_shared<std::vector<Ln::NodeId>>();
		std::copy( l.connected_channeled.begin()
			 , l.connected_channeled.end()
			 , std::back_inserter(*to_remove)
			 );
		std::copy( l.disconnected_channeled.begin()
			 , l.disconnected_channeled.end()
			 , std::back_inserter(*to_remove)
			 );

		/* Now have the secretary update our records.  */
		return db.transact().then([this, to_remove](Sqlite3::Tx tx) {
			for (auto const& n : *to_remove)
				secretary.remove_candidate(tx, n);
			tx.commit();
			return Ev::lift();
		});
	});

	/* Remove candidates that we have tried to channel with, regardless
	 * of whether the attempt failed or succeeded.  */
	bus.subscribe<Msg::ChannelCreateResult
		     >([this](Msg::ChannelCreateResult const& c) {
		if (!db)
			return Ev::lift();
		auto to_remove = std::make_shared<Ln::NodeId>(c.node);
		return Boss::log( bus, Debug
				, "ChannelCandidateInvestigator: "
				  "Node %s used in channeling attempt, "
				  "removing from investigation."
				, std::string(c.node).c_str()
				).then([this]() {
			return db.transact();
		}).then([this, to_remove](Sqlite3::Tx tx) {
			secretary.remove_candidate(tx, *to_remove);
			tx.commit();
			return Ev::lift();
		});
	});

	/* Get status.  */
	bus.subscribe<Msg::SolicitStatus
		     >([this](Msg::SolicitStatus const& c) {
		if (!db)
			return Ev::lift();
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto all = secretary.get_all(tx);
			tx.commit();
			auto status = Json::Out();
			auto arr = status.start_array();
			for (auto const& p : all) {
				auto e = Json::Out()
					.start_object()
						.field( "id"
						      , std::string(p.first)
						      )
						.field( "onlineness"
						      , double(p.second)
						      )
					.end_object()
					;
				arr.entry(std::move(e));
			}
			arr.end_array();
			return bus.raise(Msg::ProvideStatus{
				"channel_candidates", std::move(status)
			});
		});
	});
}
Ev::Io<void> Manager::solicit_candidates(std::size_t good_candidates) {
	return Boss::log( bus, Debug
			, "ChannelCandidateInvestigator: "
			  "Have %zu channel candidates, "
			  "soliciting more."
			, good_candidates
			).then([this]() {
		return bus.raise(
			Msg::SolicitChannelCandidates{}
		);
	});
}

Ev::Io<std::vector<std::pair<Ln::NodeId, Ln::NodeId>>>
Manager::get_channel_candidates() {
	return Ev::lift().then([this]() {
		return db.transact();
	}).then([this](Sqlite3::Tx tx) {
		auto candidates = secretary.get_for_channeling(tx);
		tx.commit();

		auto ncandidates = std::vector<std::pair<Ln::NodeId, Ln::NodeId>>(candidates.size());
		/* Transform.  */
		std::transform( candidates.begin(), candidates.end()
			      , ncandidates.begin()
			      , [](Msg::ProposeChannelCandidates const& p) {
			return std::make_pair(p.proposal, p.patron);
		});

		return Ev::lift(std::move(ncandidates));
	});
}

}}}
