#ifndef BOSS_MSG_RESPONSEGETCIRCULARREBALANCEFLAG_HPP
#define BOSS_MSG_RESPONSEGETCIRCULARREBALANCEFLAG_HPP

namespace Boss { namespace Msg {

/** struct Boss::Msg::ResponseGetCircularRebalanceFlag
 *
 * @brief Emitted in response to `RequestGetCircularRebalanceFlag`.
 */
struct ResponseGetCircularRebalanceFlag {
	void* requester;
	/* Set if we should be automatically performing circular rebalancing.  */
	bool state;
	/* Description of whether state is permanent or temporary and for how
	   long.  */
	std::string comment;
};

}}

#endif /* BOSS_MSG_RESPONSEGETCIRCULARREBALANCEFLAG_HPP */
