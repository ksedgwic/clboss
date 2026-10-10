#ifndef BOSS_MSG_AMOUNTSETTINGS_HPP
#define BOSS_MSG_AMOUNTSETTINGS_HPP

#include"Ln/Amount.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::AmountSettings
 *
 * @brief Broadcast before `init` completes (before RPC
 * and DB are available) with all the various settings
 * related to how large the node is, and again whenever
 * one of the amount options changes via `setconfig`.
 * Consumers copy the fields they use on every delivery.
 */
struct AmountSettings {
	/* Minimum and maximum channel size.  */
	Ln::Amount min_channel;
	Ln::Amount max_channel;
	/* How much we will always definitely leave onchain for
	 * various actions?  */
	Ln::Amount reserve;
	/* How much should be onchain in order to trigger channel
	 * creation?
	 */
	Ln::Amount min_amount;
	/* On channel creation, how much to leave onchain at minimum
	 * for the next channel creation event?  */
	Ln::Amount min_remaining;
};

}}

#endif /* !defined(BOSS_MSG_AMOUNTSETTINGS_HPP) */
