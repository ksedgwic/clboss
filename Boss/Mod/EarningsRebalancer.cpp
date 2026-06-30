
#include"Boss/Mod/EarningsRebalancer.hpp"
#include"Boss/ModG/RebalanceModeProxy.hpp"
#include"Boss/ModG/RebalanceUnmanagerProxy.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/map.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<algorithm>
#include<inttypes.h>
#include<iterator>
#include<map>
#include<random>

namespace {

/* If we call dist on the random engine, and it comes up 1, we
 * trigger earnings rebalancer.	 */
auto dist = std::uniform_int_distribution<std::size_t>(
	1, 2
);

/* If the spendable amount is below this percent of the channel
 * total, trigger rebalancing *to* the channel.	  */
auto constexpr max_spendable_percent = double(25.0);
/* Gap to prevent sources from becoming equal to the max_spendable_percent.  */
auto constexpr src_gap_percent = double(2.5);
/* Target to get to the destination.  */
auto constexpr dst_target_percent = double(75.0);
/* Maximum fee for a single rebalance in parts per million.  */
auto constexpr default_max_fee_ppm = std::uint32_t(1000);

/* The top percentile (based on earnings - expenditures) that we are going to
 * rebalance.  */
auto constexpr top_rebalancing_percentile = double(20.0);

}

namespace Boss { namespace Mod {

class EarningsRebalancer::Impl {
private:
	S::Bus& bus;

	bool working;

	ModG::ReqResp< Msg::RequestEarningsInfo
		     , Msg::ResponseEarningsInfo
		     > earnings_rr;

	struct BalanceInfo {
		Ln::Amount spendable;
		Ln::Amount receivable;
		Ln::Amount total;
	};
	std::map<Ln::NodeId, BalanceInfo> balances;
	std::map<Ln::NodeId, BalanceInfo> cached_balances;
	struct EarningsInfo {
		/* These can be negative.  */
		std::int64_t in_net_earnings;
		std::int64_t out_net_earnings;
	};
	std::map<Ln::NodeId, EarningsInfo> earnings;

        ModG::RebalanceUnmanagerProxy unmanager;
        ModG::RebalanceModeProxy mode_proxy;
        std::set<Ln::NodeId> const* unmanaged;
        std::uint32_t max_fee_ppm;

        void start() {
                struct SelfTrigger { };

                working = false;
                max_fee_ppm = default_max_fee_ppm;

		bus.subscribe<Msg::TimerRandomHourly
			     >([this](Msg::TimerRandomHourly const& _) {
			if (working)
				return Ev::lift();
			auto dice = dist(Boss::random_engine);
			if (dice != 1)
				return Ev::lift();
			return Ev::concurrent(bus.raise(SelfTrigger{}));
		});
		bus.subscribe<SelfTrigger
			    >([this](SelfTrigger const& _) {
			if (working)
				return Ev::lift();

			/* Self-gate on the rebalance mode: only the
			 * "classic" track runs the EarningsRebalancer.  */
			return mode_proxy.get_mode().then([this](RebalanceMode m) {
				if (m != RebalanceMode::classic)
					return Boss::log( bus, Debug
							, "EarningsRebalancer: "
							  "rebalance mode is not "
							  "\"classic\", skipping."
							);
				if (working)
					return Ev::lift();

				working = true;
				return Boss::log( bus, Debug
						, "EarningsRebalancer: Triggering."
						).then([this]() {
					return run();
				}).then([this]() {
					working = false;

					return Boss::log( bus, Debug
							, "EarningsRebalancer: Finished."
							);
				});
			});
		});

		bus.subscribe<Msg::ListpeersResult
			     >([this](Msg::ListpeersResult const& m) {
			if (m.initial)
				return Ev::lift();
			return update_balances(m.cpeers);
		});

                /* Command to trigger the algorithm for testing and option registration.  */
                bus.subscribe<Msg::Manifestation
                             >([this](Msg::Manifestation const& _) {
                        return bus.raise(Msg::ManifestCommand{
                                        "clboss-earnings-rebalancer",
                                        "",
                                        "Debug command to trigger EarningsRebalancer module.",
                                        false
                                })
                             + bus.raise(Msg::ManifestOption{
                                        "clboss-max-rebalance-fee-ppm",
                                        Msg::OptionType_Int,
                                        Json::Out::direct(max_fee_ppm),
                                        "Maximum fee in ppm for a single rebalance."
                                });
                });
                bus.subscribe<Msg::Option
                             >([this](Msg::Option const& o) {
                        if (o.name != "clboss-max-rebalance-fee-ppm")
                                return Ev::lift();
			auto ppm = std::uint32_t(double(o.value));
			max_fee_ppm = ppm;
                        return Boss::log( bus, Info,
                                         "EarningsRebalancer: max fee set to %u ppm",
                                         (unsigned)ppm );
                });
		bus.subscribe<Msg::CommandRequest
			     >([this](Msg::CommandRequest const& c) {
			if (c.command != "clboss-earnings-rebalancer")
				return Ev::lift();
			return Boss::concurrent(bus.raise(SelfTrigger{}))
			     + bus.raise(Msg::CommandResponse{
					c.id, Json::Out::empty_object()
			       })
			     ;
		});
	}

	Ev::Io<void> update_balances(Boss::Mod::ConstructedListpeers const& peers) {
		auto new_balances = std::map<Ln::NodeId, BalanceInfo>();
		try {
			for (auto p : peers) {
				auto spendable = Ln::Amount::sat(0);
				auto receivable = Ln::Amount::sat(0);
				auto total = Ln::Amount::sat(0);

				auto id = p.first;

				for (auto c : p.second.channels) {
					auto state = std::string(
						c["state"]
					);
					if (state != "CHANNELD_NORMAL")
						continue;
					if ( !c.has("to_us_msat")
					  || !c.has("total_msat")
					  || !c.has("htlcs")
					   )
						continue;

					auto to_us = Ln::Amount::object(
						c["to_us_msat"]
					);
					auto c_total = Ln::Amount::object(
						c["total_msat"]
					);
					auto to_them = c_total - to_us;

					spendable += to_us;
					receivable += to_them;
					total += c_total;
				}

				if (total == Ln::Amount::sat(0))
					continue;

				new_balances[id].spendable = spendable;
				new_balances[id].receivable = receivable;
				new_balances[id].total = total;
			}
		} catch (std::exception const& e) {
			return Boss::log( bus, Error
					  , "EarningsRebalancer:"
					  " Unexpected exception: %s "
					  " handling: %s"
					  , e.what()
					  , Util::stringify(peers).c_str()
				);
		}

		cached_balances = std::move(new_balances);
		return Ev::lift();
	}

	Ev::Io<void> run() {
		return Ev::lift().then([this]() {
			return unmanager.get_unmanaged();
		}).then([this](std::set<Ln::NodeId> const* unmanaged_) {
			unmanaged = unmanaged_;
			/* Copy the cached balances.  */
			balances = cached_balances;
			/* Generate vector of peers.  */
			auto peers = std::vector<Ln::NodeId>();
			std::transform( balances.begin(), balances.end()
				      , std::back_inserter(peers)
				      , [](std::pair< Ln::NodeId
						    , BalanceInfo const
						    > const& e){
				return e.first;
			});
			/* Extract earnings info.  */
			auto f = [this](Ln::NodeId n) {
				return earnings_rr.execute(Msg::RequestEarningsInfo{
					nullptr, n
				}).then([n](Msg::ResponseEarningsInfo inf) {
					return Ev::lift(std::make_pair(n, EarningsInfo{
						std::int64_t(inf.in_earnings.to_msat()) -
						std::int64_t(inf.in_expenditures.to_msat()),
						std::int64_t(inf.out_earnings.to_msat()) -
						std::int64_t(inf.out_expenditures.to_msat()),
					}));
				});
			};
			return Ev::map(std::move(f), std::move(peers));
		}).then([this](std::vector<std::pair<Ln::NodeId, EarningsInfo>> earnings_v) {
			earnings.clear();
			std::copy( earnings_v.begin(), earnings_v.end()
				 , std::inserter(earnings, earnings.begin())
				 );

			auto sources = std::vector<Ln::NodeId>();
			auto destinations = std::vector<Ln::NodeId>();

			for (auto& pb : balances) {
				auto peer = pb.first;
				/* Skip unmanaged nodes.  */
				if (unmanaged->count(peer) != 0)
					continue;
				auto balance = pb.second;
				auto percent = (balance.spendable / balance.total) * 100.0;
				if (percent < max_spendable_percent)
					destinations.push_back(peer);
				else if (percent < max_spendable_percent + src_gap_percent)
					;
				else
					sources.push_back(peer);
			}

			if (destinations.empty() || sources.empty())
				return Boss::log( bus, Debug
						, "EarningsRebalancer: Nothing to do."
						);

			auto num_d = destinations.size();
			auto num_s = sources.size();
			auto num = (num_d > num_s) ? num_s : num_d;

			/* How many in percentile?  */
			auto num_rebalance = std::size_t(
				double(num) * top_rebalancing_percentile / 100.0
			);
			if (num_rebalance == 0)
				num_rebalance = 1;

			/* Sort sources and destinations according to who has been
			 * earning mucho dinero.
			 * Destinations based on their out-earnings, sources based
			 * on their in-earnings.
			 */
			std::sort( destinations.begin(), destinations.end()
				 , [this](Ln::NodeId const& a, Ln::NodeId const& b) {
				return earnings[a].out_net_earnings
				     > earnings[b].out_net_earnings
				     ;
			});
			std::sort( sources.begin(), sources.end()
				 , [this](Ln::NodeId const& a, Ln::NodeId const& b) {
				return earnings[a].in_net_earnings
				     > earnings[b].in_net_earnings
				     ;
			});

			/* Build up the action.	 */
			auto act = Ev::lift();
			for (auto i = std::size_t(0); i < num_rebalance; ++i) {
				auto s = sources[i];
				auto d = destinations[i];

				/* If the destination has negative out earnings, stop.	*/
				auto const& ed = earnings[d];
				auto dest_earnings = ed.out_net_earnings;
				if (dest_earnings <= 0) {
					act += Boss::log( bus, Info
							, "EarningsRebalancer: Node %s has "
							  "bad net income (%" PRIi64
							  "msat), refusing to throw good "
							  "money after bad."
							, Util::stringify(d)
								.c_str()
							, dest_earnings
							);
					/* Since the vector is sorted from highest net
					 * earnings to lowest, the rest of the vector can
					 * be skipped.	*/
					break;
				}

				/* Target to get the destination to.  */
				auto const& bd = balances[d];
				auto dest_target = bd.total * (dst_target_percent / 100.0);
				auto dest_needed = dest_target - bd.spendable;

				/* Determine how much money the source can spend
				 * without going below max_spendable_percent and the
				 * gap.	 */
				auto const& bs = balances[s];
				auto src_min_allowed = bs.total
						     * ( max_spendable_percent
						       + src_gap_percent
						       ) / 100.0;
						     ;
				auto src_budget = bs.spendable - src_min_allowed;

				/* If dest_needed is higher than src_budget, go it
				 * lower.  */
				if (dest_needed > src_budget)
					dest_needed = src_budget;

                                /* Now determine fee budget.  */
                                auto fee_budget = dest_needed * (double(max_fee_ppm) / 1000000.0);
				/* If the millisatoshi amount of fee_budget exceeds
				 * our net earnings at the dest, adjust dest_needed.  */
				if (std::int64_t(fee_budget.to_msat()) > dest_earnings) {
					fee_budget = Ln::Amount::msat(std::uint64_t(
						dest_earnings
					));
					/* Also adjust the amount we are hoping to
					 * transfer downwards.	*/
                                        dest_needed = fee_budget * (1000000.0 / double(max_fee_ppm));
				}

				/* Report and move.  */
				act += Boss::log( bus, Debug
						, "EarningsRebalancer: Move %s -> %s -> %s, "
						  "for maximum fee of %s."
						, Util::stringify(s).c_str()
						, Util::stringify(dest_needed).c_str()
						, Util::stringify(d).c_str()
						, Util::stringify(fee_budget).c_str()
						)
				     + Boss::concurrent(bus.raise(Msg::RequestMoveFunds{
						this, s, d, dest_needed, fee_budget
				       }))
				     ;

			}

			return act;
		});
	}

public:
	Impl() =delete;

	explicit
	Impl(S::Bus& bus_
	    ) : bus(bus_)
	      , earnings_rr(bus_)
	      , unmanager(bus_)
	      , mode_proxy(bus_)
	      { start(); }
};

EarningsRebalancer::EarningsRebalancer(EarningsRebalancer&&) =default;
EarningsRebalancer::~EarningsRebalancer() =default;

EarningsRebalancer::EarningsRebalancer(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
