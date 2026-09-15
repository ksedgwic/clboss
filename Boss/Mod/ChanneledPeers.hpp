#ifndef BOSS_MOD_CHANNELEDPEERS_HPP
#define BOSS_MOD_CHANNELEDPEERS_HPP

#include"Ln/NodeId.hpp"
#include<set>
#include<string>

namespace Jsmn { class Object; }

namespace Boss { namespace Mod {

/** Boss::Mod::channeled_state
 *
 * @brief whether a channel in this `listpeerchannels` state counts
 * as a channel we have with the peer: opening (`OPENINGD_*`,
 * `DUALOPEND_*`) or open (`CHANNELD_*`, which includes a channel
 * awaiting splice lock-in or shutting down).  Closing and closed
 * states (`CLOSINGD_*`, `AWAITING_UNILATERAL`, `FUNDING_SPEND_SEEN`,
 * `ONCHAIN`) do not count.
 */
bool channeled_state(std::string const& state);

/** Boss::Mod::channeled_peers
 *
 * @brief the peers with at least one channel in a channeled state,
 * read from a `listpeerchannels` result.  Channels without a
 * readable `state` or `peer_id` are skipped.
 */
std::set<Ln::NodeId> channeled_peers(Jsmn::Object const& listpeerchannels);

}}

#endif /* !defined(BOSS_MOD_CHANNELEDPEERS_HPP) */
