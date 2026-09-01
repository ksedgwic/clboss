#ifndef BOSS_MSG_CHANNELDESTRUCTION_HPP
#define BOSS_MSG_CHANNELDESTRUCTION_HPP

#include"Ln/NodeId.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::ChannelDestruction
 *
 * @brief Tells everyone that we lost an existing channel
 * with a peer.
 * Basically, if we leave the live states (`CHANNELD_NORMAL`, or
 * `CHANNELD_AWAITING_SPLICE` while a splice locks in) for some
 * other state, or if we leave `CHANNELD_AWAITING_LOCKIN` but did
 * not enter a live state.
 * See also `Boss::Msg::ChannelCreation`.
 */
struct ChannelDestruction {
	Ln::NodeId peer;
};

}}

#endif /* !defined(BOSS_MSG_CHANNELDESTRUCTION_HPP) */
