#ifndef BOSS_MOD_ASKRENELAYER_HPP_
#define BOSS_MOD_ASKRENELAYER_HPP_

#include"Ev/Io.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include<cstdint>
#include<string>

namespace Boss { namespace Mod { class Rpc; } }

namespace Boss { namespace Mod { namespace AskreneLayer {

/* The node disables (NODE-level failures) and channel_update policy
 * overrides both rebalancers learn are NOT held in a shared askrene layer.
 * askrene-age never removes them (they carry no timestamp, unlike the
 * inform-channel constraints), so instead of accumulating forever in a
 * layer they are kept in CLBOSS's own store, Boss::Mod::AskreneUpdates,
 * which ages them and projects the still-fresh ones into a private,
 * per-request layer for each getroutes.  See that module for the scheme.
 */

/* Name of the persistent askrene layer that CLBOSS subsystems
 * write failure-feedback and (optionally) success-observations
 * into.  Following the xpay convention -- the layer is named
 * after the plugin that owns it.  All CLBOSS writes go to this
 * layer; downstream getroutes calls that include it in their
 * layers array benefit from the accumulated knowledge.
 */
extern std::string const clboss_layer_name;

/* Name of the persistent askrene layer used by the xrebalance
 * family of code paths (currently just the manual
 * `clboss-xmovefunds` RPC; eventually shared with the periodic
 * xrebalance Layer 3 and JIT xrebalance Layer 4 once they land).
 *
 * Distinct from clboss_layer_name so the two implementations'
 * accumulated knowledge does not commingle while both run side
 * by side during the FundsMover -> xrebalance transition.
 */
extern std::string const xrebalance_layer_name;

/* Tell askrene that a directed channel could not push at least
 * the given amount recently.  Future getroutes calls that
 * include the clboss layer will treat this as an upper-bound
 * constraint and steer around it.  Non-fatal on RpcError: if
 * the layer is missing (e.g. CLN < v24.11 where askrene layers
 * are unavailable) the call silently completes -- failure-mode
 * is degraded learning, not crashed caller.
 */
Ev::Io<void> inform_channel_constrained( Boss::Mod::Rpc& rpc
				       , std::string const& layer
				       , Ln::Scid scid
				       , std::uint32_t direction
				       , Ln::Amount amount
				       );

/* Tell askrene that a directed channel successfully pushed at
 * least the given amount recently.  Future getroutes calls
 * that include the layer will treat this as a lower-bound on
 * the channel's capacity (min=amount, max=NULL).  Non-fatal
 * on RpcError, same rationale as inform_channel_constrained.
 *
 * Naming note: this maps to askrene's `inform=unconstrained`
 * mode (which means "no upper-bound; minimum is amount").
 * Askrene also has an `inform=succeeded` mode but as of
 * v26.06 that branch is a no-op stub in askrene.c
 * (`FIXME: We could do something useful here!`).  xpay uses
 * `inform=unconstrained` for the same after-success
 * lower-bound-raise pattern (see plugins/xpay/xpay.c
 * around `"We learned something about prior nodes"`).
 */
Ev::Io<void> inform_channel_unconstrained( Boss::Mod::Rpc& rpc
					 , std::string const& layer
					 , Ln::Scid scid
					 , std::uint32_t direction
					 , Ln::Amount amount
					 );

/* Tell askrene to avoid the given node entirely for routes
 * that include this layer.  Used for NODE-level onion failures
 * (failcode bit 0x2000) and for unparsable-onion fallback,
 * where we cannot pin the failure to a specific channel.
 * Non-fatal on RpcError.
 *
 * Note: askrene's layer_add_disabled_node appends without
 * deduping; repeated calls accumulate identical entries.
 * Callers that want once-per-restart semantics (e.g. the
 * FundsMover self-exclude initialization) should gate the
 * call on is_node_disabled() below.  Callers that record a
 * fresh failure observation per call (e.g. the Attempter's
 * 0x2000 NODE-level feedback) should NOT dedup -- each call
 * is meaningful independent evidence.
 */
Ev::Io<void> disable_node( Boss::Mod::Rpc& rpc
			 , std::string const& layer
			 , Ln::NodeId node
			 );

/* Check whether the given node already appears in the layer's
 * disabled_nodes set.  Wraps the `askrene-listlayers` RPC and
 * iterates the resulting disabled_nodes array.
 *
 * Returns false on RpcError or malformed response (older CLN
 * without askrene-listlayers, etc.).  False is the
 * conservative answer: it lets the caller fall through to a
 * disable_node call which itself swallows RpcError, so the
 * worst-case behaviour matches the previous unconditional-
 * disable_node design (potential duplicate accumulation in
 * degraded mode).
 */
Ev::Io<bool> is_node_disabled( Boss::Mod::Rpc& rpc
			     , std::string const& layer
			     , Ln::NodeId node
			     );

/* Apply a fresh channel-policy override to the given layer.
 * Maps to askrene's `askrene-update-channel` command, which
 * overrides the gossmap-derived policy for that one channel-
 * direction inside the layer.  Subsequent getroutes calls that
 * include the layer see the override instead of the
 * (possibly-stale) gossip values.
 *
 * Intended use: FundsMover/Attempter on a sendpay 204 with a
 * failcode that carries a channel_update payload
 * (0x1007/0x100b/0x100c/0x100d/0x100e).  Parsing the embedded
 * channel_update yields the forwarder's CURRENT policy, which is
 * fed back here so the next within-Runner attempt uses the
 * actual fee/CLTV/min/max rather than what gossmap has cached.
 * Mirrors xpay's process_channel_update_from_onion_error in
 * cln/plugins/xpay/xpay.c.
 *
 * Non-fatal on RpcError: same degraded-learning posture as
 * inform_channel_constrained -- if the layer is missing or
 * askrene-update-channel is unavailable (CLN < v24.11), the
 * call silently completes and routing simply does not benefit
 * from the refreshed policy.
 */
Ev::Io<void> update_channel( Boss::Mod::Rpc& rpc
			   , std::string const& layer
			   , Ln::Scid scid
			   , std::uint32_t direction
			   , bool enabled
			   , Ln::Amount htlc_minimum_msat
			   , Ln::Amount htlc_maximum_msat
			   , Ln::Amount fee_base_msat
			   , std::uint32_t fee_proportional_millionths
			   , std::uint16_t cltv_expiry_delta
			   );

/* --- inform-channel write coalescing -------------------------------
 *
 * askrene's get_constraints folds every constraint for a given
 * (layer, scid-dir) down to a single tightest [min, max] at query
 * time, so a stream of repeated inform-channel writes for the same dir
 * only bloats the layer: hot rebalance corridors accreted hundreds-to-
 * thousands of dominated min_msat copies, every one of which askrene
 * must re-fold on each getroutes through that dir.  inform_channel
 * coalesces them -- per (layer, scid-dir, inform-kind) it keeps the
 * tightest bound seen in the current time bucket and emits a write only
 * when a new bucket opens (a keep-alive against the layer aging) or the
 * bound tightens.  Dropping a dominated write is lossless: it is exactly
 * what get_constraints discards.
 *
 * InformObs is the per-key state; inform_coalesce_emit is the pure
 * decision, exposed here so it can be unit-tested directly.
 */
struct InformObs {
	/* Time bucket (Ev::now() / window) the tightest bound was last
	 * emitted in. */
	std::uint64_t bucket;
	/* That tightest bound, in msat: the highest min for a lower-bound
	 * (unconstrained) kind, the lowest max for an upper-bound
	 * (constrained) kind. */
	std::uint64_t tightest_msat;
};

/* Decide whether a new observation should be written through to
 * askrene.  prior is the last emitted state for this
 * (layer, scid-dir, kind), or nullptr if none.  is_lower_bound is true
 * for the unconstrained/min kind, false for the constrained/max kind.
 * Emit on no prior, on a new bucket, or on a tightening within the
 * same bucket. */
bool inform_coalesce_emit( InformObs const* prior
			 , std::uint64_t bucket
			 , std::uint64_t amount_msat
			 , bool is_lower_bound
			 );

/* Set the inform-channel coalescing bucket length from the layer aging
 * window: the bucket is a fixed fraction of aging_secs, so the
 * once-per-bucket keep-alive always refreshes a constraint before it
 * ages out.  Wired from FundsMover's clboss-classic-layer-age-secs
 * handler so the window tracks the aging window live. */
void set_aging_window_secs(std::uint64_t aging_secs);

}}}

#endif /* !defined(BOSS_MOD_ASKRENELAYER_HPP_) */
