#ifndef BOSS_MSG_ASKRENECHANNELUPDATE_HPP
#define BOSS_MSG_ASKRENECHANNELUPDATE_HPP

#include"Ln/Amount.hpp"
#include"Ln/Scid.hpp"
#include<cstdint>

namespace Boss { namespace Msg {

/** struct Boss::Msg::AskreneChannelUpdate
 *
 * @brief a rebalancer learned, from routing-failure feedback, a fresher
 * policy for one channel direction (the channel_update the erring hop
 * handed back) and wants it applied as an override.
 *
 * @desc recorded by Boss::Mod::AskreneUpdates into its append-only
 * channel-update log.  askrene never ages these local channel_update
 * overrides, so CLBOSS owns their lifetime: the latest per direction is
 * re-projected into a private per-request layer only while still within
 * clboss-channel-update-age-secs, and rows are pruned at the retention
 * horizon.  Not all of these "block": only the enabled=false case
 * excludes the direction; the rest just re-price / re-bound it.
 *
 * The same struct doubles as the row type in
 * Boss::Msg::ResponseAskreneUpdates.
 */
struct AskreneChannelUpdate {
	Ln::Scid scid;
	std::uint32_t direction;
	bool enabled;
	Ln::Amount htlc_minimum_msat;
	Ln::Amount htlc_maximum_msat;
	Ln::Amount fee_base_msat;
	std::uint32_t fee_proportional_millionths;
	std::uint16_t cltv_expiry_delta;
};

}}

#endif /* !defined(BOSS_MSG_ASKRENECHANNELUPDATE_HPP) */
