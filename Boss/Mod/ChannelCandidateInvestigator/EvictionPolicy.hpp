#ifndef BOSS_MOD_CHANNELCANDIDATEINVESTIGATOR_EVICTIONPOLICY_HPP
#define BOSS_MOD_CHANNELCANDIDATEINVESTIGATOR_EVICTIONPOLICY_HPP

#include"Boss/Msg/TrackRecord.hpp"
#include"Ln/NodeId.hpp"
#include<map>
#include<set>
#include<utility>
#include<vector>

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator {

/** Boss::Mod::ChannelCandidateInvestigator::EvictionPolicy
 *
 * @brief chooses which candidate to drop when the candidate
 * table exceeds its cap.
 *
 * @desc Strata, most expendable first:
 *
 *   1. proven underperformers,
 *   2. no-record candidates not advertising splice support,
 *   3. no-record candidates advertising splice support,
 *   4. keepers.
 *
 * Uniform random within the first non-empty stratum, so the
 * eviction can never cost a keeper (or a spliceable
 * candidate) while something more expendable is in the pool.
 * A node absent from `verdicts` counts as no-record.
 */
namespace EvictionPolicy {

/* Returns the victim and the stratum name for the log.
 * `pool` must be non-empty.  */
std::pair<Ln::NodeId, char const*>
pick( std::vector<Ln::NodeId> const& pool
    , std::map<Ln::NodeId, Boss::Msg::TrackRecordVerdict> const& verdicts
    , std::set<Ln::NodeId> const& spliceable
    );

}

}}}

#endif /* !defined(BOSS_MOD_CHANNELCANDIDATEINVESTIGATOR_EVICTIONPOLICY_HPP) */
