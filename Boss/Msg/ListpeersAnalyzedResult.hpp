#ifndef BOSS_MSG_LISTPEERSANALYZEDRESULT_HPP
#define BOSS_MSG_LISTPEERSANALYZEDRESULT_HPP

#include"Ln/NodeId.hpp"
#include<set>

namespace Boss { namespace Msg {

/** struct Boss::Msg::ListpeersAnalyzedResult
 *
 * @brief message derived from `Boss::Msg::ListpeersResult`
 * with the peers split up into bins: connected+channeled,
 * disconnected+channeled, connected+unchanneled,
 * disconnected+unchanneled.
 *
 * @desc "channeled" specifically means that there is at
 * least one channel that is in some `OPENINGD` or
 * `CHANNELD` state.
 */
struct ListpeersAnalyzedResult {
	std::set<Ln::NodeId> connected_channeled;
	std::set<Ln::NodeId> disconnected_channeled;
	std::set<Ln::NodeId> connected_unchanneled;
	std::set<Ln::NodeId> disconnected_unchanneled;
	/* Whether this `listpeers` was performed during `init`
	 * or on the 10-minute timer.  */
	bool initial;
	/* We have channeled peers and no connection of any kind,
	 * channeled or not.  That is our own outage (lightningd
	 * --offline, a Tor or firewall failure), not every peer
	 * failing at once, and the internet probe cannot see it
	 * (#346).  Filled by `Boss::Mod::ListpeersAnalyzer`.  */
	bool all_peers_disconnected;
};

}}

#endif /* !defined(BOSS_MSG_LISTPEERSANALYZEDRESULT_HPP) */
