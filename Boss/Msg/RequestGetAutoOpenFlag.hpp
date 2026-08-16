#ifndef BOSS_MSG_REQUESTGETAUTOOPENFLAG_HPP
#define BOSS_MSG_REQUESTGETAUTOOPENFLAG_HPP

namespace Boss { namespace Msg {

/** struct Boss::Msg::RequestGetAutoOpenFlag
 *
 * @brief emit in order to query if we should be automatically opening channels
 * or not.
 * This will be replied to with `ResponseGetAutoOpenFlag`.
 */
struct RequestGetAutoOpenFlag {
	void* requester;
};

}}

#endif /* BOSS_MSG_REQUESTGETAUTOOPENFLAG_HPP */
