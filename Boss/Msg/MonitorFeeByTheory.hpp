#ifndef BOSS_MSG_MONITORFEEBYTHEORY_HPP
#define BOSS_MSG_MONITORFEEBYTHEORY_HPP

#include"Ln/NodeId.hpp"
#include<cstdint>
#include<optional>

namespace Boss { namespace Msg {

/** struct Boss::Msg::MonitorFeeByTheory
 *
 * @brief informs the fee monitor of the price-theory
 * fee level and multiplier for a peer.
 */
struct MonitorFeeByTheory {
	Ln::NodeId node;
	std::int64_t level;
	double mult;
	std::optional<std::uint32_t> cards_left;
};

}}

#endif /* !defined(BOSS_MSG_MONITORFEEBYTHEORY_HPP) */
