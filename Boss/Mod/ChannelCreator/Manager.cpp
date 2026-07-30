#include"Boss/Mod/ChannelCandidateInvestigator/Main.hpp"
#include"Boss/Mod/ChannelCreator/Carpenter.hpp"
#include"Boss/Mod/ChannelCreator/Manager.hpp"
#include"Boss/Mod/ChannelCreator/Planner.hpp"
#include"Boss/Mod/ChannelCreator/RearrangerBySize.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/AmountSettings.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/RequestChannelCreation.hpp"
#include"Boss/Msg/SolicitChannelCandidates.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/map.hpp"
#include"Ev/memoize.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/FeatureBit.hpp"
#include"Net/IPAddrOrOnion.hpp"
#include"Net/IPBinnerBySubnet.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<algorithm>
#include<assert.h>
#include<cmath>
#include<sstream>

namespace {

/* If all the entries in the plan are 0, the plan is empty.  */
bool plan_is_empty(std::map<Ln::NodeId, Ln::Amount> const& plan) {
	return std::all_of( plan.begin(), plan.end()
			  , [](std::pair<Ln::NodeId, Ln::Amount> const& e) {
		return e.second == Ln::Amount::sat(0);
	});
}

void append_entry(std::string& s, std::string const& entry) {
	if (!s.empty())
		s += ", ";
	s += entry;
}

Ev::Io<void> report_proposals( S::Bus& bus, char const* prefix
			     , std::vector< std::pair<Ln::NodeId, Ln::NodeId>
					  > const& proposals
			     ) {
	auto os = std::ostringstream();

	auto first = true;
	for (auto& p : proposals) {
		if (first)
			first = false;
		else
			os << ", ";
		os << p.first;
	}
	return Boss::log( bus, Boss::Debug
			, "ChannelCreator: %s: %s"
			, prefix
			, os.str().c_str()
			);
}

}

namespace Boss { namespace Mod { namespace ChannelCreator {

void Manager::start() {
	bus.subscribe<Msg::Manifestation
		     >([this](Msg::Manifestation const& _) {
		return bus.raise(Msg::ManifestOption{
			"clboss-candidate-prefer-spliceable",
			Msg::OptionType_Bool,
			Json::Out::direct(true),
			"Whether channel-open candidates without a "
			"track record are ordered so that nodes "
			"announcing splicing support come first.  "
			"Dynamic: settable at runtime via "
			"`lightning-cli setconfig`.  Default true.",
			/* dynamic = */ true
		});
	});
	bus.subscribe<Msg::Option
		     >([this](Msg::Option const& o) {
		if (o.name != "clboss-candidate-prefer-spliceable")
			return Ev::lift();
		/* Tolerate both the bool-at-startup and
		 * string-via-setconfig encodings.  */
		if (o.value.is_boolean())
			prefer_spliceable = bool(o.value);
		else if ( o.value.is_string()
		       && ( std::string(o.value) == "true"
			 || std::string(o.value) == "false"
			  ))
			prefer_spliceable = (std::string(o.value) == "true");
		else {
			o.reject( "clboss-candidate-prefer-spliceable: "
				  "expected true or false"
				);
			return Boss::log( bus, Warn
					, "ChannelCreator: "
					  "clboss-candidate-prefer-spliceable: "
					  "expected true or false; keeping %s."
					, prefer_spliceable ? "true" : "false"
					);
		}
		return Ev::lift();
	});
	bus.subscribe<Msg::AmountSettings
		     >([this](Msg::AmountSettings const& m) {
		min_amount = m.min_channel;
		max_amount = m.max_channel;
		min_remaining = m.min_remaining;
		return Ev::lift();
	});
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		self = init.self_id;
		reprioritizer = Util::make_unique<Reprioritizer>
			( init.signer
			, Util::make_unique<Net::IPBinnerBySubnet>()
			, [this](Ln::NodeId n) { return get_node_addr(n); }
			, [this]() { return get_peers(); }
			);
		return Ev::lift();
	});
	bus.subscribe<Msg::RequestChannelCreation
		     >([this](Msg::RequestChannelCreation const& rcc) {
		if (!rpc)
			return Ev::lift();
		return Boss::concurrent(
			on_request_channel_creation(rcc.amount)
		);
	});
}

Ev::Io<void>
Manager::on_request_channel_creation(Ln::Amount amt) {
	/* The Planner asserts both of these at construction; check
	 * here and skip the cycle instead of aborting.  The first
	 * can fail if onchain funds changed between the decider's
	 * trigger and now; the second is enforced at option
	 * validation, so failing it here is a bug.  */
	if (amt < min_amount * 2.0)
		return Boss::log( bus, Warn
				, "ChannelCreator: Onchain amount %s "
				  "below twice the minimum channel size "
				  "%s, not creating channels."
				, std::string(amt).c_str()
				, std::string(min_amount).c_str()
				);
	if (min_amount + min_remaining > max_amount)
		return Boss::log( bus, Error
				, "ChannelCreator: Channel size limits "
				  "(min %s, max %s) violate the planner "
				  "precondition, not creating channels."
				, std::string(min_amount).c_str()
				, std::string(max_amount).c_str()
				);

	auto num_chans = std::make_shared<std::size_t>();
	auto plan = std::make_shared<std::map<Ln::NodeId, Ln::Amount>>();

	/* Construct the dowser function.  */
	auto base_dowser_func = [this]( Ln::NodeId proposal
				      , Ln::NodeId patron
				      ) {
		auto amount = std::make_shared<Ln::Amount>();

		return Ev::lift().then([this, proposal, patron]() {
			/* Size the probe to max_amount (clboss-max-channel),
			 * NOT min_amount: the Planner opens up to the dowsed
			 * flow (rejecting below min_amount, capping at
			 * max_amount), and the askrene dowser caps its result
			 * at the probe -- so probing at min_amount would pin
			 * every new channel to min-channel regardless of the
			 * candidate's real capacity.  Probing at max_amount
			 * lets a well-connected candidate report its true
			 * reachable flow up to the largest channel we'd open.  */
			return dowser.execute(Msg::RequestDowser{
				nullptr, proposal, patron, max_amount
			});
		}).then([this
			, amount
			, proposal
			, patron
			](Msg::ResponseDowser resp) {
			*amount = resp.amount;
			return Boss::log( bus, Debug
					, "ChannelCreator: "
					  "Propose %s to %s "
					  "(patron %s)"
					, std::string(*amount).c_str()
					, std::string(proposal).c_str()
					, std::string(patron).c_str()
					);
		}).then([amount]() {
			return Ev::lift(*amount);
		});
	};
	auto dowser_func = Ev::memoize(std::move(base_dowser_func));

	return Ev::lift().then([this]() {
		return Boss::log( bus, Debug
				, "ChannelCreator: Triggered."
				);
	}).then([this]() {
		return rpc->command("getinfo"
				   , Json::Out::empty_object()
				   );
	}).then([this, num_chans](Jsmn::Object info) {
		*num_chans = (double)info["num_pending_channels"]
			   + (double)info["num_active_channels"]
			   ;
		return investigator.get_channel_candidates();
	}).then([ dowser_func
		](std::vector<std::pair<Ln::NodeId, Ln::NodeId>> proposals) {
		auto rearranger = RearrangerBySize(dowser_func);

		/* First, rearrange slightly perturbs the given order of
		 * proposals, letting a higher-capacity proposal go up in
		 * priority.
		 */
		return rearranger.rearrange_by_size(proposals);
	}).then([this](std::vector<std::pair<Ln::NodeId, Ln::NodeId>> proposals) {
		/* Then, we reprioritze according to IP binning, greatly
		 * reducing the chance that we will create channels to
		 * nodes with similar locations.
		 */
		return reprioritize(std::move(proposals));
	}).then([this](std::vector<std::pair<Ln::NodeId, Ln::NodeId>> proposals) {
		/* Finally, partition by earnings track record, so
		 * proven earners are funded first and known
		 * underperformers only when nothing else can absorb
		 * the funds.  This runs after the rearranger and
		 * reprioritizer on purpose: those two only perturb
		 * the order, and must not promote a candidate across
		 * a track-record tier boundary.  The Planner consumes
		 * proposals in order until funds run out, so placing
		 * a tier last implements "only if there are no
		 * others" without an outright veto.
		 */
		return prioritize_by_track_record(std::move(proposals));
	}).then([ num_chans
		, amt
		, dowser_func
		, this
		](std::vector<std::pair<Ln::NodeId, Ln::NodeId>> proposals) {
		auto planner = Planner( std::move(dowser_func)
				      , amt
				      , std::move(proposals)
				      , *num_chans
				      , min_amount
				      , max_amount
				      , min_remaining
				      );
		return std::move(planner).run();
	}).then([plan](std::map<Ln::NodeId, Ln::Amount> n_plan) {
		*plan = n_plan;

		return Ev::yield();
	}).then([this, plan]() {

		if (plan_is_empty(*plan)) {
			return Boss::log( bus, Info
					, "ChannelCreator: Insufficient "
					  "channel candidates, will solicit "
					  "more."
					).then([this]() {
				return bus.raise(
					Msg::SolicitChannelCandidates()
				);
			});
		}

		auto report = std::ostringstream();
		auto first = true;
		for (auto const& p : *plan) {
			if (p.second == Ln::Amount::sat(0))
				continue;
			if (first)
				first = false;
			else
				report << ", ";
			report << p.first << ": " << p.second;
		}

		return Boss::log( bus, Info
				, "ChannelCreator: %s"
				, report.str().c_str()
				);
	}).then([this, plan]() {
		/* Carpenter is responsible for disseminating 0-amount
		 * candidates as failures to create channels.
		 * So `plan_is_empty` will still cause this to be called,
		 * in case the plan has any 0-amount entries.
		 * Thus, we need to separately check that the plan is truly
		 * empty here, else the Carpenter assert will trigger.
		 */
		if (plan->empty())
			return Ev::lift();
		return carpenter.construct(std::move(*plan));
	});
}
Ev::Io<std::unique_ptr<Net::IPAddrOrOnion>>
Manager::get_node_addr(Ln::NodeId n) {
	assert(rpc);
	return Ev::lift().then([this, n]() {
		return rpc->command("listnodes"
				   , Json::Out()
					.start_object()
						.field("id", std::string(n))
					.end_object()
				   );
	}).then([this](Jsmn::Object res) {
		auto rv = Net::IPAddrOrOnion();
		try {
			auto nodes = res["nodes"];
			/* Node not known?  */
			if (nodes.length() == 0)
				return Ev::lift(std::unique_ptr<Net::IPAddrOrOnion>());
			auto node = nodes[0];
			auto addrs = node["addresses"];
			/* No addresses known for node?  */
			if (addrs.length() == 0)
				return Ev::lift(std::unique_ptr<Net::IPAddrOrOnion>());
			/* Report first address.  */
			auto addr_j = addrs[0];
			auto addr_s = std::string(addr_j["address"]);
			rv = Net::IPAddrOrOnion(addr_s);
		} catch (...) {
			return Boss::log( bus, Error
					, "ChannelCreator: Unexpected result from "
					  "listnodes: %s"
					, res.direct_text().c_str()
					).then([]() {
				return Ev::lift(std::unique_ptr<Net::IPAddrOrOnion>());
			});
		}
		return Ev::lift(Util::make_unique<Net::IPAddrOrOnion>(std::move(rv)));
	});
}
Ev::Io<std::vector<Ln::NodeId>>
Manager::get_peers() {
	assert(rpc);
	return Ev::lift().then([this]() {
		return rpc->command("listpeers", Json::Out::empty_object());
	}).then([this](Jsmn::Object res) {
		auto rv = std::vector<Ln::NodeId>();
		try {
			auto peers = res["peers"];
			for (auto peer : peers) {
				auto id_j = peer["id"];
				auto id_s = std::string(id_j);
				auto id = Ln::NodeId(id_s);
				rv.push_back(std::move(id));
			}
		} catch (...) {
			return Boss::log( bus, Error
					, "ChannelCreator: Unexpected result from "
					  "listpeers: %s"
					, res.direct_text().c_str()
					).then([rv]() {
				return Ev::lift(rv);
			});
		}
		return Ev::lift(std::move(rv));
	});
}
Ev::Io<std::set<Ln::NodeId>>
Manager::get_spliceable_nodes(std::vector<Ln::NodeId> nodes) {
	assert(rpc);
	auto lookup = [this](Ln::NodeId n) {
		return rpc->command("listnodes"
				   , Json::Out()
					.start_object()
						.field("id", std::string(n))
					.end_object()
				   ).then([n](Jsmn::Object res) {
			auto spliceable = false;
			try {
				auto ns = res["nodes"];
				if (ns.length() != 0 && ns[0].has("features")) {
					auto f = std::string(ns[0]["features"]);
					/* BOLT #9 `option_splice`.  */
					spliceable = Ln::feature_bit(f, 62)
						  || Ln::feature_bit(f, 63)
						   ;
				}
			} catch (...) { /* Treat as not spliceable.  */ }
			return Ev::lift(std::make_pair(n, spliceable));
		}).catching<RpcError>([n](RpcError const&) {
			return Ev::lift(std::make_pair(n, false));
		});
	};
	return Ev::map( std::move(lookup), std::move(nodes)
		      ).then([](std::vector<std::pair<Ln::NodeId, bool>> flags) {
		auto rv = std::set<Ln::NodeId>();
		for (auto const& f : flags)
			if (f.second)
				rv.insert(f.first);
		return Ev::lift(std::move(rv));
	});
}
Ev::Io<std::vector<std::pair<Ln::NodeId, Ln::NodeId>>>
Manager::reprioritize(std::vector<std::pair<Ln::NodeId, Ln::NodeId>> proposals_v) {
	auto proposals = std::make_shared<std::vector<std::pair<Ln::NodeId, Ln::NodeId>>>
		(std::move(proposals_v));
	return Ev::lift().then([this, proposals]() {
		return report_proposals( bus, "Proposals from ChannelCandidateInvestigator"
				       , *proposals
				       );
	}).then([this, proposals]() {
		return reprioritizer->reprioritize(std::move(*proposals));
	}).then([this, proposals](std::vector< std::pair<Ln::NodeId, Ln::NodeId>
					     > n_proposals) {
		*proposals = std::move(n_proposals);
		return report_proposals( bus, "After reprioritization from IP binning"
				       , *proposals
				       );
	}).then([proposals]() {
		return Ev::lift(std::move(*proposals));
	});
}
Ev::Io<std::vector<std::pair<Ln::NodeId, Ln::NodeId>>>
Manager::prioritize_by_track_record(std::vector<std::pair<Ln::NodeId, Ln::NodeId>> proposals_v) {
	typedef std::vector<std::pair<Ln::NodeId, Ln::NodeId>> Proposals;

	if (proposals_v.empty())
		return Ev::lift(std::move(proposals_v));

	auto nodes = std::vector<Ln::NodeId>();
	for (auto const& p : proposals_v)
		nodes.push_back(p.first);
	auto proposals = std::make_shared<Proposals>(std::move(proposals_v));

	return track_record.execute(Msg::RequestPeerTrackRecord{
		nullptr, std::move(nodes)
	}).then([ this
		, proposals
		](Msg::ResponsePeerTrackRecord resp) {
		auto keepers = std::make_shared<Proposals>();
		auto no_records = std::make_shared<Proposals>();
		auto underperformers = std::make_shared<Proposals>();

		/* Per-tier report text; nodes within a tier keep their
		 * relative order from the earlier stages.  The no-record
		 * text is built later, after the splice preference has
		 * settled that tier's order.  */
		auto keepers_s = std::make_shared<std::string>();
		auto underperformers_s = std::make_shared<std::string>();

		for (auto const& p : *proposals) {
			auto rec = Msg::TrackRecord{
				Msg::TrackRecordVerdict::NoRecord,
				0.0, 0.0, 0
			};
			auto it = resp.records.find(p.first);
			if (it != resp.records.end())
				rec = it->second;

			auto os = std::ostringstream();
			os << p.first;
			if (rec.verdict != Msg::TrackRecordVerdict::NoRecord)
				os << "(" << std::showpos
				   << (long long) std::llround(rec.tral_bps)
				   << std::noshowpos << "bps/"
				   << (long long) std::llround(rec.op_days)
				   << "d)"
				   ;

			switch (rec.verdict) {
			case Msg::TrackRecordVerdict::Keeper:
				keepers->push_back(p);
				append_entry(*keepers_s, os.str());
				break;
			case Msg::TrackRecordVerdict::NoRecord:
				no_records->push_back(p);
				break;
			case Msg::TrackRecordVerdict::Underperformer:
				underperformers->push_back(p);
				append_entry(*underperformers_s, os.str());
				break;
			}
		}

		/* The no-record tier carries no earnings evidence, so
		 * order it by a capability prior: nodes announcing
		 * splicing support first, since their channels can be
		 * resized later without a close+reopen.  Keepers and
		 * underperformers are left alone; earnings evidence
		 * outranks the prior, and this must not move anyone
		 * across a tier boundary.  Opt out with
		 * clboss-candidate-prefer-spliceable=false, which also
		 * skips the listnodes lookups.  */
		auto no_record_nodes = std::vector<Ln::NodeId>();
		if (prefer_spliceable)
			for (auto const& p : *no_records)
				no_record_nodes.push_back(p.first);

		return get_spliceable_nodes( std::move(no_record_nodes)
					   ).then([ this
						  , proposals
						  , keepers
						  , no_records
						  , underperformers
						  , keepers_s
						  , underperformers_s
						  ](std::set<Ln::NodeId> spliceable) {
			std::stable_partition( no_records->begin()
					     , no_records->end()
					     , [&spliceable]( std::pair< Ln::NodeId
								       , Ln::NodeId
								       > const& p) {
				return spliceable.count(p.first) != 0;
			});

			auto no_records_s = std::string();
			for (auto const& p : *no_records) {
				auto os = std::ostringstream();
				os << p.first;
				if (spliceable.count(p.first) != 0)
					os << "(S)";
				append_entry(no_records_s, os.str());
			}

			auto report = std::string();
			if (!keepers_s->empty())
				report += "keepers: " + *keepers_s + "; ";
			if (!no_records_s.empty())
				report += "no record: " + no_records_s + "; ";
			if (!underperformers_s->empty())
				report += "underperformers: "
					+ *underperformers_s + "; "
					;
			/* Trim the trailing "; ".  */
			report.erase(report.size() - 2);

			*proposals = std::move(*keepers);
			proposals->insert( proposals->end()
					 , no_records->begin(), no_records->end()
					 );
			proposals->insert( proposals->end()
					 , underperformers->begin()
					 , underperformers->end()
					 );

			return Boss::log( bus, Info
					, "ChannelCreator: Track records: %s"
					, report.c_str()
					).then([proposals]() {
				return Ev::lift(std::move(*proposals));
			});
		});
	});
}

}}}
