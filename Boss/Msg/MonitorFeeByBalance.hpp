#ifndef BOSS_MSG_MONITORFEEBYBALANCE_HPP
#define BOSS_MSG_MONITORFEEBYBALANCE_HPP

#include"Ln/NodeId.hpp"
#include<cstdint>

namespace Boss { namespace Msg {

/** struct Boss::Msg::MonitorFeeByBalance
 *
 * @brief informs the fee monitor of the balance-based
 * fee multiplier for a peer.
 */
struct MonitorFeeByBalance {
	Ln::NodeId node;
	double mult;
	std::uint64_t our_msat;
	std::uint64_t total_msat;
};

}}

#endif /* !defined(BOSS_MSG_MONITORFEEBYBALANCE_HPP) */
