#include"Boss/Mod/JitRebalancer.hpp"
#include"Boss/ModG/RebalanceModeProxy.hpp"
#include"Boss/ModG/RebalanceUnmanagerProxy.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/ModG/RpcProxy.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/ReleaseHtlcAccepted.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/RequestPeerFromScid.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/Msg/ResponsePeerFromScid.hpp"
#include"Boss/Msg/ProvideHtlcAcceptedDeferrer.hpp"
#include"Boss/Msg/SolicitHtlcAcceptedDeferrer.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/HtlcAccepted.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Stats/ReservoirSampler.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<map>

namespace {

/* Weight of an HTLC output on the commitment tx.
 * The HTLC needs to be paid for, so also factor that in our spendable
 * computation.
 */
auto constexpr htlc_weight = std::size_t(172);

/* Up to how many percent of the total earnings from the outgoing
 * channel do we allow a rebalancing fee to that channel.
 *
 * See https://lists.ozlabs.org/pipermail/c-lightning/2019-July/000160.html
 * for the attack this prevents.
 *
 * The below is how much the above attack can extract from us.
 * Our hope is that the attack is not mounted often enough that the
 * below extractable amount can be taken from us.
 */
auto constexpr max_fee_percent = double(25.0);

/* Up to how much rebalancing fee we allow "for free" before we insist on
 * the rebalancing fee being less than the above percentage of the fee already
 * earned.
 *
 * Without this, a fresh node would not ever JIT-rebalance.
 *
 * This is exploitable by attackers (via the attack linked above) by
 * starting up new channels with us, but since the below limit is fairly
 * low, they can only steal this "free fee" and not more than that.
 * Our expectation is that attackers will spend much, much more on
 * channel opening fees than the few sats we give them for free for
 * rebalancing, while this small amount should be sufficient for at least
 * one or two rebalances to "seed" our node with actual successful forwards.
 */
auto const free_fee = Ln::Amount::sat(10);

/* Maximum limit for costs of a *single* rebalance, in parts per million.  */
auto constexpr default_max_rebalance_fee_ppm = std::uint32_t(1000);
auto const min_rebalance_fee = Ln::Amount::sat(5);

std::string stringify_cid(Ln::CommandId const& id) {
	auto rv = std::string();
	id.cmatch([&](std::uint64_t nid) {
		rv = Util::stringify(nid);
	}, [&](std::string const& sid) {
		rv = sid;
	});
	return rv;
}

}

namespace Boss { namespace Mod {

class JitRebalancer::Impl {
private:
	S::Bus& bus;
	Boss::ModG::RpcProxy rpc;

	typedef
	Boss::ModG::ReqResp< Msg::RequestEarningsInfo
			   , Msg::ResponseEarningsInfo
			   > EarningsInfoRR;
	typedef
	Boss::ModG::ReqResp< Msg::RequestMoveFunds
			   , Msg::ResponseMoveFunds
			   > MoveFundsRR;
	typedef
	Boss::ModG::ReqResp< Msg::RequestPeerFromScid
			   , Msg::ResponsePeerFromScid
			   > PeerFromScidRR;
	EarningsInfoRR earnings_info_rr;
	MoveFundsRR move_funds_rr;
	PeerFromScidRR peer_from_scid_rr;

        ModG::RebalanceUnmanagerProxy unmanager;
        ModG::RebalanceModeProxy mode_proxy;
        std::uint32_t max_rebalance_fee_ppm;

        void start() {
                max_rebalance_fee_ppm = default_max_rebalance_fee_ppm;

                bus.subscribe<Msg::Manifestation
                             >([this](Msg::Manifestation const&) {
                        return bus.raise(Msg::ManifestOption{
                                "clboss-max-rebalance-fee-ppm",
                                Msg::OptionType_Int,
                                Json::Out::direct(max_rebalance_fee_ppm),
                                "Maximum fee in ppm for a single rebalance."
                        });
                });
                bus.subscribe<Msg::Option
                             >([this](Msg::Option const& o) {
                        if (o.name != "clboss-max-rebalance-fee-ppm")
                                return Ev::lift();
			auto ppm = std::uint32_t(double(o.value));
			max_rebalance_fee_ppm = ppm;
                        return Boss::log( bus, Info,
                                         "JitRebalancer: max fee set to %u ppm",
                                         (unsigned)ppm );
                });

                bus.subscribe<Msg::SolicitHtlcAcceptedDeferrer
                             >([this
                               ](Msg::SolicitHtlcAcceptedDeferrer const&) {
                        auto f = [this](Ln::HtlcAccepted::Request const& req) {
                                return htlc_accepted(req);
                        };
                        return bus.raise(Msg::ProvideHtlcAcceptedDeferrer{
                                std::move(f)
                        });
                });
        }

	Ev::Io<bool>
	htlc_accepted(Ln::HtlcAccepted::Request const& req) {
		/* Is it a forward?  */
		if (!req.next_channel)
			return Ev::lift(false);

		/* Self-gate on the rebalance mode: JIT rebalancing only runs
		 * in the "classic" track.  In other modes do not defer the
		 * HTLC, so forwarding is never delayed by the JIT logic.  */
		return mode_proxy.get_mode().then([this, req](RebalanceMode m) {
			if (m != RebalanceMode::classic)
				return Ev::lift(false);

			/* Get it from the table.  */
			return peer_from_scid_rr.execute(Msg::RequestPeerFromScid{
				nullptr, req.next_channel
			}).then([ this
				, req
				](Msg::ResponsePeerFromScid const& r) {
				if (!r.peer)
					return Ev::lift(false);
				return htlc_accepted_cont( r.peer
							 , req.id
							 , req.next_amount
							 );
			});
		});
	}
	Ev::Io<bool>
	htlc_accepted_cont( Ln::NodeId const& node
			  , Ln::CommandId id
			  , Ln::Amount amount
			  ) {
		return unmanager.get_unmanaged().then([ this
						      , node
						      , amount
						      , id
						      ](std::set<Ln::NodeId> const* unmanaged) {
			if (unmanaged->count(node) != 0) {
				return Boss::log( bus, Debug
						, "JitRebalancer: HTLC %s to "
						  "unmanaged node %s, will "
						  "ignore."
						, stringify_cid(id).c_str()
						, Util::stringify(node).c_str()
						).then([]() {
					return Ev::lift(false);
				});
			}
			return Boss::concurrent( check_and_move(node, amount, id)
					       ).then([]() {
				return Ev::lift(true);
			});
		});

	}

	class Run {
	private:
		class Impl;
		std::shared_ptr<Impl> pimpl;
	public:
		Run() =delete;

                Run(S::Bus& bus, Boss::ModG::RpcProxy& rpc
                   , Ln::NodeId const& node
                   , Ln::Amount amount
                   , Ln::CommandId id
                   , EarningsInfoRR& earnings_info_rr
                   , MoveFundsRR& move_funds_rr
                   , ModG::RebalanceUnmanagerProxy& unmanager
                   , std::uint32_t& max_rebalance_fee_ppm
                   );
		Run(Run&&) =default;
		Run(Run const&) =default;
		~Run() =default;

		Ev::Io<void> execute();
	};

	Ev::Io<void>
	check_and_move( Ln::NodeId const& node
		      , Ln::Amount amount
		      , Ln::CommandId id
		      ) {
		return Ev::lift().then([this, node, amount, id]() {
                        auto r = Run( bus, rpc, node, amount, id
                                    , earnings_info_rr, move_funds_rr
                                    , unmanager, max_rebalance_fee_ppm
                                    );
			return r.execute();
		});
	}

public:
	Impl( S::Bus& bus_
	    ) : bus(bus_)
	      , rpc(bus_)
	      , earnings_info_rr(bus)
	      , move_funds_rr(bus)
	      , peer_from_scid_rr(bus)
              , unmanager(bus)
              , mode_proxy(bus)
              { start(); }
};

/* Yes, what a messy name... */
class JitRebalancer::Impl::Run::Impl {
private:
	S::Bus& bus;
	Boss::ModG::RpcProxy& rpc;
	Ln::NodeId out_node;
	Ln::Amount amount;
	Ln::CommandId id;

	/* Unilateral-close feerate.  */
	std::uint64_t feerate;
	/* Amount available on all nodes.  */
	struct ChannelInfo {
		Ln::Amount to_us;
		Ln::Amount capacity;
	};
	std::map<Ln::NodeId, ChannelInfo> available;
	/* How much should we add to the destination?  */
	Ln::Amount to_move;
	/* Up to how much to pay for *this* rebalance.	*/
	Ln::Amount this_rebalance_fee;

	/* ReqResp to `Boss::Mod::EarningsTracker`.  */
	EarningsInfoRR& earnings_info_rr;
	/* ReqResp to `Boss::Mod::FundsMover`.	*/
	MoveFundsRR& move_funds_rr;
	/* Unmanager proxy.  */
        ModG::RebalanceUnmanagerProxy& unmanager;
        std::uint32_t& max_rebalance_fee_ppm;

	std::set<Ln::NodeId> const* unmanaged;

	/* Thrown to signal that we should release the HTLC.  */
	struct Continue {};
	/* Exceptions are dual to `call-with-current-continuation`,
	 * and one use of `call-with-current-continuation` is to
	 * create "early outs" without having to thread through a lot
	 * of functions.
	 * We simply use thrown objects to implement a sort of
	 * `call-with-current-continuation` to implement early outs.
	 */

	/* Just the information we need from the earnings tracker.  */
	struct Earnings {
		/* These are the "out" earnings and expenditures. */
		Ln::Amount earnings;
		Ln::Amount expenditures;
	};

	Ev::Io<void> core_execute() {
		return Ev::lift().then([this]() {
			return unmanager.get_unmanaged();
		}).then([this](std::set<Ln::NodeId> const* unmanaged_) {
			unmanaged = unmanaged_;

			/* Get the unilateral close feerate.  */
			auto parms = Json::Out()
				.start_array()
					.entry("perkw")
				.end_array()
				;
			return rpc.command("feerates", std::move(parms));
		}).then([this](Jsmn::Object res) {
			feerate = 0;
			try {
				auto data = res["perkw"];
				if (data.has("unilateral_close"))
					feerate = std::uint64_t(double(
						data["unilateral_close"]
					));
			} catch (Jsmn::TypeError const&) {
				return Boss::log( bus, Error
						, "JitRebalancer: Unexpected "
						  "result from feerates: %s"
						, Util::stringify(res).c_str()
						).then([]() {
					throw Continue();
					return Ev::lift();
				});
			}
			return Ev::lift();
		}).then([this]() {

			/* Adjust the amount by the feerate times weight
			 * plus the HTLC.  */
			amount += Ln::Amount::msat(feerate * htlc_weight);

			/* Determine the amounts available.  */
			auto parms = Json::Out::empty_object();
			return rpc.command("listpeerchannels", std::move(parms));
		}).then([this](Jsmn::Object res) {
			try {
				  // auto ps = res["peers"];
				  // for (auto p : ps) {
				auto cs = res["channels"];
				for (auto c : cs) {
					auto to_us = Ln::Amount::sat(0);
					auto capacity = Ln::Amount::sat(0);
					auto peer = Ln::NodeId(std::string(
						c["peer_id"]
					));

					/* Skip peers in the unmanaged
					 * list.  */
					if (unmanaged->count(peer) != 0)
						continue;

					auto state = std::string(
						c["state"]
					);
					if (state != "CHANNELD_NORMAL")
						continue;
					to_us += Ln::Amount::object(
						c["to_us_msat"]
					);
					capacity += Ln::Amount::object(
						c["total_msat"]
					);

					auto& av = available[peer];
					av.to_us += to_us;
					av.capacity += capacity;
				}
			} catch (std::exception const& ex) {
				return Boss::log( bus, Error
						, "JitRebalancer: Unexpected "
						  "result from listpeerchannels: %s: %s"
						, Util::stringify(res).c_str()
						, ex.what()
						).then([]() {
					throw Continue();
					return Ev::lift();
				});
			}
			return Ev::lift();
		}).then([this]() {

			/* Check if it is available.  */
			auto it = available.find(out_node);
			if (it == available.end())
				/* Not in the map?
				 * Continue, this will fail.  */
				throw Continue();

			/* Check if the amount fits.  */
			if (amount <= it->second.to_us)
				/* Will fit, just continue.  */
				return Boss::log( bus, Debug
						, "JitRebalancer: HTLC %s "
						  "of amount %s fits in "
						  "outgoing amount %s."
						, stringify_cid(id).c_str()
						, std::string(amount).c_str()
						, std::string(it->second.to_us)
							.c_str()
						).then([]() {
					throw Continue();
					return Ev::lift();
				});

			/* Target to have the out-node have midway between
			 * the needed amount, and the total capacity of
			 * the channel, to also fund future outgoing forwards
			 * via this channel.
			 */
			auto target = (amount + it->second.capacity) / 2.0;
			to_move = target - it->second.to_us;

			return Boss::log( bus, Debug
					, "JitRebalancer: HTLC %s needs "
					  "amount %s, only %s available "
					  "at %s, want to rebalance %s."
					, stringify_cid(id).c_str()
					, std::string(amount).c_str()
					, std::string(it->second.to_us).c_str()
					, std::string(out_node).c_str()
					, std::string(to_move).c_str()
					);
		}).then([this]() {

			/* Determine how much fee we can use for
			 * rebalancing.	 */
			return get_earnings(out_node);
		}).then([this](Earnings e) {
			/* Total aggregated limit.  */
			auto limit = free_fee
				   + (e.earnings * (max_fee_percent / 100.0))
				   ;
			if (limit < e.expenditures)
				return Boss::log( bus, Debug
						, "JitRebalancer: HTLC %s: "
						  "Cannot rebalance to %s, we "
						  "already spent %s on "
						  "rebalances, limit is %s."
						, stringify_cid(id).c_str()
						, std::string(out_node).c_str()
						, std::string(e.expenditures)
							.c_str()
						, std::string(limit).c_str()
						).then([]() {
					throw Continue();
					return Ev::lift();
				});

                        auto max_rebalance_fee = to_move
                                               * ( double(max_rebalance_fee_ppm)
                                                   / 1000000.0
                                                 );
			if (max_rebalance_fee < min_rebalance_fee)
				max_rebalance_fee = min_rebalance_fee;

			this_rebalance_fee = limit - e.expenditures;
			if (this_rebalance_fee > max_rebalance_fee)
				this_rebalance_fee = max_rebalance_fee;

			/* Now select a source channel.	 */
			auto min_required = to_move
					  + (this_rebalance_fee / 2.0)
					  ;
			auto sampler = Stats::ReservoirSampler<Ln::NodeId>(1);
			for (auto& e : available) {
				auto& candidate = e.first;
				auto& channel = e.second;

				/* The outgoing node is disqualified.  */
				if (candidate == out_node)
					continue;
				/* If the node does not have enough,
				 * skip it.  */
				if (channel.to_us < min_required)
					continue;

				/* Score according to how much it is
				 * balanced in our favor.  */
				sampler.add( candidate
					   , channel.to_us / channel.capacity
					   , Boss::random_engine
					   );
			}
			auto& samples = sampler.get();
			if (samples.size() == 0)
				return Boss::log( bus, Debug
						, "JitRebalancer: HTLC %s: "
						  "No candidates to get funds "
						  "from."
						, stringify_cid(id).c_str()
						).then([]() {
					throw Continue();
					return Ev::lift();
				});
			auto& selected = samples[0];

			return Boss::log( bus, Debug
					, "JitRebalancer: HTLC %s: Move %s "
					  "from %s to %s."
					, stringify_cid(id).c_str()
					, std::string(to_move).c_str()
					, std::string(selected).c_str()
					, std::string(out_node).c_str()
					)
			     + move_funds(selected)
			     ;
		});
	}

	Ev::Io<Earnings> get_earnings(Ln::NodeId const& node) {
		return earnings_info_rr.execute(Msg::RequestEarningsInfo{
			nullptr, node
		}).then([](Msg::ResponseEarningsInfo raw) {
			return Ev::lift(Earnings{
				raw.out_earnings, raw.out_expenditures
			});
		});
	}

	Ev::Io<void> move_funds(Ln::NodeId const& source) {
		return move_funds_rr.execute(Msg::RequestMoveFunds{
			nullptr,
			source, out_node,
			to_move, this_rebalance_fee
		}).then([](Msg::ResponseMoveFunds _) {
			/* Ignore result.  */
			return Ev::lift();
		});
	}

public:
        Impl( S::Bus& bus_
            , Boss::ModG::RpcProxy& rpc_
            , Ln::NodeId const& out_node_
            , Ln::Amount amount_
            , Ln::CommandId id_
            , EarningsInfoRR& earnings_info_rr_
            , MoveFundsRR& move_funds_rr_
            , ModG::RebalanceUnmanagerProxy& unmanager_
            , std::uint32_t& max_rebalance_fee_ppm_
            ) : bus(bus_), rpc(rpc_)
              , out_node(out_node_), amount(amount_), id(id_)
              , earnings_info_rr(earnings_info_rr_)
              , move_funds_rr(move_funds_rr_)
              , unmanager(unmanager_)
              , max_rebalance_fee_ppm(max_rebalance_fee_ppm_)
              { }

	static
	Ev::Io<void> execute(std::shared_ptr<Impl> self) {
		return self->core_execute().catching<Continue
						    >([](Continue const& _) {
			return Ev::lift();
		}).catching<std::exception>([self](std::exception const& e) {
			return Boss::log( self->bus, Error
					, "JitRebalancer: Uncaught error: "
					  "%s"
					, e.what()
					);
		}).then([self]() {
			return self->bus.raise(Msg::ReleaseHtlcAccepted{
				Ln::HtlcAccepted::Response::cont(self->id)
			});
		});
	}
};
JitRebalancer::Impl::Run::Run( S::Bus& bus
			     , Boss::ModG::RpcProxy& rpc
			     , Ln::NodeId const& node
			     , Ln::Amount amount
			     , Ln::CommandId id
			     , EarningsInfoRR& earnings_info_rr
                             , MoveFundsRR& move_funds_rr
                             , ModG::RebalanceUnmanagerProxy& unmanager
                             , std::uint32_t& max_rebalance_fee_ppm
                             )
        : pimpl(std::make_shared<Impl>( bus, rpc, node, amount, id
                                      , earnings_info_rr, move_funds_rr
                                      , unmanager, max_rebalance_fee_ppm
                                      )) { }
Ev::Io<void> JitRebalancer::Impl::Run::execute() {
	return Impl::execute(pimpl);
}

JitRebalancer::JitRebalancer(JitRebalancer&&) =default;
JitRebalancer::~JitRebalancer() =default;

JitRebalancer::JitRebalancer(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
