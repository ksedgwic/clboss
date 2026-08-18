#ifndef BOSS_MOD_XREBALANCEPARTMONITOR_HPP
#define BOSS_MOD_XREBALANCEPARTMONITOR_HPP

#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/RequestPeerFromScid.hpp"
#include"Boss/Msg/ResponsePeerFromScid.hpp"

namespace Ln { class Amount; }
namespace Ln { class NodeId; }
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::XRebalancePartMonitor
 *
 * @brief Monitors the external xrebalance plugin's
 * `xrebalance_part` notifications and raises
 * `Boss::Msg::XRebalanceAttribution` for each completed part,
 * so EarningsTracker accounts funds the plugin moves --
 * whether the `xrebalance` mode or some other plugin client
 * initiated the transfer.
 *
 * The notification's `first_hop` and `return_hop` are real
 * scids of our own channels; the mapper resolves them to the
 * source / destination peers the attribution schema wants.
 */
class XRebalancePartMonitor {
private:
	S::Bus& bus;

	Boss::ModG::ReqResp< Msg::RequestPeerFromScid
			   , Msg::ResponsePeerFromScid
			   > peer_from_scid_rr;

	void start();
	Ev::Io<void> cont( Ln::NodeId source
			 , Ln::NodeId destination
			 , Ln::Amount amount
			 , Ln::Amount fee
			 );

public:
	XRebalancePartMonitor() =delete;
	XRebalancePartMonitor(XRebalancePartMonitor&&) =delete;
	XRebalancePartMonitor(XRebalancePartMonitor const&) =delete;

	explicit
	XRebalancePartMonitor(S::Bus& bus_
			     ) : bus(bus_)
			       , peer_from_scid_rr(bus_)
			       { start(); }
};

}}

#endif /* !defined(BOSS_MOD_XREBALANCEPARTMONITOR_HPP) */
