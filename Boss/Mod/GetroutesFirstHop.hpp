#ifndef BOSS_MOD_GETROUTESFIRSTHOP_HPP_
#define BOSS_MOD_GETROUTESFIRSTHOP_HPP_

#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include<cstdint>

namespace Jsmn { class Object; }

namespace Boss { namespace Mod {

/** struct Boss::Mod::GetroutesFirstHop
 *
 * @brief Out-side view of a `getroutes` route's first hop: the
 * node the source forwards to, and the amount and CLTV the
 * source must send onward.
 *
 * @desc CLN v26.06 put these on the hop directly (`node_id_out`
 * / `amount_out_msat` / `cltv_out`).  A stock v26.04 hop carries
 * only the deprecated trio (`next_node_id` / `amount_msat` /
 * `delay`), whose amount and delay are the hop's IN-side values;
 * the out-side values live one hop over -- the second hop's
 * in-values, or for a single-hop route the route's delivered
 * `amount_msat` and `final_cltv` (both required in the v26.04
 * schema).  The constructor reads the v26.06 fields when present
 * and otherwise derives them by that shift, which is an
 * identity, not a guess.  A route carrying neither form throws
 * `Jsmn::TypeError`.
 */
struct GetroutesFirstHop {
	Ln::NodeId node_id_out;
	Ln::Amount amount_out;
	std::uint32_t cltv_out;

	/* route is one entry of the getroutes `routes` array.  */
	explicit GetroutesFirstHop(Jsmn::Object const& route);
};

}}

#endif /* !defined(BOSS_MOD_GETROUTESFIRSTHOP_HPP_) */
