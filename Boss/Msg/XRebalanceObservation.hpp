#ifndef BOSS_MSG_XREBALANCEOBSERVATION_HPP
#define BOSS_MSG_XREBALANCEOBSERVATION_HPP

#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include<cstdint>

namespace Boss { namespace Msg {

/** enum Boss::Msg::XRebalanceObservationKind
 *
 * @brief What kind of liquidity evidence a single xrebalance
 * payment part produced about one directed channel.
 */
enum class XRebalanceObservationKind
{ /* A part traversed this hop carrying `amount`: liquidity in
   * this direction was >= amount at `time`.  */
  Success
  /* Channel-level failure: the hop could not carry `amount`,
   * so liquidity in this direction was < amount at `time`.  */
, LiquidityFail
  /* 0x100c inbound-fee exclusion: the channel was excluded for
   * fee-policy reasons (we cannot pay positive inbound fees),
   * not for lack of liquidity.  `amount` is the nominal 1 msat
   * of the max=0 constraint.  */
, PolicyFail
  /* Node-level onion failure: the whole erring node was
   * disabled; `scid` is the erring channel for context.  */
, NodeFail
};

/** struct Boss::Msg::XRebalanceObservation
 *
 * @brief One real liquidity observation from an xrebalance
 * payment part, raised by Boss::Mod::XMoveFunds alongside every
 * askrene feedback write (inform_channel_* / disable_node) on
 * the clboss-xrebalance layer.
 *
 * @desc Boss::Mod::XRebalanceHistory subscribes and records
 * these losslessly (kind, failcode, erring node -- richer than
 * the min/max projection askrene keeps) so the persistence
 * forecaster and future reliability statistics have a full
 * evidence base.  Synthetic forecast re-assertions are NOT
 * observations and must never raise this message.
 */
struct XRebalanceObservation {
	/* Unix time (seconds) the observation was made.  */
	std::uint64_t time;
	/* The directed channel observed.  */
	Ln::Scid scid;
	std::uint32_t dir;
	XRebalanceObservationKind kind;
	/* Hop amount carried (Success) or attempted (the *Fail
	 * kinds; falls back to 1 msat when the per-hop amount is
	 * unparseable, mirroring the inform fallback).  */
	Ln::Amount amount;
	/* BOLT4 failcode for the *Fail kinds; 0 for Success.  */
	std::uint16_t failcode;
	/* Erring node for the *Fail kinds; null for Success.  */
	Ln::NodeId erring_node;
};

}}

#endif /* !defined(BOSS_MSG_XREBALANCEOBSERVATION_HPP) */
