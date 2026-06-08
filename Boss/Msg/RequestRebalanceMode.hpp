#ifndef BOSS_MSG_REQUESTREBALANCEMODE_HPP
#define BOSS_MSG_REQUESTREBALANCEMODE_HPP

namespace Boss { namespace Msg {

/** struct Boss::Msg::RequestRebalanceMode
 *
 * @brief Requests the `Boss::Mod::RebalanceModeManager` to provide
 * the currently-active rebalancing mode.
 */
struct RequestRebalanceMode {
	void* requester;
};

}}

#endif /* !defined(BOSS_MSG_REQUESTREBALANCEMODE_HPP) */
