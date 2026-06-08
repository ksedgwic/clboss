#include"Boss/Mod/InitialRebalancer.hpp"
#include"Boss/ModG/RebalanceModeProxy.hpp"
#include"Boss/ModG/RebalanceUnmanagerProxy.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/map.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Stats/ReservoirSampler.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<assert.h>
#include<map>
#include<set>
#include<sstream>
#include<vector>

namespace {

/* If the spendable amount exceeds this percent of the channel total,
 * this code triggers.
 */
auto constexpr spendable_percent = double(80.0);
/* Gap to prevent destinations from hitting the spendable_percent.  */
auto constexpr dest_gap_percent = double(5.0);
/* Limit on rebalance fee.  */
auto const min_rebalance_fee = Ln::Amount::sat(3);
auto constexpr rebalance_fee_percent = double(0.25);

/* Limit on total amount this module will expend on all rebalances,
 * as a percent of the channel capacity.  */
auto constexpr max_in_expenditures_percent = double(0.04); // 10mBTC * 0.0004 = 400 sats

}

namespace Boss { namespace Mod {

class InitialRebalancer::Impl {
private:
	S::Bus& bus;
	/* Interface to funds mover.  */
	typedef 
	ModG::ReqResp< Msg::RequestMoveFunds
		     , Msg::ResponseMoveFunds
		     > MoveRR;
	MoveRR move_rr;
	/* Interface to expenditures tracker.  */
	typedef
	ModG::ReqResp< Msg::RequestEarningsInfo
		     , Msg::ResponseEarningsInfo
		     > ExpenseRR;
	ExpenseRR expense_rr;
	/* Interface to the rebalance unmanager.  */
	ModG::RebalanceUnmanagerProxy unmanager;
	ModG::RebalanceModeProxy mode_proxy;

	/* Peers currently being rebalanced.  */
	std::set<Ln::NodeId> current_sources;

	void start() {
		bus.subscribe<Msg::ListpeersResult
			     >([this](Msg::ListpeersResult const& m) {
			/* If this is the initial startup, then we might not
			 * be connected to the peers involved yet, so better
			 * to wait and let it "simmer" a bit.
			 */
			if (m.initial)
				return Ev::lift();
			return run(m.cpeers);
		});
	}

	class Run {
	private:
		class Impl;
		std::shared_ptr<Impl> pimpl;

	public:
		Run() =delete;

		Run(Run&&) =default;
		~Run() =default;

		explicit
		Run( S::Bus& bus, Boss::Mod::ConstructedListpeers const& peers
		   , MoveRR& move_rr, ExpenseRR& expense_rr
		   , std::set<Ln::NodeId>& current_sources
		   , std::set<Ln::NodeId> const& unmanaged
		   );
		Ev::Io<void> run();
	};

	Ev::Io<void>
	run(Boss::Mod::ConstructedListpeers const& peers) {
		auto ppeers = std::make_shared<Boss::Mod::ConstructedListpeers>(peers);
		return mode_proxy.get_mode().then([this, ppeers](RebalanceMode m) {
			/* Self-gate on the rebalance mode: only the "classic"
			 * track runs the InitialRebalancer.  */
			if (m != RebalanceMode::classic)
				return Ev::lift();
			return Ev::lift().then([this]() {
				return unmanager.get_unmanaged();
			}).then([ this
				, ppeers
				](std::set<Ln::NodeId> const* unmanagedp) {
				return Boss::concurrent( Run( bus
							    , *ppeers
							    , move_rr
							    , expense_rr
							    , current_sources
							    , *unmanagedp
							    ).run()
						       );
			});
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl( S::Bus& bus_
	    ) : bus(bus_)
	      , move_rr(bus_)
	      , expense_rr(bus_)
	      , unmanager(bus_)
	      , mode_proxy(bus_)
	      { start(); }
};

class InitialRebalancer::Impl::Run::Impl
		: public std::enable_shared_from_this<Impl> {
private:
	S::Bus& bus;
	Boss::Mod::ConstructedListpeers peers;

	/* Data about a peer.  */
	struct Info {
		Ln::Amount spendable;
		Ln::Amount receivable;
		Ln::Amount total;
	};
	std::map<Ln::NodeId, Info> info;
	/* Sources and destinations.  */
	std::vector<std::pair<Ln::NodeId, Ln::Amount>> sources_total;
	std::vector<Ln::NodeId> sources;
	std::map<Ln::NodeId, Info> destinations;
	/* Plan to move.  */
	std::vector<std::pair<Ln::NodeId, Ln::NodeId>> plan;

	/* Interface to funds mover.  */
	ModG::ReqResp< Msg::RequestMoveFunds
		     , Msg::ResponseMoveFunds
		     >& move_rr;
	/* Interface to expenditures tracker.  */
	ModG::ReqResp< Msg::RequestEarningsInfo
		     , Msg::ResponseEarningsInfo
		     >& expense_rr;
	std::set<Ln::NodeId>& current_sources;

	/* Set of unmanaged nodes.  */
	std::set<Ln::NodeId> const& unmanaged;

	Ev::Io<void> core_run() {
		return Ev::lift().then([this]() {
			try {
				for (auto p : peers) {
					auto spendable = Ln::Amount::sat(0);
					auto receivable = Ln::Amount::sat(0);
					auto total = Ln::Amount::sat(0);

                                        auto id = p.first;
					if (unmanaged.count(id) != 0)
						continue;

					auto cs = p.second.channels;
					for (auto c : cs) {
						auto state = std::string(
							c["state"]
						);
						if (state != "CHANNELD_NORMAL")
							continue;
						compute_spendable( spendable
								 , receivable
								 , total
								 , c
								 );
					}

					if (total == Ln::Amount::sat(0))
						continue;
					info[id].spendable = spendable;
					info[id].receivable = receivable;
					info[id].total = total;
				}
			} catch (std::exception const& e) {
				return Boss::log( bus, Error
						  , "InitialRebalancer:"
						  " Unexpected exception: %s "
						  " handling: %s"
						  , e.what()
						  , Util::stringify(peers).c_str()
						);
			}
			return plan_move();
		});
	}
	void compute_spendable( Ln::Amount& a_spendable
			      , Ln::Amount& a_receivable
			      , Ln::Amount& a_total
			      , Jsmn::Object const& c
			      ) {
		if ( !c.has("to_us_msat")
		  || !c.has("total_msat")
		  || !c.has("htlcs")
		   )
			return;
		/* FIXME: Handle reserves.  */
		auto to_us = Ln::Amount::object(c["to_us_msat"]);
		auto total = Ln::Amount::object(c["total_msat"]);
		auto to_them = total - to_us;
		for (auto h : c["htlcs"])
			to_them -= Ln::Amount::object(
				h["amount_msat"]
		);
		a_spendable += to_us;
		a_receivable += to_them;
		a_total += total;
	}
	Ev::Io<void> plan_move() {
		auto msg = std::ostringstream();
		auto first = true;
		/* Gather sources and destinations.  */
		sources_total.clear();
		destinations.clear();
		for ( auto it = info.begin(), next = info.begin()
		    ; it != info.end()
		    ; it = next
		    ) {
			next = it;
			++next;

			if (first)
				first = false;
			else
				msg << ", ";

			auto& info = it->second;
			auto peer_spendable_percent = ( info.spendable
						      / info.total
						      )
						    * 100.0
						    ;
			msg << it->first << ": "
			    << peer_spendable_percent << "% "
			    ;
			if (peer_spendable_percent >= spendable_percent) {
				sources_total.push_back(std::make_pair( it->first
								      , info.total
								      ));
				msg << "(source)";
			} else if ( peer_spendable_percent
				 >= (spendable_percent - dest_gap_percent)
				  ) {
				msg << "(neutral)";
			} else {
				destinations.insert(*it);
				msg << "(destination)";
			}
		}
		auto act = Ev::lift();
		if (!first)
			act += Boss::log( bus, Debug
					, "InitialRebalancer: %s"
					, msg.str().c_str()
					);

		/* Nothing to do.  */
		if (sources_total.empty())
			return act;

		return std::move(act) + filter_sources();
	}

	/* Reject sources that have already spent too much on rebalances.  */
	Ev::Io<void> filter_sources() {
		/* Data about a potential source.  */
		struct SourceInfo {
			Ln::NodeId source;
			Ln::Amount total;
			Ln::Amount in_expenditures;
		};
		auto get_source_info = [this](std::pair< Ln::NodeId
							   , Ln::Amount
							   > source_total) {
			return expense_rr.execute(Msg::RequestEarningsInfo{
				nullptr, source_total.first
			}).then([source_total](Msg::ResponseEarningsInfo rsp) {
				auto source = source_total.first;
				auto total = source_total.second;
				return Ev::lift(SourceInfo{
					source, total, rsp.in_expenditures
				});
			});
		};
		return Ev::map( get_source_info
			      , std::move(sources_total)
			      ).then([this](std::vector<SourceInfo> source_infos) {
			auto act = Ev::lift();
			auto new_sources = std::vector<Ln::NodeId>();
			for (auto const& si : source_infos) {
				auto source = si.source;
				auto total = si.total;
				auto in_expenditures = si.in_expenditures;

				auto limit = (total * max_in_expenditures_percent) / 100.0;

				if (in_expenditures > limit)
					act += Boss::log( bus, Debug
							, "InitialRebalancer: Will not "
							  "rebalance from %s, we already "
							  "spent %s on it, limit is %s."
							, std::string(source)
								.c_str()
							, Util::stringify(in_expenditures)
								.c_str()
							, Util::stringify(limit)
								.c_str()
							);
				else
					new_sources.push_back(source);
			}

			sources = std::move(new_sources);

			return std::move(act) + assign_destinations();
		});
	}

	Ev::Io<void> assign_destinations() {
		for (auto& s : sources) {
			if (destinations.empty())
				break;

			auto sampler = Stats::ReservoirSampler<Ln::NodeId>(1);
			for (auto& d : destinations) {
				auto& info = d.second;
				sampler.add( d.first
					   , info.receivable / info.total
					   , Boss::random_engine
					   );
			}

			auto dest = std::move(sampler).finalize()[0];
			plan.push_back(std::make_pair(s, dest));
		}

		return execute_plan();
	}

	Ev::Io<void> execute_plan() {
		auto act = Ev::lift();
		for (auto& p : plan) {
			auto source = p.first;
			auto destination = p.second;
			auto s_info = info[source];
			auto d_info = info[destination];

			auto max_send = s_info.spendable / 2.0;

			auto max_dest = d_info.total * ( ( spendable_percent
							 - dest_gap_percent
							 )
						       / 100.0
						       );
			auto max_receive = max_dest - d_info.spendable;

			auto amount = max_send;
			if (amount > max_receive)
				amount = max_receive;

			if (amount == Ln::Amount::sat(0))
				continue;

			auto it = current_sources.find(source);
			if (it != current_sources.end()) {
				act += Boss::log( bus, Debug
						, "InitialRebalancer: %s currently "
						  "rebalancing, will not rebalance further."
						, std::string(source).c_str()
						);
				continue;
			}
			current_sources.insert(source);

			act += Boss::log( bus, Debug
					, "InitialRebalancer: %s --> %s --> %s"
					, std::string(source).c_str()
					, std::string(amount).c_str()
					, std::string(destination).c_str()
					);
			/* Since move_funds is performed concurrently, we
			 * keep our self alive, otherwise we could be
			 * deleted before move_funds completes.
			 */
			act += Boss::concurrent(move_funds( source
							  , destination
							  , amount
							  /* Keep alive!  */
							  , shared_from_this()
							  ));
		}
		return act;
	}

	Ev::Io<void> move_funds( Ln::NodeId const& source
			       , Ln::NodeId const& destination
			       , Ln::Amount amount
			       , std::shared_ptr<Impl> self
			       ) {
		auto moved = std::make_shared<Msg::ResponseMoveFunds>();

		auto this_rebalance_fee = amount
					* (rebalance_fee_percent / 100.0)
					;
		if (this_rebalance_fee < min_rebalance_fee)
			this_rebalance_fee = min_rebalance_fee;

		return move_rr.execute(Msg::RequestMoveFunds{
			nullptr, source, destination, amount,
			this_rebalance_fee
		}).then([this, source, moved
			](Msg::ResponseMoveFunds move_result) {
			*moved = move_result;

			auto it = current_sources.find(source);
			assert(it != current_sources.end());
			current_sources.erase(it);
			return Ev::lift();

			/* `self` is used below in order to ensure that
			 * we are still alive after requesting the
			 * transfer of funds.
			 */
		}).then([self, source, moved, destination
			]() {
			auto amount = moved->amount_moved;
			auto fee = moved->fee_spent;
			return Boss::log( self->bus, Debug
					, "InitialRebalancer: "
					  "Moved %s -> "
					  "%s (fee: %s) -> "
					  "%s"
					, std::string(source).c_str()
					, std::string(amount).c_str()
					, std::string(fee).c_str()
					, std::string(destination).c_str()
					);
		});
	}

public:
	Impl( S::Bus& bus_
	    , Boss::Mod::ConstructedListpeers const& peers_
	    , MoveRR& move_rr_
	    , ExpenseRR& expense_rr_
	    , std::set<Ln::NodeId>& current_sources_
	    , std::set<Ln::NodeId> const& unmanaged_
	    ) : bus(bus_), peers(peers_)
	      , move_rr(move_rr_)
	      , expense_rr(expense_rr_)
	      , current_sources(current_sources_)
	      , unmanaged(unmanaged_)
	      { }
	/* Make sure a shared pointer exists, since core_run uses
	 * shared_from_this.
	 */
	static
	Ev::Io<void> run(std::shared_ptr<Impl> self) {
		return self->core_run().then([self]() {
			return Ev::lift();
		});
	}
};

InitialRebalancer::Impl::Run::Run( S::Bus& bus
				 , Boss::Mod::ConstructedListpeers const& peers
				 , MoveRR& move_rr, ExpenseRR& expense_rr
				 , std::set<Ln::NodeId>& current_sources
				 , std::set<Ln::NodeId> const& unmanaged
				 ) : pimpl(std::make_shared<Impl>( bus, peers
								 , move_rr, expense_rr
								 , current_sources
								 , unmanaged
								 ))
				   { }
Ev::Io<void> InitialRebalancer::Impl::Run::run() {
	return Impl::run(pimpl);
}

InitialRebalancer::InitialRebalancer(InitialRebalancer&&) =default;
InitialRebalancer::~InitialRebalancer() =default;

InitialRebalancer::InitialRebalancer(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
