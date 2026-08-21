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
		/* first_hop / return_hop are scidds ("845x1x0/1"); the
		 * mapper keys on the channel alone, so drop the
		 * direction suffix.  */
		auto scid_of_scidd = [](std::string const& s) {
			auto slash = s.find('/');
			return Ln::Scid( slash == std::string::npos
				       ? s : s.substr(0, slash));
		};
		try {
			/* lightningd relays the sender's payload verbatim
			 * as params (origin as a sibling field).  Within
			 * it the cln-plugin crate double-shapes: the
			 * fields flat (deprecated, removal scheduled for
			 * CLN 26.09) plus a copy nested under the topic
			 * key.  Read the nested form first so a crate
			 * upgrade that drops the flat copy cannot end
			 * attribution.  */
			auto payload = n.params;
			if ( payload.has("xrebalance_part")
			  && payload["xrebalance_part"].is_object()
			   )
				payload = payload["xrebalance_part"];
			/* The plugin's Part::json always carries these
			 * fields, so a miss is a payload-shape change,
			 * not a routine part; a silent return here is
			 * how attribution dies unnoticed.  */
			if ( !payload.has("status")
			  || !payload.has("first_hop")
			  || !payload.has("return_hop")
			  || !payload.has("delivered_msat")
			  || !payload.has("fee_msat")
			   )
				return Boss::log( bus, Error
						, "XRebalancePartMonitor: "
						  "xrebalance_part payload "
						  "missing expected fields: %s"
						, Util::stringify(n.params).c_str()
						);
			/* Only completed parts carry earnings; failed and
			 * pending parts are the plugin's business.  */
			if (std::string(payload["status"]) != "complete")
				return Ev::lift();

			first_scid = scid_of_scidd(std::string(
				payload["first_hop"]
			));
			return_scid = scid_of_scidd(std::string(
				payload["return_hop"]
			));
			amount = Ln::Amount::object(
				payload["delivered_msat"]
			);
			fee = Ln::Amount::object(
				payload["fee_msat"]
			);
		/* std::exception, not std::runtime_error: Ln::Scid
		 * throws invalid_argument (a logic_error), and a
		 * narrower catch let exactly that escape unlogged --
		 * the silent-attribution-loss bug.  */
		} catch (std::exception const& err) {
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
