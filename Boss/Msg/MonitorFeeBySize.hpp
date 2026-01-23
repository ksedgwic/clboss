#ifndef BOSS_MSG_MONITORFEEBYSIZE_HPP
#define BOSS_MSG_MONITORFEEBYSIZE_HPP

#include"Ln/NodeId.hpp"
#include<cstdint>

namespace Boss { namespace Msg {

/** struct Boss::Msg::MonitorFeeBySize
 *
 * @brief informs the fee monitor of the size-based
 * fee multiplier for a peer.
 */
struct MonitorFeeBySize {
	Ln::NodeId node;
	std::uint64_t total_peers;
	std::uint64_t less_peers;
	double mult;
};

}}

#endif /* !defined(BOSS_MSG_MONITORFEEBYSIZE_HPP) */
