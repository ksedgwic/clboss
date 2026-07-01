#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/FundsMover/Claimer.hpp"
#include"Boss/Mod/FundsMover/Main.hpp"
#include"Boss/Mod/FundsMover/Runner.hpp"
#include"Boss/Mod/FundsMover/create_label.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/RebalanceUnmanagerProxy.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
#include"Boss/Msg/ProvideDeletablePaymentLabelFilter.hpp"
#include"Boss/Msg/RequestAskreneUpdates.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseAskreneUpdates.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
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
	/* Shared request/response to Boss::Mod::AskreneUpdates for the
	 * still-fresh learned updates each attempt projects into its own
	 * private askrene layer.  Held here (module lifetime) and passed by
	 * reference into each Runner/Attempter, which are short-lived.  */
	Boss::ModG::ReqResp< Msg::RequestAskreneUpdates
			   , Msg::ResponseAskreneUpdates
			   > updates_rr;

	/* Cutoff (seconds) for askrene-age on the clboss layer.  Dynamic
	 * via clboss-classic-layer-age-secs; default 43200 (12h).  See
	 * age_clboss_layer() for the aging mechanics and rationale. */
	std::uint64_t aging_window_secs = std::uint64_t(43200);

	/* Minimum fee budget, as ppm of the moved amount, worth attempting
	 * a rebalance at.  A requested move whose fee_budget/amount is below
	 * this is declined up front -- no Runner, no getroutes, no split-
	 * retry storm -- because classic rebalances essentially never clear
	 * below ~50 ppm: the high-traffic drains get budgeted near 35 ppm and
	 * deliver nothing while saturating askrene.  Dynamic via
	 * clboss-min-rebalance-ppm; default 50.  Set to 0 to disable the gate
	 * and attempt every requested move. */
	std::uint64_t min_rebalance_ppm = std::uint64_t(50);

	/* Minimum askrene route success probability (ppm) below which a
	 * found rebalance route is NOT sent.  askrene returns a
	 * probability_ppm with every route; a route scored below this floor
	 * is declined (the attempt fails and the Runner splits to a smaller,
	 * more-probable amount) instead of sending a route that will almost
	 * certainly 204.  Dynamic via clboss-min-rebalance-prob-ppm; default
	 * 500000 (refuse routes askrene scores below ~50% likely; 0 disables).
	 * Snapshotted into each Runner/Attempter. */
	std::uint64_t min_prob_ppm = std::uint64_t(500000);

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
				"<secs>`.  Default 43200 (12h); the xrebalance "
				"layer has the analogous "
				"clboss-xrebalance-age-secs.",
				/* dynamic = */ true
			})
			+ bus.raise(Msg::ManifestOption{
				"clboss-min-rebalance-ppm",
				Msg::OptionType_Int,
				Json::Out::direct(min_rebalance_ppm),
				"Minimum fee budget, in ppm of the moved amount, "
				"worth attempting a rebalance at.  A move whose "
				"requested fee_budget/amount is below this is "
				"declined immediately -- no route solve, no "
				"split-retry storm -- because classic rebalances "
				"essentially never succeed below this rate.  "
				"Dynamic: settable at runtime via `lightning-cli "
				"setconfig clboss-min-rebalance-ppm <ppm>`.  "
				"Default 50; set to 0 to disable (attempt every "
				"requested move).",
				/* dynamic = */ true
			})
			+ bus.raise(Msg::ManifestOption{
				"clboss-min-rebalance-prob-ppm",
				Msg::OptionType_Int,
				Json::Out::direct(min_prob_ppm),
				"Minimum askrene success probability, in ppm, for a "
				"found route to be sent.  askrene returns a "
				"probability_ppm with every route (its estimate that "
				"the liquidity is actually there); a route below this "
				"floor is not sent -- the attempt fails and the move "
				"is split to a smaller, more-probable amount.  This "
				"stops paying for routes askrene already expects to "
				"fail (a sendpay 204).  Dynamic: settable at runtime "
				"via `lightning-cli setconfig "
				"clboss-min-rebalance-prob-ppm <ppm>`.  Default "
				"500000 (refuse routes scored below ~50% likely); "
				"set to 0 to disable (send every route askrene "
				"returns).",
				/* dynamic = */ true
			});
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != "clboss-classic-layer-age-secs")
				return Ev::lift();
			/* Number at startup, string via setconfig -- the same
			 * dual encoding the xrebalance options handle. */
			/* Signed so a negative value is rejected below
			 * rather than wrapping to a huge unsigned. */
			long long secs = 0;
			try {
				if (o.value.is_number()) {
					secs = static_cast<long long>(double(o.value));
				} else if (o.value.is_string()) {
					secs = std::stoll(std::string(o.value));
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
			if (secs <= 0) {
				return Boss::log( bus, Warn
						, "FundsMover: clboss-classic-"
						  "layer-age-secs: must be > 0; "
						  "keeping %" PRIu64 "."
						, aging_window_secs
						);
			}
			aging_window_secs = std::uint64_t(secs);
			/* Keep the AskreneLayer inform-coalescing bucket a
			 * fixed fraction of the aging window (see
			 * set_aging_window_secs); tracks setconfig live. */
			Boss::Mod::AskreneLayer::set_aging_window_secs(
				aging_window_secs
			);
			return Boss::log( bus, Info
					, "FundsMover: clboss layer aging "
					  "window = %" PRIu64 " seconds"
					, aging_window_secs
					);
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != "clboss-min-rebalance-ppm")
				return Ev::lift();
			/* Number at startup, string via setconfig -- the same
			 * dual encoding clboss-classic-layer-age-secs handles.
			 * Signed so a negative value is rejected below rather
			 * than wrapping to a huge unsigned. */
			long long ppm = 0;
			try {
				if (o.value.is_number()) {
					ppm = static_cast<long long>(double(o.value));
				} else if (o.value.is_string()) {
					ppm = std::stoll(std::string(o.value));
				} else {
					return Boss::log( bus, Warn
							, "FundsMover: clboss-min-"
							  "rebalance-ppm: unsupported "
							  "value type; keeping %"
							  PRIu64 "."
							, min_rebalance_ppm
							);
				}
			} catch (std::exception const& e) {
				return Boss::log( bus, Warn
						, "FundsMover: clboss-min-rebalance-"
						  "ppm: parse error '%s'; keeping %"
						  PRIu64 "."
						, e.what()
						, min_rebalance_ppm
						);
			}
			if (ppm < 0) {
				return Boss::log( bus, Warn
						, "FundsMover: clboss-min-rebalance-"
						  "ppm: must be >= 0; keeping %"
						  PRIu64 "."
						, min_rebalance_ppm
						);
			}
			min_rebalance_ppm = std::uint64_t(ppm);
			return Boss::log( bus, Info
					, "FundsMover: min rebalance budget = %"
					  PRIu64 " ppm"
					, min_rebalance_ppm
					);
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != "clboss-min-rebalance-prob-ppm")
				return Ev::lift();
			/* Number at startup, string via setconfig -- the same
			 * dual encoding the other dynamic options handle.
			 * Signed so a negative value is rejected below rather
			 * than wrapping to a huge unsigned. */
			long long ppm = 0;
			try {
				if (o.value.is_number()) {
					ppm = static_cast<long long>(double(o.value));
				} else if (o.value.is_string()) {
					ppm = std::stoll(std::string(o.value));
				} else {
					return Boss::log( bus, Warn
							, "FundsMover: clboss-min-"
							  "rebalance-prob-ppm: "
							  "unsupported value type; "
							  "keeping %" PRIu64 "."
							, min_prob_ppm
							);
				}
			} catch (std::exception const& e) {
				return Boss::log( bus, Warn
						, "FundsMover: clboss-min-rebalance-"
						  "prob-ppm: parse error '%s'; "
						  "keeping %" PRIu64 "."
						, e.what()
						, min_prob_ppm
						);
			}
			if (ppm < 0) {
				return Boss::log( bus, Warn
						, "FundsMover: clboss-min-rebalance-"
						  "prob-ppm: must be >= 0; keeping %"
						  PRIu64 "."
						, min_prob_ppm
						);
			}
			min_prob_ppm = std::uint64_t(ppm);
			return Boss::log( bus, Info
					, "FundsMover: min rebalance route "
					  "probability = %" PRIu64 " ppm"
					, min_prob_ppm
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
				/* Decline a rebalance whose fee budget is below
				 * clboss-min-rebalance-ppm: classic rebalances
				 * essentially never clear below this rate, so skip
				 * the whole Runner / getroutes / split-retry
				 * machinery and emit the zero ResponseMoveFunds
				 * that Runner::finish() would have produced after
				 * giving up.  Cross-multiplied to avoid a divide:
				 * fee_budget/amount < min_ppm/1e6 iff
				 * fee_budget*1e6 < min_ppm*amount.  min_ppm == 0
				 * disables the gate (the test is never true). */
				if ( min_rebalance_ppm > 0
				  && double(msg->fee_budget.to_msat()) * 1000000.0
				     < double(min_rebalance_ppm)
				       * double(msg->amount.to_msat())
				   ) {
					auto src_pfx =
						std::string(msg->source).substr(0, 8);
					auto dst_pfx =
						std::string(msg->destination).substr(0, 8);
					return Boss::log( bus, Debug
							, "FundsMover: not moving %s "
							  "from %s... to %s... -- fee "
							  "budget %s is below clboss-min-"
							  "rebalance-ppm=%" PRIu64 "; "
							  "never clears this cheap."
							, std::string(msg->amount).c_str()
							, src_pfx.c_str()
							, dst_pfx.c_str()
							, std::string(msg->fee_budget)
								.c_str()
							, min_rebalance_ppm
							)
					     + bus.raise(Msg::ResponseMoveFunds{
							msg->requester,
							Ln::Amount::sat(0),
							Ln::Amount::sat(0),
							msg->source,
							msg->destination
						});
				}
				auto runner = Runner::create( bus
							    , *rpc
							    , self_id
							    , claimer
							    , *msg
							    , min_prob_ppm
							    , updates_rr
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

	/* Age out stale entries in the clboss layer.
	 *
	 * askrene-age removes timestamped entries older than the
	 * cutoff: the askrene-inform-channel constraints (and the
	 * channel/node biases) that FundsMover's per-attempt
	 * 204-feedback writes into the layer.  It does NOT touch
	 * disabled_nodes or channel_update overrides -- those carry
	 * no aging timestamp that askrene-age consults, so this pass
	 * leaves them as-is.
	 *
	 * Window: clboss-classic-layer-age-secs (dynamic; default
	 * 43200 = 12h).  FundsMover's 204-feedback (policy refreshes
	 * parsed from BOLT 04 onion errors, capacity signals from
	 * 0x1007) lands in the layer continuously and goes stale on
	 * minutes-to-hours timescales, so the window trades policy
	 * freshness against breadth retention: too short and the
	 * learned per-direction constraints never accumulate, since
	 * each must be re-learned from a fresh 204.  12h is the value
	 * validated in production.
	 *
	 * The self_id self-exclude (which keeps askrene-getroutes
	 * from routing back through us as a middle hop) is written
	 * ONCE at startup in create_clboss_layer(), gated by
	 * is_node_disabled().  It is deliberately NOT refreshed here:
	 * askrene-age never removes disabled_nodes, so the single
	 * startup entry persists for the life of the layer.  (An
	 * earlier version re-wrote disable_node(self_id) on every
	 * aging pass, on the mistaken belief that askrene-age would
	 * expire it; since it does not, and layer_add_disabled_node
	 * does not dedup, that only accumulated duplicate self
	 * entries.)
	 *
	 * RpcError is swallowed for graceful degradation; the
	 * .catching block below handles aging-side errors.
	 */
	Ev::Io<void> age_clboss_layer() {
		return Ev::lift().then([this]() {
			auto now_secs = std::uint64_t(std::time(nullptr));
			/* Clamp: a misconfigured huge age window must not
			 * underflow the cutoff and wipe the whole layer. */
			auto cutoff = aging_window_secs >= now_secs
				    ? std::uint64_t(0)
				    : now_secs - aging_window_secs;
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
			   , updates_rr(bus_)
			   { start(); }
};

Main::Main(Main&&) =default;
Main::~Main() =default;

Main::Main(S::Bus& bus) : pimpl(Util::make_unique<Impl>(bus)) { }

}}}
