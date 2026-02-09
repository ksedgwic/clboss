#ifndef BOSS_MSG_MONITORFEESETCHANNEL_HPP
#define BOSS_MSG_MONITORFEESETCHANNEL_HPP

#include"Ln/NodeId.hpp"
#include<cstdint>

namespace Boss { namespace Msg {

/** struct Boss::Msg::MonitorFeeSetChannel
 *
 * @brief informs the fee monitor that fees were set
 * for a peer.
 */
struct MonitorFeeSetChannel {
	Ln::NodeId node;
	std::uint32_t base;
	std::uint32_t proportional;
};

}}

#endif /* !defined(BOSS_MSG_MONITORFEESETCHANNEL_HPP) */
