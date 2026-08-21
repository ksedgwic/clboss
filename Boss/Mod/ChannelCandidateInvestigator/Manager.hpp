#ifndef BOSS_MOD_CHANNELCANDIDATEINVESTIGATOR_MANAGER_HPP
#define BOSS_MOD_CHANNELCANDIDATEINVESTIGATOR_MANAGER_HPP

#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/RequestPeerTrackRecord.hpp"
#include"Boss/Msg/ResponsePeerTrackRecord.hpp"
#include"Sqlite3/Db.hpp"
#include"Ln/NodeId.hpp"
#include<cstddef>
#include<set>
#include<utility>
#include<vector>

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator { class Gumshoe; }}}
namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator { class Janitor; }}}
namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator { class Secretary; }}}
namespace Boss { namespace Mod { class InternetConnectionMonitor; }}
namespace Boss { namespace Mod { class Rpc; }}
namespace S { class Bus; }

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator {

/** class Boss::Mod::ChannelCandidateInvestigator::Manager
 *
 * @brief performs the heuristics needed to operate
 * the investigator.
 */
class Manager {
private:
	S::Bus& bus;
	Secretary& secretary;
	Janitor& janitor;
	Gumshoe& gumshoe;
	InternetConnectionMonitor& imon;

	Sqlite3::Db db;
	Boss::Mod::Rpc* rpc;

	std::set<Ln::NodeId> unmanaged;

	Ln::Amount min_channel;

	ModG::ReqResp< Msg::RequestPeerTrackRecord
		     , Msg::ResponsePeerTrackRecord
		     > track_record;

	void start();
	Ev::Io<void> solicit_candidates(std::size_t good_candidates);
	Ev::Io<std::pair<Ln::NodeId, char const*>>
	pick_eviction_victim(std::vector<Ln::NodeId> nodes);
	Ev::Io<std::set<Ln::NodeId>>
	spliceable_among(std::vector<Ln::NodeId> nodes);

public:
	Manager() =delete;
	Manager(Manager const&) =delete;
	Manager(Manager&&) =delete;

	explicit
	Manager( S::Bus& bus_
	       , Secretary& secretary_
	       , Janitor& janitor_
	       , Gumshoe& gumshoe_
	       , InternetConnectionMonitor& imon_
	       ) : bus(bus_)
		 , secretary(secretary_)
		 , janitor(janitor_)
		 , gumshoe(gumshoe_)
		 , imon(imon_)
		 , rpc(nullptr)
		 , track_record(bus_)
		 {
		start();
	}

	Ev::Io<std::vector<std::pair<Ln::NodeId, Ln::NodeId>>>
	get_channel_candidates();
};

}}}

#endif /* !defined(BOSS_MOD_CHANNELCANDIDATEINVESTIGATOR_MANAGER_HPP) */
