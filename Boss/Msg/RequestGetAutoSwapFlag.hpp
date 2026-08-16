#ifndef BOSS_MSG_REQUESTGETAUTOSWAPFLAG_HPP
#define BOSS_MSG_REQUESTGETAUTOSWAPFLAG_HPP

namespace Boss { namespace Msg {

/** struct Boss::Msg::RequestGetAutoSwapFlag
 *
 * @brief emit in order to query if we should be automatically performing
 * offchain-to-onchain swaps or not.
 * This will be replied to with `ResponseGetAutoSwapFlag`.
 */
struct RequestGetAutoSwapFlag {
	void* requester;
};

}}

#endif /* BOSS_MSG_REQUESTGETAUTOSWAPFLAG_HPP */
