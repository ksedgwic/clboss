#ifndef BOSS_MSG_ASKRENENODEDISABLEUPDATE_HPP
#define BOSS_MSG_ASKRENENODEDISABLEUPDATE_HPP

#include"Ln/NodeId.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::AskreneNodeDisableUpdate
 *
 * @brief a rebalancer learned, from routing-failure feedback, that a
 * whole node should be avoided (a NODE-level failure).
 *
 * @desc recorded by Boss::Mod::AskreneUpdates into its append-only
 * node-disable log.  askrene never ages disabled_nodes, so CLBOSS owns
 * their lifetime: they are re-projected into a private per-request layer
 * only while still within clboss-node-disable-age-secs of the last such
 * failure, and pruned from the log at the retention horizon.
 */
struct AskreneNodeDisableUpdate {
	Ln::NodeId node;
};

}}

#endif /* !defined(BOSS_MSG_ASKRENENODEDISABLEUPDATE_HPP) */
