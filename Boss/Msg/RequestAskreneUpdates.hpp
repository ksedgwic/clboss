#ifndef BOSS_MSG_REQUESTASKRENEUPDATES_HPP
#define BOSS_MSG_REQUESTASKRENEUPDATES_HPP

namespace Boss { namespace Msg {

/** struct Boss::Msg::RequestAskreneUpdates
 *
 * @brief ask Boss::Mod::AskreneUpdates for the set of learned updates
 * that are still within their projection window, to load into a private
 * per-request askrene layer before a getroutes.
 *
 * @desc answered by Boss::Msg::ResponseAskreneUpdates.  Used by both
 * rebalance engines (classic FundsMover and xrebalance XMoveFunds) so
 * the projection logic lives in one place with no per-engine variation.
 */
struct RequestAskreneUpdates {
	void* requester;
};

}}

#endif /* !defined(BOSS_MSG_REQUESTASKRENEUPDATES_HPP) */
