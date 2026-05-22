#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/FundsMover/Claimer.hpp"
#include"Boss/Mod/FundsMover/Main.hpp"
#include"Boss/Mod/FundsMover/Runner.hpp"
#include"Boss/Mod/FundsMover/create_label.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/RebalanceUnmanagerProxy.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ProvideDeletablePaymentLabelFilter.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/SolicitDeletablePaymentLabelFilter.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<cinttypes>
#include<ctime>

#if HAVE_CONFIG_H
# include"config.h"
#endif

namespace Boss { namespace Mod { namespace FundsMover {

class Main::Impl {
private:
	S::Bus& bus;
	Claimer claimer;
	Boss::Mod::Rpc* rpc;
	Ln::NodeId self_id;
	/* True once create_clboss_layer() has resolved (either by
	 * successfully creating/finding the persistent layer, or by
	 * logging a non-fatal RpcError on older CLN).  Gated on by
	 * wait_for_ready() so that Msg::RequestMoveFunds handling
	 * never tries to use the layer before askrene is told about
	 * it.
	 */
	bool layer_ready;

	Boss::ModG::RebalanceUnmanagerProxy unmanager;

	void start() {
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			rpc = &init.rpc;
			self_id = init.self_id;
			return Boss::concurrent(create_clboss_layer());
		});
		bus.subscribe<Msg::RequestMoveFunds
			     >([this](Msg::RequestMoveFunds const& m) {
			auto msg = std::make_shared<Msg::RequestMoveFunds>(m);
			return wait_for_ready().then([this]() {
				return unmanager.get_unmanaged();
			}).then([this, msg](std::set<Ln::NodeId> const* unmanaged) {
				auto un_s = (unmanaged->count(msg->source) != 0);
				auto un_d = (unmanaged->count(msg->destination) != 0);
				if (un_s || un_d) {
					char const* tpl = nullptr;
					if (un_s && un_d) {
						tpl = "%1$sfrom an unmanaged node %2$s "
						      "to an unmanaged node %3$s%4$s"
						      ;
					} else if (un_s) {
						tpl = "%1$sfrom an unmanaged node %2$s"
						      "%4$s"
						      ;
					} else {
						tpl = "%1$s"
						      "to an unmanaged node %3$s%4$s"
						      ;
					}
					return Boss::log( bus, Error
							, tpl
							, "FundsMover: *SOMETHING* is "
							  "attempting to move funds "
							, Util::stringify(msg->source)
								.c_str()
							, Util::stringify(msg->destination)
								.c_str()
							, ", this may be a bug, "
							  "refusing to move.  "
							  "Contact " PACKAGE_BUGREPORT
							);
				}
				auto runner = Runner::create( bus
							    , *rpc
							    , self_id
							    , claimer
							    , *msg
							    );
				return Runner::start(runner);
			});
		});
		using Msg::ProvideDeletablePaymentLabelFilter;
		using Msg::SolicitDeletablePaymentLabelFilter;
		bus.subscribe<SolicitDeletablePaymentLabelFilter
			     >([this
			       ](SolicitDeletablePaymentLabelFilter const& _) {
			return bus.raise(ProvideDeletablePaymentLabelFilter{
					&is_our_label
			});
		});
		bus.subscribe<Msg::TimerRandomHourly
			     >([this](Msg::TimerRandomHourly const& _) {
			return wait_for_ready().then([this]() {
				return age_clboss_layer();
			});
		});
	}
	/* Gate Msg::RequestMoveFunds handling on FundsMover's
	 * startup-time setup: rpc must have arrived via Msg::Init,
	 * and create_clboss_layer() must have completed (either
	 * successfully or via the logged-RpcError graceful-
	 * degradation path).  Without the layer_ready check there
	 * is a startup window where the first move can run before
	 * askrene-create-layer returns, and any subsequent
	 * askrene-inform-channel / askrene-disable-node writes
	 * would fail with "no such layer".
	 */
	Ev::Io<void> wait_for_ready() {
		return Ev::lift().then([this]() {
			if (!rpc || !layer_ready)
				return Ev::yield() + wait_for_ready();
			return Ev::lift();
		});
	}

	/* Trim clboss-layer entries older than aging_window_secs.
	 * Without this, askrene-inform-channel constraints (and any
	 * disable_node writes) recorded over the lifetime of CLBOSS
	 * would persist forever -- askrene does NOT consult the
	 * per-entry timestamp during route scoring; the timestamp
	 * is only used by this explicit aging RPC to delete entries
	 * with timestamp < cutoff.
	 *
	 * Without aging, transient capacity dips and one-off node
	 * outages would embed permanently in CLBOSS's routing model.
	 *
	 * Cadence is driven by Msg::TimerRandomHourly (~once per
	 * hour with jitter) and the 24h window is chosen to roughly
	 * match the ActiveProber natural-refresh cadence (each peer
	 * is probed once every ~24h on average -- see
	 * RegularActiveProbe's uniform(1, 144) per 10 min dice
	 * roll); the explicit aging is the only freshness mechanism
	 * for FundsMover-written constraints (since FundsMover
	 * routes around them and never re-tests).
	 *
	 * Compared to xpay's own layer (cutoff 1h, fires every
	 * 60s) we are much less aggressive because CLBOSS's
	 * rebalance and probe cadence is on a 24h+ timescale.
	 *
	 * RpcError swallowed for graceful degradation, same pattern
	 * as create_clboss_layer.
	 */
	Ev::Io<void> age_clboss_layer() {
		auto constexpr aging_window_secs = std::uint64_t(86400);
		return Ev::lift().then([this]() {
			auto cutoff = std::uint64_t(std::time(nullptr))
				    - aging_window_secs;
			auto parms = Json::Out()
				.start_object()
					.field("layer",
					       Boss::Mod::AskreneLayer::clboss_layer_name)
					.field("cutoff", cutoff)
				.end_object()
				;
			return rpc->command( "askrene-age"
					   , std::move(parms)
					   );
		}).then([this](Jsmn::Object res) {
			auto removed = std::uint64_t(0);
			if (res.has("num_removed")
			 && res["num_removed"].is_number())
				removed = std::uint64_t(double(res["num_removed"]));
			return Boss::log( bus, Debug
					, "FundsMover: askrene-age (clboss) "
					  "removed %" PRIu64 " stale entries."
					, removed
					);
		}).catching<RpcError>([this](RpcError const& e) {
			/* Distinguish CLN-too-old (askrene-age RPC
			 * missing) from other failures.  The standard
			 * JSON-RPC "method not found" code (-32601) is
			 * the explicit graceful-degradation case
			 * (CLN < v24.11, no askrene plugin); we keep
			 * that at Debug so older nodes don't spam logs
			 * once an hour.  Any other RpcError suggests
			 * something unexpected (transient askrene
			 * problem, layer corruption, etc.) -- promote
			 * to Warn since this aging path is the only
			 * cleanup for FundsMover-written constraints,
			 * and a sustained failure would let stale
			 * pessimism accumulate indefinitely.
			 */
			auto code = int(0);
			if (e.error.has("code") && e.error["code"].is_number())
				code = int(double(e.error["code"]));
			auto is_method_missing = (code == -32601);
			return Boss::log( bus
					, is_method_missing ? Debug : Warn
					, "FundsMover: askrene-age (clboss) "
					  "failed: %s%s"
					, Util::stringify(e.error).c_str()
					, is_method_missing
						? " (RPC missing; aging "
						  "unavailable on this CLN)."
						: " (unexpected; stale "
						  "entries will accumulate "
						  "until next successful "
						  "aging pass)."
					);
		});
	}

	/* Ensure the persistent "clboss" askrene layer exists.  Called
	 * once at startup, fire-and-forget.  Idempotent: when persistent
	 * is true, askrene-create-layer succeeds even if the layer
	 * already exists.  Failures (e.g. CLN < v24.11 where the RPC
	 * does not exist) are logged but non-fatal -- subsequent
	 * getroutes calls will simply not benefit from the
	 * failure-learning layer.
	 */
	Ev::Io<void> create_clboss_layer() {
		return Ev::lift().then([this]() {
			auto parms = Json::Out()
				.start_object()
					.field("layer",
					       Boss::Mod::AskreneLayer::clboss_layer_name)
					.field("persistent", true)
				.end_object()
				;
			return rpc->command( "askrene-create-layer"
					   , std::move(parms)
					   );
		}).then([this](Jsmn::Object _) {
			/* Self-exclude from middle hops by adding our
			 * node_id to the clboss layer's disabled_nodes.
			 * Without this, askrene-getroutes can return
			 * paths that loop through us as a middle node
			 * (us -> source -> us -> destination -> us),
			 * which appear to succeed but actually drain
			 * the destination channel in the wrong direction
			 * while paying fees for zero net progress.
			 *
			 * The legacy getroute call had this protection
			 * via its exclude=[self_id] argument; the
			 * askrene-getroutes API has no inline equivalent,
			 * so the persistent layer's disabled_nodes set
			 * is the only path to express the same intent.
			 *
			 * Idempotency: layer_add_disabled_node appends
			 * without de-dup so successive restarts will
			 * accumulate duplicate self entries, but the
			 * layer_disables_node membership check works
			 * correctly with duplicates -- minor storage
			 * bloat we accept as cheap.
			 */
			return Boss::Mod::AskreneLayer::disable_node(
				*rpc,
				Boss::Mod::AskreneLayer::clboss_layer_name,
				self_id
			);
		}).then([this]() {
			layer_ready = true;
			return Ev::lift();
		}).catching<RpcError>([this](RpcError const& e) {
			/* Mark ready even on failure: degraded mode (no
			 * persistent learning layer) must still allow
			 * rebalances to proceed, otherwise we'd livelock
			 * wait_for_ready() on CLN < v24.11.
			 */
			layer_ready = true;
			return Boss::log( bus, Error
					, "FundsMover: askrene-create-layer "
					  "(clboss) failed: %s; failure-"
					  "learning will not be available."
					, Util::stringify(e.error).c_str()
					);
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl(S::Bus& bus_) : bus(bus_)
			   , claimer(bus_)
			   , rpc(nullptr)
			   , layer_ready(false)
			   , unmanager(bus_)
			   { start(); }
};

Main::Main(Main&&) =default;
Main::~Main() =default;

Main::Main(S::Bus& bus) : pimpl(Util::make_unique<Impl>(bus)) { }

}}}
