#ifndef BOSS_MOD_CHANNELBALANCE_HPP
#define BOSS_MOD_CHANNELBALANCE_HPP

#include"Ln/Amount.hpp"

namespace Jsmn { class Object; }

namespace Boss { namespace Mod {

/** struct Boss::Mod::ChannelBalance
 *
 * @brief our balance and the capacity of one `listpeerchannels`
 * channel, with a pending splice-out already deducted.
 *
 * @desc While a splice is pending (`CHANNELD_AWAITING_SPLICE`),
 * `listpeerchannels` reports `to_us_msat` and `total_msat` of the
 * old funding until the new one locks in, but `channeld` already
 * admits HTLCs against the lower post-splice balance: it subtracts
 * the lowest `splice_amount` over the inflight fundings.  Reading
 * the old numbers would let us send funds through a channel that
 * no longer has them.  A splice-in is not credited until lock-in,
 * the same as `channeld`.
 */
struct ChannelBalance {
	Ln::Amount to_us;
	Ln::Amount total;
};

/** Boss::Mod::channel_balance
 *
 * @brief reads the balance of a `listpeerchannels` channel object,
 * deducting a pending splice-out.
 * Throws `Jsmn::TypeError` if `to_us_msat` or `total_msat` is
 * missing or malformed, as a plain read would.
 */
ChannelBalance channel_balance(Jsmn::Object const& channel);

}}

#endif /* !defined(BOSS_MOD_CHANNELBALANCE_HPP) */
