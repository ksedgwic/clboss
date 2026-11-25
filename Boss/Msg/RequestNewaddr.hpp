#ifndef BOSS_MSG_REQUESTNEWADDR_HPP
#define BOSS_MSG_REQUESTNEWADDR_HPP

#include<cstdint>

namespace Boss { namespace Msg {

/** struct Boss::Msg::RequestNewaddr
 *
 * @brief emitted to request for a new
 * address.
 */
struct RequestNewaddr {
	void* requester;
	/* The requester's round: an answer carries it back, so a
	 * requester can tell the answer to its current request from a
	 * late one to a request it has given up on.  */
	std::uint64_t round;
};

}}

#endif /* !defined(BOSS_MSG_REQUESTNEWADDR_HPP) */
