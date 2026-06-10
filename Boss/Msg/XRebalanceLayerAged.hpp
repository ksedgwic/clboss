#ifndef BOSS_MSG_XREBALANCELAYERAGED_HPP
#define BOSS_MSG_XREBALANCELAYERAGED_HPP

#include<cstdint>

namespace Boss { namespace Msg {

/** struct Boss::Msg::XRebalanceLayerAged
 *
 * @brief Raised by Boss::Mod::XMoveFunds after its hourly
 * `askrene-age` pass over the clboss-xrebalance layer (whether or
 * not the RPC succeeded; on failure the layer merely retains stale
 * entries, which is safe for subscribers).
 *
 * @desc This is the "end of expiration cycle" hook: constraints
 * older than `cutoff` have just been trimmed from the routed layer,
 * so any channel direction whose newest real observation is older
 * than `cutoff` now has NO live evidence -- exactly the set the
 * persistence forecaster (Boss::Mod::XRebalancePredictor) considers
 * for synthetic re-assertion.
 */
struct XRebalanceLayerAged {
	/* When the aging pass ran (unix seconds).  */
	std::uint64_t time;
	/* The cutoff passed to askrene-age: constraints with
	 * timestamp < cutoff were removed.  */
	std::uint64_t cutoff;
};

}}

#endif /* !defined(BOSS_MSG_XREBALANCELAYERAGED_HPP) */
