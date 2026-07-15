#ifndef BOSS_MOD_CHANNELCREATOR_MANAGER_HPP
#define BOSS_MOD_CHANNELCREATOR_MANAGER_HPP

#include"Boss/Mod/ChannelCreator/Reprioritizer.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/RequestDowser.hpp"
#include"Boss/Msg/RequestPeerTrackRecord.hpp"
#include"Boss/Msg/ResponseDowser.hpp"
#include"Boss/Msg/ResponsePeerTrackRecord.hpp"
#include"Ln/NodeId.hpp"
#include<memory>
#include<utility>
#include<vector>

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator {
	class Main;
}}}
namespace Boss { namespace Mod { namespace ChannelCreator {
	class Carpenter;
}}}
namespace Boss { namespace Mod { class Rpc; }}
namespace Ev { template<typename a> class Io; }
namespace Ln { class Amount; }
namespace Ln { class NodeId; }
namespace Net { class IPAddrOrOnion; }
namespace S { class Bus; }

namespace Boss { namespace Mod { namespace ChannelCreator {

/** class Boss::Mod::ChannelCreator::Manager
 *
 * @brief submodule that handles incoming
 * messages to the channel creator, then
 * drives the rest of the processing.
 */
class Manager {
private:
	S::Bus& bus;
	Boss::Mod::Rpc* rpc;
	Boss::Mod::ChannelCandidateInvestigator::Main& investigator;
	Boss::Mod::ChannelCreator::Carpenter& carpenter;
	Ln::NodeId self;

	ModG::ReqResp<Msg::RequestDowser, Msg::ResponseDowser> dowser;
	ModG::ReqResp< Msg::RequestPeerTrackRecord
		     , Msg::ResponsePeerTrackRecord
		     > track_record;

	std::unique_ptr<Boss::Mod::ChannelCreator::Reprioritizer> reprioritizer;

	Ln::Amount min_amount;
	Ln::Amount max_amount;
	Ln::Amount min_remaining;

	void start();
	Ev::Io<void> on_request_channel_creation(Ln::Amount);

	/* Needed by reprioritizer.  */
	Ev::Io<std::unique_ptr<Net::IPAddrOrOnion>> get_node_addr(Ln::NodeId);
	Ev::Io<std::vector<Ln::NodeId>> get_peers();
	/* Perform reprioritization and log it.  */
	Ev::Io<std::vector<std::pair<Ln::NodeId, Ln::NodeId>>>
	reprioritize(std::vector<std::pair<Ln::NodeId, Ln::NodeId>>);
	/* Partition proposals by earnings track record and log it.  */
	Ev::Io<std::vector<std::pair<Ln::NodeId, Ln::NodeId>>>
	prioritize_by_track_record(std::vector<std::pair<Ln::NodeId, Ln::NodeId>>);

public:
	Manager() =delete;
	Manager(Manager const&) =delete;
	Manager(Manager&&) =delete;

	explicit
	Manager( S::Bus& bus_
	       , Boss::Mod::ChannelCandidateInvestigator::Main& investigator_
	       , Boss::Mod::ChannelCreator::Carpenter& carpenter_
	       ) : bus(bus_)
		 , rpc(nullptr)
		 , investigator(investigator_)
		 , carpenter(carpenter_)
		 , self()
		 , dowser(bus)
		 , track_record(bus)
		 {
		start();
	}
};

}}}

#endif /* !defined(BOSS_MOD_CHANNELCREATOR_MANAGER_HPP) */
