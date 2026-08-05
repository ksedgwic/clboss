#ifndef BOSS_MSG_DEMANDOBSERVED_HPP
#define BOSS_MSG_DEMANDOBSERVED_HPP

#include"Ln/Scid.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::DemandObserved
 *
 * @brief Raised by `Boss::Mod::DemandTracker` for each forward
 * about to exit through one of our channels: someone is spending
 * that channel's outgoing liquidity right now.
 *
 * Pure observation -- the HTLC is never held.  The amount is
 * deliberately not carried: unforwardable probe HTLCs cost an
 * attacker nothing, so any consumer sizing from a demanded
 * amount would hand out a free lever over our spend.
 */
struct DemandObserved {
	/* The outgoing channel of the forward.  */
	Ln::Scid out_scid;
};

}}

#endif /* !defined(BOSS_MSG_DEMANDOBSERVED_HPP) */
