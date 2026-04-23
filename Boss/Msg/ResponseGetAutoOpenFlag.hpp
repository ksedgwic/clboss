#ifndef BOSS_MSG_RESPONSEGETAUTOOPENFLAG_HPP
#define BOSS_MSG_RESPONSEGETAUTOOPENFLAG_HPP

namespace Boss { namespace Msg {

/** struct Boss::Msg::ResponseGetAutoOpenFlag
 *
 * @brief Emitted in response to `RequestGetAutoOpenFlag`.
 */
struct ResponseGetAutoOpenFlag {
	void* requester;
	/* Set if we should be automatically opening channels.  */
	bool state;
	/* Description of whether state is permanent or temporary and for how
	   long.  */
	std::string comment;
};

}}

#endif /* BOSS_MSG_RESPONSEGETAUTOOPENFLAG_HPP */
