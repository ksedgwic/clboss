#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/FundsMover/Claimer.hpp"
#include"Boss/Mod/FundsMover/Main.hpp"
#include"Boss/Mod/FundsMover/Runner.hpp"
#include"Boss/Mod/FundsMover/create_label.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/RebalanceUnmanagerProxy.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
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

	/* Cutoff (seconds) for askrene-age on the clboss layer.  Dynamic
	 * via clboss-classic-layer-age-secs; default 3600 (1h).  See
	 * age_clboss_layer() for the aging mechanics and rationale. */
	std::uint64_t aging_window_secs = std::uint64_t(3600);

	void start() {
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			rpc = &init.rpc;
			self_id = init.self_id;
			return Boss::concurrent(create_clboss_layer());
		});
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const&) {
			return bus.raise(Msg::ManifestOption{
				"clboss-classic-layer-age-secs",
				Msg::OptionType_Int,
				Json::Out::direct(aging_window_secs),
				"Cutoff (seconds) for periodic askrene-age on "
				"the persistent clboss layer (the classic "
				"rebalancer's failure/transit feedback plus "
				"ActiveProber's probe results).  Entries older "
				"than this are trimmed once per "
				"TimerRandomHourly tick -- the pass cadence is "
				"fixed, so this sets only the expiration age "
				"(values below ~1h do not trim faster).  "
				"Dynamic: settable at runtime via `lightning-cli "
				"setconfig clboss-classic-layer-age-secs "
				"<secs>`.  Default 3600 (1h); the xrebalance "
				"layer has the analogous "
				"clboss-xrebalance-age-secs.",
				/* dynamic = */ true
			});
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != "clboss-classic-layer-age-secs")
				return Ev::lift();
			/* Number at startup, string via setconfig -- the same
			 * dual encoding the xrebalance options handle. */
			auto secs = std::uint64_t(0);
			try {
				if (o.value.is_number()) {
					secs = std::uint64_t(double(o.value));
				} else if (o.value.is_string()) {
					secs = std::stoull(std::string(o.value));
				} else {
					return Boss::log( bus, Warn
							, "FundsMover: clboss-"
							  "classic-layer-age-secs: "
							  "unsupported value type; "
							  "keeping %" PRIu64 "."
							, aging_window_secs
							);
				}
			} catch (std::exception const& e) {
				return Boss::log( bus, Warn
						, "FundsMover: clboss-classic-"
						  "layer-age-secs: parse error "
						  "'%s'; keeping %" PRIu64 "."
						, e.what()
						, aging_window_secs
						);
			}
			if (secs == 0) {
				return Boss::log( bus, Warn
						, "FundsMover: clboss-classic-"
						  "layer-age-secs: must be > 0; "
						  "keeping %" PRIu64 "."
						, aging_window_secs
						);
			}
			aging_window_secs = secs;
			return Boss::log( bus, Info
					, "FundsMover: clboss layer aging "
					  "window = %" PRIu64 " seconds"
					, aging_window_secs
					);
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

	/* Trim clboss-layer entries older than aging_window_secs,
	 * and refresh the self_id disable_node entry just before
	 * aging so the self-loop guard survives the cutoff.
	 *
	 * Aging mechanics: askrene-inform-channel constraints and
	 * askrene-disable-node entries carry per-entry timestamps
	 * that askrene does NOT consult during route scoring; this
	 * explicit aging RPC is the only mechanism that removes
	 * them.  Without periodic aging, transient capacity dips,
	 * one-off node outages, and stale policy refreshes embed
	 * permanently in CLBOSS's routing model.
	 *
	 * Window: clboss-classic-layer-age-secs (dynamic; default 3600
	 * = 1h), matching xpay's own layer aging cadence.  Was
	 * 24h, originally chosen to roughly match the ActiveProber
	 * natural-refresh cadence (one probe per peer per ~24h on
	 * average via RegularActiveProbe).  Reduced to 1h because
	 * FundsMover's per-attempt 204-feedback writes (policy
	 * refreshes parsed from BOLT 04 onion errors, capacity
	 * signals from 0x1007) now land in this layer at a much
	 * higher rate than ActiveProber's writes, and policy
	 * corrections become stale on minutes-to-hours timescales.
	 * Keeping ActiveProber-style 24h capacity memory was less
	 * load-bearing than expected -- FundsMover's own retries
	 * effectively re-discover the same capacity information on
	 * faster timescales, so giving up the long-tail capacity
	 * memory is a cheap price for keeping FundsMover's
	 * own policy-correction writes fresh.
	 *
	 * Self-loop guard refresh: write disable_node(self_id)
	 * unconditionally at the start of every aging cycle, before
	 * the askrene-age RPC runs.  The fresh timestamp puts the
	 * new entry above the cutoff so it survives; any
	 * previous-cycle self_id entries whose timestamps fall
	 * below the cutoff are removed by the same aging pass.
	 * Steady state of disabled_nodes is therefore a small
	 * constant (no unbounded accumulation), and there is no
	 * race window where self_id could be missing from the
	 * layer between aging-removal and re-writing -- the fresh
	 * write completes before the aging RPC fires.
	 *
	 * RpcError swallowed for graceful degradation: the
	 * disable_node wrapper silently no-ops on error, and the
	 * .catching block below handles aging-side errors.
	 */
	Ev::Io<void> age_clboss_layer() {
		return Boss::Mod::AskreneLayer::disable_node(
			*rpc,
			Boss::Mod::AskreneLayer::clboss_layer_name,
			self_id
		).then([this]() {
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
			 * Dedup against existing layer state via
			 * is_node_disabled before writing.  askrene's
			 * layer_add_disabled_node is a pure append, so
			 * an unconditional disable_node on every
			 * FundsMover startup would accumulate duplicate
			 * self entries indefinitely (5 copies observed
			 * in the production clboss layer at the time this
			 * dedup was added).  Functionally harmless
			 * (membership check still works) but unbounded
			 * growth is worth avoiding.
			 *
			 * On any RpcError or malformed response,
			 * is_node_disabled returns false, so we fall
			 * through to the unconditional disable_node
			 * path -- matches the pre-dedup behaviour in
			 * degraded mode.
			 */
			return Boss::Mod::AskreneLayer::is_node_disabled(
				*rpc,
				Boss::Mod::AskreneLayer::clboss_layer_name,
				self_id
			);
		}).then([this](bool already_disabled) {
			if (already_disabled) {
				return Boss::log( bus, Debug
						, "FundsMover: self_id "
						  "already in clboss "
						  "disabled_nodes; "
						  "skipping disable_node"
						);
			}
			return Boss::Mod::AskreneLayer::disable_node(
				*rpc,
				Boss::Mod::AskreneLayer::clboss_layer_name,
				self_id
			)
			+ Boss::log( bus, Debug
				   , "FundsMover: added self_id to "
				     "clboss disabled_nodes"
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
