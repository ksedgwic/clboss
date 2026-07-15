#ifndef BOSS_MSG_REQUESTPEERTRACKRECORD_HPP
#define BOSS_MSG_REQUESTPEERTRACKRECORD_HPP

#include"Ln/NodeId.hpp"
#include<vector>

namespace Boss { namespace Msg {

/** struct Boss::Msg::RequestPeerTrackRecord
 *
 * @brief emit this to ask `Boss::Mod::PeerTrackRecord` to
 * judge the earnings track record of the given nodes.
 * Responded to with `Boss::Msg::ResponsePeerTrackRecord`.
 */
struct RequestPeerTrackRecord {
	/* Used to identify the requesting object; copied to the
	 * corresponding `Boss::Msg::ResponsePeerTrackRecord`.  */
	void* requester;
	std::vector<Ln::NodeId> nodes;
};

}}

#endif /* !defined(BOSS_MSG_REQUESTPEERTRACKRECORD_HPP) */
