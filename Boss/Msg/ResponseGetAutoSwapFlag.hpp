#ifndef BOSS_MSG_RESPONSEGETAUTOSWAPFLAG_HPP
#define BOSS_MSG_RESPONSEGETAUTOSWAPFLAG_HPP

namespace Boss { namespace Msg {

/** struct Boss::Msg::ResponseGetAutoSwapFlag
 *
 * @brief Emitted in response to `RequestGetAutoSwapFlag`.
 */
struct ResponseGetAutoSwapFlag {
	void* requester;
	/* Set if we should be automatically performing offchain-to-onchain
	   swaps.  */
	bool state;
	/* Description of whether state is permanent or temporary and for how
	   long.  */
	std::string comment;
};

}}

#endif /* BOSS_MSG_RESPONSEGETAUTOSWAPFLAG_HPP */
