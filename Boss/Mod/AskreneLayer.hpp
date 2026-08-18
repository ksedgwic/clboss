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

/* Tell askrene to avoid the given node entirely for routes
 * that include this layer.  Non-fatal on RpcError.
 *
 * Note: askrene's layer_add_disabled_node appends without
 * deduping; repeated calls accumulate identical entries.
 * Callers that want once-per-restart semantics (e.g. the
 * clboss-self exclusion initialization) should gate the
 * call on is_node_disabled() below.
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

/* --- self-exclusion layer ------------------------------------------
 *
 * Name of the tiny persistent askrene layer whose only content is our
 * own node in disabled_nodes.  Modules whose getroutes must never
 * route through us as a middle hop -- Dowser capacity probes and
 * ActiveProber probes, whose askrene source is a REMOTE node -- pass
 * this layer.  The legacy getroute calls expressed the same intent
 * with exclude=[self_id]; askrene has no inline equivalent, only
 * layers.
 */
extern std::string const self_layer_name;

/* Ensure self_layer_name exists and carries our node in its
 * disabled_nodes (idempotent create; the disable is deduped via
 * is_node_disabled, since askrene appends without deduping).
 * Resolves true when the layer is ready to be named in a getroutes
 * layers array; false when askrene is unavailable (RpcError), in
 * which case the caller must omit the layer -- naming an absent
 * layer is a hard getroutes error -- and degrade to probing without
 * self-exclusion.  Cheap enough to call per probe: create is a no-op
 * on an existing persistent layer, and the dedup check dumps only
 * this one-entry layer.
 */
Ev::Io<bool> ensure_self_layer( Boss::Mod::Rpc& rpc
			      , Ln::NodeId self_id
			      );

}}}

#endif /* !defined(BOSS_MOD_ASKRENELAYER_HPP_) */
