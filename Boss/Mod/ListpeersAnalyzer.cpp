#include"Boss/Mod/ListpeersAnalyzer.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"

namespace Boss { namespace Mod {

ListpeersAnalyzer::ListpeersAnalyzer(S::Bus& bus) {
	bus.subscribe< Msg::ListpeersResult
		     >([&bus](Msg::ListpeersResult const& l) {
		auto ar = Msg::ListpeersAnalyzedResult();
		ar.initial = l.initial;

		for (auto peer : l.cpeers) {

			auto id = peer.first;
			auto connected = peer.second.connected;

			auto has_chan = bool(false);
			auto chans = peer.second.channels;
			for (auto chan : chans) {
				if (!chan.is_object() || !chan.has("state"))
					continue;

				auto state_j = chan["state"];
				if (!state_j.is_string())
					continue;
				auto state = std::string(state_j);
				auto prefix = std::string( state.begin()
							 , state.begin() + 8
							 );

				if ( prefix == "OPENINGD"
				  || prefix == "CHANNELD"
				   ) {
					has_chan = true;
					break;
				}
			}

			if (connected) {
				if (has_chan)
					ar.connected_channeled.emplace(id);
				else
					ar.connected_unchanneled.emplace(id);
			} else {
				if (has_chan)
					ar.disconnected_channeled.emplace(id);
				else
					ar.disconnected_unchanneled.emplace(id);
			}
		}

		ar.all_peers_disconnected =
			!ar.disconnected_channeled.empty()
		     && ar.connected_channeled.empty()
		     && ar.connected_unchanneled.empty()
		      ;

		return bus.raise(std::move(ar));
	});
}

}}
