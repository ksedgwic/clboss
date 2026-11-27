#ifndef BOSS_MOD_CHANNELCREATEDESTROYMONITOR_HPP
#define BOSS_MOD_CHANNELCREATEDESTROYMONITOR_HPP

#include"Ln/NodeId.hpp"
#include<map>
#include<set>
#include<string>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::ChannelCreateDestroyMonitor
 *
 * @brief module that monitors channel creation and
 * destruction events, and informs other modules of
 * these events.
 */
class ChannelCreateDestroyMonitor {
private:
	S::Bus& bus;
	bool initted;
	std::set<Ln::NodeId> channeled;
	/* The channel ids we believe are open or opening, per peer.
	 * A peer can have several channels, so a channel leaving the
	 * open states only destroys the peer's channeled status when
	 * it was the peer's last one.  */
	std::map<Ln::NodeId, std::set<std::string>> open_channels;
	/* The channel ids a `channel_state_changed` notification
	 * saw leave the open states.  A channel never comes back
	 * from there, so a listing that still shows one of them
	 * open was taken before the change, and is not believed
	 * for it.  An id goes once a listing no longer shows the
	 * channel open.  */
	std::set<std::string> closing;

	void start();

public:
	ChannelCreateDestroyMonitor() =delete;
	ChannelCreateDestroyMonitor(ChannelCreateDestroyMonitor&&) =delete;
	ChannelCreateDestroyMonitor(ChannelCreateDestroyMonitor const&)
		=delete;

	explicit
	ChannelCreateDestroyMonitor(S::Bus& bus_
				   ) : bus(bus_)
				     , initted(false)
				     {
		start();
	}
};

}}

#endif /* !defined(BOSS_MOD_CHANNELCREATEDESTROYMONITOR_HPP) */
