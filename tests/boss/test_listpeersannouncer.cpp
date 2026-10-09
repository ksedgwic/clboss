#undef NDEBUG
#include"Boss/Mod/ListpeersAnalyzer.hpp"
#include"Boss/Mod/ListpeersAnnouncer.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/RequestRpcCommand.hpp"
#include"Boss/Msg/ResponseRpcCommand.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<memory>
#include<set>
#include<string>
#include<vector>

/* `listpeerchannels` has no entry for a peer without channels.  The
 * announcer takes those peers from `listpeers`, so that they reach
 * the connected_unchanneled bin (#355).  */

namespace {

/* A channel, connected.  */
auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000");
/* A channel, disconnected.  */
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
/* No channel, connected.  */
auto const C = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
/* Got its first channel between the two commands.  */
auto const D = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");

auto const listpeers = std::string(R"JSON(
{ "peers":
  [ { "id": "020000000000000000000000000000000000000000000000000000000000000000"
    , "connected": true
    , "num_channels": 1
    }
  , { "id": "020000000000000000000000000000000000000000000000000000000000000001"
    , "connected": false
    , "num_channels": 1
    }
  , { "id": "020000000000000000000000000000000000000000000000000000000000000002"
    , "connected": true
    , "num_channels": 0
    }
  , { "id": "020000000000000000000000000000000000000000000000000000000000000003"
    , "connected": true
    , "num_channels": 0
    }
  ]
}
)JSON");

auto const listpeerchannels = std::string(R"JSON(
{ "channels":
  [ { "peer_id": "020000000000000000000000000000000000000000000000000000000000000000"
    , "peer_connected": true
    , "state": "CHANNELD_NORMAL"
    }
  , { "peer_id": "020000000000000000000000000000000000000000000000000000000000000001"
    , "peer_connected": false
    , "state": "CHANNELD_NORMAL"
    }
  , { "peer_id": "020000000000000000000000000000000000000000000000000000000000000003"
    , "peer_connected": true
    , "state": "OPENINGD"
    }
  ]
}
)JSON");

}

int main() {
	auto bus = S::Bus();
	Boss::Mod::ListpeersAnnouncer announcer(bus);
	Boss::Mod::ListpeersAnalyzer analyzer(bus);
	(void) announcer;
	(void) analyzer;

	/* Mock the two commands.  */
	auto commands = std::vector<std::string>();
	bus.subscribe<Boss::Msg::RequestRpcCommand
		     >([&](Boss::Msg::RequestRpcCommand const& m) {
		commands.push_back(m.command);
		auto const& text = m.command == "listpeers"
				 ? listpeers
				 : listpeerchannels
				 ;

		auto response = Boss::Msg::ResponseRpcCommand();
		response.requester = m.requester;
		response.succeeded = true;
		response.result = Jsmn::Object::parse_json(text.c_str());

		return bus.raise(std::move(response));
	});

	auto result = std::unique_ptr<Boss::Msg::ListpeersResult>();
	bus.subscribe<Boss::Msg::ListpeersResult
		     >([&](Boss::Msg::ListpeersResult const& r) {
		result = std::make_unique<Boss::Msg::ListpeersResult>(r);
		return Ev::lift();
	});
	auto analyzed = std::unique_ptr<Boss::Msg::ListpeersAnalyzedResult>();
	bus.subscribe<Boss::Msg::ListpeersAnalyzedResult
		     >([&](Boss::Msg::ListpeersAnalyzedResult const& r) {
		analyzed = std::make_unique<Boss::Msg::ListpeersAnalyzedResult>(r);
		return Ev::lift();
	});

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::Timer10Minutes{});
	}).then([&]() {
		/* Let the module execute.  */
		return Ev::yield(25);
	}).then([&]() {
		typedef std::vector<std::string> Commands;
		assert(commands == Commands({"listpeers", "listpeerchannels"}));

		assert(result);
		assert(!result->initial);
		auto& cpeers = result->cpeers;
		assert(cpeers.size() == 4);
		assert(cpeers[A].connected);
		assert(cpeers[A].channels.size() == 1);
		assert(!cpeers[B].connected);
		assert(cpeers[B].channels.size() == 1);
		assert(cpeers[C].connected);
		assert(cpeers[C].channels.empty());
		assert(cpeers[D].connected);
		assert(cpeers[D].channels.size() == 1);

		typedef std::set<Ln::NodeId> Nodes;
		assert(analyzed);
		assert(analyzed->connected_channeled == Nodes({A, D}));
		assert(analyzed->disconnected_channeled == Nodes({B}));
		assert(analyzed->connected_unchanneled == Nodes({C}));
		assert(analyzed->disconnected_unchanneled.empty());

		return Ev::lift(0);
	});

	return Ev::start(code);
}
