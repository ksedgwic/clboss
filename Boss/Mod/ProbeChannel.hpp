#ifndef BOSS_MOD_PROBECHANNEL_HPP
#define BOSS_MOD_PROBECHANNEL_HPP

#include"Ln/Amount.hpp"
#include"Ln/Scid.hpp"

namespace Jsmn { class Object; }

namespace Boss { namespace Mod {

/** struct Boss::Mod::ProbeChannel
 *
 * @brief the channel an active probe of a peer goes out through,
 * and how much can be sent over it.
 */
struct ProbeChannel {
	Ln::Scid scid;
	Ln::Amount spendable;
};

/** Boss::Mod::probe_channel
 *
 * @brief picks the channel to probe a peer through from the
 * `channels` array of a `listpeerchannels` result for that peer:
 * the `CHANNELD_NORMAL` channel with a short channel id and the
 * largest `spendable_msat`.  The probe names this channel as its
 * first hop and lightningd sends over exactly that channel, so the
 * probe amount is bounded by one channel's spendable, never by the
 * sum over the peer's channels.
 *
 * @desc Only `CHANNELD_NORMAL` counts: while a splice is pending,
 * `spendable_msat` still describes the old funding, and a probe sized
 * from it could fail locally instead of measuring the peer.
 *
 * Returns a default (false) `scid` when the peer has no such channel.
 * Throws `Jsmn::TypeError` on a malformed channel, as a plain read
 * would.
 */
ProbeChannel probe_channel(Jsmn::Object const& channels);

}}

#endif /* !defined(BOSS_MOD_PROBECHANNEL_HPP) */
