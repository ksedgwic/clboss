#undef NDEBUG
#include"Boss/Mod/ConstructedListpeers.hpp"
#include"Boss/Mod/ListpeersAnalyzer.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<memory>
#include<sstream>
#include<string>

/* Checks the bins and the all_peers_disconnected flag the analyzer
 * derives from a listpeers result (#346).  */

int main() {
	auto bus = S::Bus();
	Boss::Mod::ListpeersAnalyzer analyzer(bus);
	(void) analyzer;

	auto received = std::unique_ptr<Boss::Msg::ListpeersAnalyzedResult>();
	bus.subscribe<Boss::Msg::ListpeersAnalyzedResult
		     >([&](Boss::Msg::ListpeersAnalyzedResult const& r) {
		received = std::make_unique<Boss::Msg::ListpeersAnalyzedResult>(r);
		return Ev::lift();
	});

	auto listpeers = [&](std::string peers_json) {
		received = nullptr;
		auto is = std::stringstream(std::move(peers_json));
		auto peers = Jsmn::Object();
		is >> peers;
		return bus.raise(Boss::Msg::ListpeersResult{
			Boss::Mod::convert_legacy_listpeers(peers), false
		});
	};

	auto code = Ev::lift().then([&]() {
		/* No peers at all: nothing to be disconnected from.  */
		return listpeers(R"JSON(
			[]
		)JSON");
	}).then([&]() {
		assert(received);
		assert(!received->all_peers_disconnected);

		/* Every channeled peer disconnected, no other
		 * connection: a blackout.  */
		return listpeers(R"JSON(
			[ { "id": "020000000000000000000000000000000000000000000000000000000000000000"
			  , "connected": false
			  , "channels": [ { "state": "CHANNELD_NORMAL" } ]
			  }
			, { "id": "020000000000000000000000000000000000000000000000000000000000000001"
			  , "connected": false
			  , "channels": [ { "state": "CHANNELD_AWAITING_LOCKIN" } ]
			  }
			]
		)JSON");
	}).then([&]() {
		assert(received);
		assert(received->disconnected_channeled.size() == 2);
		assert(received->connected_channeled.empty());
		assert(received->all_peers_disconnected);

		/* One channeled peer connected: not a blackout.  */
		return listpeers(R"JSON(
			[ { "id": "020000000000000000000000000000000000000000000000000000000000000000"
			  , "connected": false
			  , "channels": [ { "state": "CHANNELD_NORMAL" } ]
			  }
			, { "id": "020000000000000000000000000000000000000000000000000000000000000001"
			  , "connected": true
			  , "channels": [ { "state": "CHANNELD_NORMAL" } ]
			  }
			]
		)JSON");
	}).then([&]() {
		assert(received);
		assert(received->disconnected_channeled.size() == 1);
		assert(received->connected_channeled.size() == 1);
		assert(!received->all_peers_disconnected);

		/* Every channeled peer disconnected, but an unchanneled
		 * peer is connected: we are reachable, so the channeled
		 * peer's disconnection is its own.  */
		return listpeers(R"JSON(
			[ { "id": "020000000000000000000000000000000000000000000000000000000000000000"
			  , "connected": false
			  , "channels": [ { "state": "CHANNELD_NORMAL" } ]
			  }
			, { "id": "020000000000000000000000000000000000000000000000000000000000000001"
			  , "connected": true
			  , "channels": [ ]
			  }
			]
		)JSON");
	}).then([&]() {
		assert(received);
		assert(received->disconnected_channeled.size() == 1);
		assert(received->connected_unchanneled.size() == 1);
		assert(!received->all_peers_disconnected);

		/* Only unchanneled peers, all disconnected: no channeled
		 * peer to be disconnected from.  */
		return listpeers(R"JSON(
			[ { "id": "020000000000000000000000000000000000000000000000000000000000000000"
			  , "connected": false
			  , "channels": [ { "state": "ONCHAIN" } ]
			  }
			]
		)JSON");
	}).then([&]() {
		assert(received);
		assert(received->disconnected_unchanneled.size() == 1);
		assert(!received->all_peers_disconnected);

		return Ev::lift(0);
	});
	return Ev::start(code);
}
