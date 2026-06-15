#ifndef BOSS_MSG_REQUESTDOWSER_HPP
#define BOSS_MSG_REQUESTDOWSER_HPP

#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::RequestDowser
 *
 * @brief Requests for a dowser estimation of the flow between
 * two nodes.
 *
 * @desc The dowser just guesses how much capacity is available
 * between the two specified nodes.
 */
struct RequestDowser {
	void* requester;
	Ln::NodeId fromid;
	Ln::NodeId toid;
	/* The flow level the caller wants the probe sized to.  The dowser
	 * sizes its probe so that a full-flow result reaches this amount;
	 * the askrene dowser caps its result at the probe, so it can never
	 * report more than this.  When zero (the default), the dowser uses
	 * its own fixed default probe amount.
	 *
	 * Threshold callers that only ask "is at least clboss-min-channel
	 * reachable?" set this to min-channel.  Callers that SIZE something
	 * from the result -- the ChannelCreator opens up to the dowsed
	 * amount -- must set this to the largest amount they would use
	 * (clboss-max-channel); otherwise the result is pinned to the probe
	 * and the size collapses to it.  */
	Ln::Amount probe_target;
};

}}

#endif /* !defined(BOSS_MSG_REQUESTDOWSER_HPP) */
