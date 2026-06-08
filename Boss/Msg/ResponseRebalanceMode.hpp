#ifndef BOSS_MSG_RESPONSEREBALANCEMODE_HPP
#define BOSS_MSG_RESPONSEREBALANCEMODE_HPP

#include"Boss/RebalanceMode.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::ResponseRebalanceMode
 *
 * @brief Broadcasted by `Boss::Mod::RebalanceModeManager` in response
 * to `Boss::Msg::RequestRebalanceMode`.
 */
struct ResponseRebalanceMode {
	void* requester;

	RebalanceMode mode;
};

}}

#endif /* !defined(BOSS_MSG_RESPONSEREBALANCEMODE_HPP) */
