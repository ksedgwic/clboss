#ifndef BOSS_MSG_MONITORFEESETCHANNEL_HPP
#define BOSS_MSG_MONITORFEESETCHANNEL_HPP

#include"Ln/NodeId.hpp"
#include<cstdint>

namespace Boss { namespace Msg {

/** struct Boss::Msg::MonitorFeeSetChannel
 *
 * @brief informs the fee monitor that fees were set
 * for a peer.
 *
 * Fees are per peer, not per channel: complaints, fee policy and
 * the track record are all kept per peer, so with several channels
 * to one peer the same fee applies to each of them.
 */
struct MonitorFeeSetChannel {
	Ln::NodeId node;
	std::uint32_t base;
	std::uint32_t proportional;
};

}}

#endif /* !defined(BOSS_MSG_MONITORFEESETCHANNEL_HPP) */
