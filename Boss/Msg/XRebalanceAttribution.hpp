#ifndef BOSS_MSG_XREBALANCEATTRIBUTION_HPP
#define BOSS_MSG_XREBALANCEATTRIBUTION_HPP

#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::XRebalanceAttribution
 *
 * @brief Per-part earnings attribution from a successful
 * `clboss-xmovefunds` sendpay part.
 *
 * Raised by Boss::Mod::XMoveFunds once per part that returns
 * `status: "complete"` from waitsendpay.  Boss::Mod::EarningsTracker
 * subscribes and applies the same symmetric DB update the
 * FundsMover path already runs on Msg::ResponseMoveFunds: source
 * peer gets `in_expenditures += fee_spent` / `in_rebalanced +=
 * amount_moved`, destination peer gets the matching `out_*`
 * increments.
 *
 * Distinct from Msg::ResponseMoveFunds because XMoveFunds has no
 * Runner pendings table -- a single `clboss-xmovefunds` invocation
 * can use a SET of source / dest scids and have askrene's MCF
 * split the flow across multiple (source, dest) pairs.  Each
 * successful part identifies its actual (source_peer, dest_peer)
 * from the askrene route (first hop's node_id_out and the closing
 * hop's node_id_in).
 */
struct XRebalanceAttribution {
	/* Peer at the outbound end of the source channel used by this
	 * part -- the node we forwarded TO on the first real hop. */
	Ln::NodeId source;
	/* Peer at the inbound end of the destination channel used by
	 * this part -- the node that forwarded BACK to us on the
	 * closing hop (a.k.a. fill_peer). */
	Ln::NodeId destination;
	/* Amount delivered (i.e. amount returned to us via the closing
	 * hop) -- waitsendpay.amount_msat. */
	Ln::Amount amount_moved;
	/* Total fee paid across all middle hops for this part --
	 * waitsendpay.amount_sent_msat - waitsendpay.amount_msat. */
	Ln::Amount fee_spent;
};

}}

#endif /* !defined(BOSS_MSG_XREBALANCEATTRIBUTION_HPP) */
