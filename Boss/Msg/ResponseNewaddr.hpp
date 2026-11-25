#ifndef BOSS_MSG_RESPONSENEWADDR_HPP
#define BOSS_MSG_RESPONSENEWADDR_HPP

#include<cstdint>
#include<string>

namespace Boss { namespace Msg {

/** struct Boss::Msg::ResponseNewaddr
 *
 * @brief emitted in response to a
 * `Boss::Msg::RequestNewaddr`
 */
struct ResponseNewaddr {
	std::string address;
	void* requester;
	/* The round of the `Boss::Msg::RequestNewaddr`, as given.  */
	std::uint64_t round;
};

}}

#endif /* !defined(BOSS_MSG_RESPONSENEWADDR_HPP) */
