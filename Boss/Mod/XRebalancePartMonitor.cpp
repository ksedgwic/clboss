#include"Boss/Mod/XRebalancePartMonitor.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Boss/Msg/XRebalanceAttribution.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/map.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Util/stringify.hpp"
#include<vector>

namespace Boss { namespace Mod {

void XRebalancePartMonitor::start() {
	bus.subscribe<Msg::Manifestation
		     >([this](Msg::Manifestation const& _) {
		/* lightningd only warns about subscriptions to topics no
		 * loaded plugin provides, so this is safe without the
		 * xrebalance plugin present.  */
		return bus.raise(Msg::ManifestNotification{
			"xrebalance_part"
		});
	});
	bus.subscribe<Msg::Notification
		     >([this](Msg::Notification const& n) {
		if (n.notification != "xrebalance_part")
			return Ev::lift();

		auto first_scid = Ln::Scid();
		auto return_scid = Ln::Scid();
		auto amount = Ln::Amount();
		auto fee = Ln::Amount();
		try {
			auto payload = n.params["xrebalance_part"];
			if ( !payload.has("status")
			  || !payload.has("first_hop")
			  || !payload.has("return_hop")
			  || !payload.has("delivered_msat")
			  || !payload.has("fee_msat")
			   )
				return Ev::lift();
			/* Only completed parts carry earnings; failed and
			 * pending parts are the plugin's business.  */
			if (std::string(payload["status"]) != "complete")
				return Ev::lift();

			first_scid = Ln::Scid(std::string(
				payload["first_hop"]
			));
			return_scid = Ln::Scid(std::string(
				payload["return_hop"]
			));
			amount = Ln::Amount::object(
				payload["delivered_msat"]
			);
			fee = Ln::Amount::object(
				payload["fee_msat"]
			);
		} catch (std::runtime_error const& err) {
			return Boss::log( bus, Error
					, "XRebalancePartMonitor: unexpected "
					  "xrebalance_part payload: %s: %s"
					, Util::stringify(n.params).c_str()
					, err.what()
					);
		}

		auto f = [this](Ln::Scid scid) {
			return peer_from_scid_rr.execute(Msg::RequestPeerFromScid{
				nullptr, scid
			}).then([](Msg::ResponsePeerFromScid r) {
				return Ev::lift(std::move(r.peer));
			});
		};
		auto scids = std::vector<Ln::Scid>{first_scid, return_scid};
		return Ev::map( std::move(f), std::move(scids)
			      ).then([ this
				     , amount
				     , fee
				     ](std::vector<Ln::NodeId> nids) {
			return cont( std::move(nids[0])
				   , std::move(nids[1])
				   , amount
				   , fee
				   );
		});
	});
}

Ev::Io<void> XRebalancePartMonitor::cont( Ln::NodeId source
					, Ln::NodeId destination
					, Ln::Amount amount
					, Ln::Amount fee
					) {
	if (!source || !destination)
		/* Funds moved but the channel is gone from
		 * listpeerchannels (closed between part completion and
		 * this lookup); the earnings go unattributed.  */
		return Boss::log( bus, Warn
				, "XRebalancePartMonitor: completed part on "
				  "unknown channel, not attributed."
				);

	auto act = Ev::lift();
	act += Boss::log( bus, Debug
			, "XRebalancePartMonitor: %s -> %s, "
			  "moved %s, fee %s."
			, std::string(source).c_str()
			, std::string(destination).c_str()
			, std::string(amount).c_str()
			, std::string(fee).c_str()
			);
	act += Boss::concurrent(bus.raise(Msg::XRebalanceAttribution{
		std::move(source),
		std::move(destination),
		amount,
		fee
	}));
	return act;
}

}}
