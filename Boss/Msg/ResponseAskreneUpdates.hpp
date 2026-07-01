#ifndef BOSS_MSG_RESPONSEASKRENEUPDATES_HPP
#define BOSS_MSG_RESPONSEASKRENEUPDATES_HPP

#include"Boss/Msg/AskreneChannelUpdate.hpp"
#include"Ln/NodeId.hpp"
#include<vector>

namespace Boss { namespace Msg {

/** struct Boss::Msg::ResponseAskreneUpdates
 *
 * @brief the learned updates still within their projection window,
 * answering Boss::Msg::RequestAskreneUpdates.
 *
 * @desc `node_disables` is the distinct set of nodes to disable;
 * `channel_updates` is the latest override per channel direction.  The
 * requester loads these into a private per-request layer via
 * Boss::Mod::AskreneUpdates::open_layer.
 */
struct ResponseAskreneUpdates {
	void* requester;
	std::vector<Ln::NodeId> node_disables;
	std::vector<AskreneChannelUpdate> channel_updates;
};

}}

#endif /* !defined(BOSS_MSG_RESPONSEASKRENEUPDATES_HPP) */
