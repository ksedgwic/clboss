#ifndef BOSS_MSG_REQUESTGETCIRCULARREBALANCEFLAG_HPP
#define BOSS_MSG_REQUESTGETCIRCULARREBALANCEFLAG_HPP

namespace Boss { namespace Msg {

/** struct Boss::Msg::RequestGetCircularRebalanceFlag
 *
 * @brief emit in order to query if we should be automatically performing
 * circular rebalancing or not.
 * This will be replied to with `ResponseGetCircularRebalanceFlag`.
 */
struct RequestGetCircularRebalanceFlag {
	void* requester;
};

}}

#endif /* BOSS_MSG_REQUESTGETCIRCULARREBALANCEFLAG_HPP */
