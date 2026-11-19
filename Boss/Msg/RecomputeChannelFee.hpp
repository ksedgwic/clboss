#ifndef BOSS_MSG_RECOMPUTECHANNELFEE_HPP
#define BOSS_MSG_RECOMPUTECHANNELFEE_HPP

#include"Ln/NodeId.hpp"
#include<memory>

namespace Boss { namespace Msg {

/** struct Boss::Msg::RecomputeChannelFee
 *
 * @brief asks for the fee of the channels with a peer to be
 * computed and set again now, without waiting for the next
 * survey of the peer's competitors.
 *
 * @desc `Boss::Mod::ChannelFeeManager` computes it from the
 * median fee it last received for the peer.
 * If it has none for the peer, nothing is computed.
 * If `recomputed` is given, it is set to whether the fee was
 * computed.
 * The message returns once the fee has been set.
 */
struct RecomputeChannelFee {
	Ln::NodeId node;
	std::shared_ptr<bool> recomputed;
};

}}

#endif /* !defined(BOSS_MSG_RECOMPUTECHANNELFEE_HPP) */
