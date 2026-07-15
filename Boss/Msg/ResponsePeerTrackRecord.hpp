#ifndef BOSS_MSG_RESPONSEPEERTRACKRECORD_HPP
#define BOSS_MSG_RESPONSEPEERTRACKRECORD_HPP

#include"Boss/Msg/TrackRecord.hpp"
#include"Ln/NodeId.hpp"
#include<map>

namespace Boss { namespace Msg {

/** struct Boss::Msg::ResponsePeerTrackRecord
 *
 * @brief response to `Boss::Msg::RequestPeerTrackRecord`;
 * one entry per requested node.
 */
struct ResponsePeerTrackRecord {
	/* Copied from the corresponding request.  */
	void* requester;
	std::map<Ln::NodeId, TrackRecord> records;
};

}}

#endif /* !defined(BOSS_MSG_RESPONSEPEERTRACKRECORD_HPP) */
