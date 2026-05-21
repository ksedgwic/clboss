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

}}}

#endif /* !defined(BOSS_MOD_ASKRENELAYER_HPP_) */
