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

/* Name of the persistent askrene layer that CLBOSS subsystems
 * write failure-feedback and (optionally) success-observations
 * into.  Following the xpay convention -- the layer is named
 * after the plugin that owns it.  All CLBOSS writes go to this
 * layer; downstream getroutes calls that include it in their
 * layers array benefit from the accumulated knowledge.
 */
extern std::string const clboss_layer_name;

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

}}}

#endif /* !defined(BOSS_MOD_ASKRENELAYER_HPP_) */
