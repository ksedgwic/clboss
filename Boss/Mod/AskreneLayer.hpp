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
 */
Ev::Io<void> disable_node( Boss::Mod::Rpc& rpc
			 , std::string const& layer
			 , Ln::NodeId node
			 );

/* Create a fresh non-persistent askrene layer scoped to a single
 * caller (typically one FundsMover::Attempter run).  The layer's
 * lifetime is the caller's responsibility -- pair with remove_layer
 * at the end of the run.  Used to encode absolute within-run
 * exclusions (e.g. failing channel-directions) that should NOT
 * accumulate in the persistent clboss layer because:
 *
 *   - The failcode might not be a capacity issue (FEE_INSUFFICIENT,
 *     CLTV problems, etc.) -- persistent max_msat would be the
 *     wrong feedback and accumulate misinformation.
 *   - Even when the failcode IS capacity-related, conditional
 *     max_msat constraints in the persistent layer can be re-
 *     selected at amount = max_msat - 1, producing a ratchet-
 *     by-1-msat retry storm if askrene's gossmap hasn't refreshed
 *     between failure and retry.  An absolute "channel disabled"
 *     write in a transient layer dominates the conditional
 *     max_msat (askrene takes the min of all max_msats across
 *     layers, askrene/layer.c:1004-1009) and ends the storm.
 *
 * Layer name is "clboss-attempt-<uuid>" where uuid is a fresh
 * 16-byte random value (Uuid::random()).  Returned name should be
 * passed to subsequent inform_channel_constrained / disable_node
 * calls (with this layer name instead of clboss_layer_name) and
 * to remove_layer at the end.
 *
 * Non-fatal on RpcError: returns the would-be layer name even on
 * failure so the caller can still attempt remove_layer (a no-op
 * if create failed), and subsequent inform/disable writes will
 * also fail silently in degraded-learning mode -- the
 * within-attempter retry storm protection is lost but the
 * Attempter itself continues to work.
 */
Ev::Io<std::string> create_transient_layer(Boss::Mod::Rpc& rpc);

/* Remove the named layer.  Used to clean up a transient layer
 * created by create_transient_layer at the end of the caller's
 * lifetime.  Non-fatal on RpcError: a leaked transient layer is
 * bounded (persistent=false means it does not survive plugin
 * restart) but should be rare in practice.
 */
Ev::Io<void> remove_layer( Boss::Mod::Rpc& rpc
			 , std::string const& layer
			 );

}}}

#endif /* !defined(BOSS_MOD_ASKRENELAYER_HPP_) */
