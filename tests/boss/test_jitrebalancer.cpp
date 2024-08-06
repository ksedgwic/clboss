#undef NDEBUG
#include"Boss/Mod/JitRebalancer.hpp"
#include"Boss/Mod/PeerFromScidMapper.hpp"
#include"Boss/Mod/RebalanceUnmanager.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/ProvideHtlcAcceptedDeferrer.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/RequestRpcCommand.hpp"
#include"Boss/Msg/ReleaseHtlcAccepted.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/Msg/ResponseRpcCommand.hpp"
#include"Boss/Msg/SolicitHtlcAcceptedDeferrer.hpp"
#include"Ev/Io.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/foreach.hpp"
#include"Ev/map.hpp"
#include"Ev/now.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/HtlcAccepted.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<functional>
#include<set>
#include<sstream>
#include<vector>

namespace {

auto const feerates_result = R"JSON(
{ "perkw": { "unilateral_close": 253
           }
}
)JSON";

auto const listpeers_result = R"JSON(
{ "peers": [ { "id": "020000000000000000000000000000000000000000000000000000000000000000"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "to_us_msat":  "750000000msat"
                             , "total_msat": "1000000000msat"
                             , "short_channel_id": "1000x1x0"
                             }
                           ]
             }
           , { "id": "020000000000000000000000000000000000000000000000000000000000000001"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "to_us_msat": "0msat"
                             , "total_msat": "1000000000msat"
                             , "short_channel_id": "1000x1x1"
                             }
                           , { "state": "ONCHAIN"
                             , "to_us_msat": "1000000000msat"
                             , "total_msat": "1000000000msat"
                             , "short_channel_id": "999x1x1"
                             }
                           ]
             }
           , { "id": "020000000000000000000000000000000000000000000000000000000000000002"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "to_us_msat":      "80000msat"
                             , "total_msat": "1000000000msat"
                             , "short_channel_id": "1000x1x2"
                             }
                           ]
             }

           , { "id": "02000000000000000000000000000000000000000000000000000000000000FF00"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "to_us_msat":  "900000000msat"
                             , "total_msat": "1000000000msat"
                             , "short_channel_id": "9999x1x0"
                             }
                           ]
             }
           , { "id": "02000000000000000000000000000000000000000000000000000000000000FF01"
             , "channels": [ { "state": "CHANNELD_NORMAL"
                             , "to_us_msat":  "900000000msat"
                             , "total_msat": "1000000000msat"
                             , "short_channel_id": "9999x1x1"
                             }
                           ]
             }
           ]
}
)JSON";

auto const listpeerchannels_result = R"JSON(
{
  "channels": [
    {
      "state": "CHANNELD_NORMAL",
      "to_us_msat": "750000000msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "1000x1x0",
      "peer_id": "020000000000000000000000000000000000000000000000000000000000000000",
      "peer_connected": true
    },
    {
      "state": "CHANNELD_NORMAL",
      "to_us_msat": "0msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "1000x1x1",
      "peer_id": "020000000000000000000000000000000000000000000000000000000000000001",
      "peer_connected": true
    },
    {
      "state": "ONCHAIN",
      "to_us_msat": "1000000000msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "999x1x1",
      "peer_id": "020000000000000000000000000000000000000000000000000000000000000001",
      "peer_connected": true
    },
    {
      "state": "CHANNELD_NORMAL",
      "to_us_msat": "80000msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "1000x1x2",
      "peer_id": "020000000000000000000000000000000000000000000000000000000000000002",
      "peer_connected": true
    },
    {
      "state": "CHANNELD_NORMAL",
      "to_us_msat": "900000000msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "9999x1x0",
      "peer_id": "02000000000000000000000000000000000000000000000000000000000000FF00",
      "peer_connected": true
    },
    {
      "state": "CHANNELD_NORMAL",
      "to_us_msat": "900000000msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "9999x1x1",
      "peer_id": "02000000000000000000000000000000000000000000000000000000000000FF01",
      "peer_connected": true
    }
  ]
}
)JSON";

class DummyEarningsManager {
private:
	S::Bus& bus;

	void start() {
		using Boss::Msg::RequestEarningsInfo;
		using Boss::Msg::ResponseEarningsInfo;
		bus.subscribe< RequestEarningsInfo
			     >([this](RequestEarningsInfo const& m) {
			/* Give inflated earnings.  */
			return bus.raise(ResponseEarningsInfo{
				m.requester,
				m.node,
				Ln::Amount::sat(10000),
				Ln::Amount::sat(0),
				Ln::Amount::sat(10000),
				Ln::Amount::sat(0)
			});
		});
	}

public:
	DummyEarningsManager( S::Bus& bus_
			    ) : bus(bus_) { start(); }
};

class DummyRpc {
private:
	S::Bus& bus;

	Ev::Io<void> respond(void* requester, char const* res) {
		return bus.raise(Boss::Msg::ResponseRpcCommand{
			requester, true,
			Jsmn::Object::parse_json(res),
			""
		});
	}

public:
	DummyRpc(S::Bus& bus_) : bus(bus_) {
		bus.subscribe< Boss::Msg::RequestRpcCommand
			     >([ this
			       ](Boss::Msg::RequestRpcCommand const& m) {
			if (m.command == "listpeers") {
				return respond(m.requester, listpeers_result);
			} else if (m.command == "listpeerchannels") {
				return respond(m.requester, listpeerchannels_result);
			} else if (m.command == "feerates") {
				return respond(m.requester, feerates_result);
			} else {
				/* Unmocked command.  */
				std::cerr << "COMMAND WAS " << m.command << std::endl;
				assert(0);
				return Ev::lift();
			}
		});
	}
};

class Cout {
private:
	S::Bus& bus;

public:
	Cout(S::Bus& bus_) : bus(bus_) {
		bus.subscribe< Boss::Msg::JsonCout
			     >([](Boss::Msg::JsonCout const& m) {
			std::cout << m.obj.output() << std::endl;
			return Ev::lift();
		});
	}
};

/* Construct a Ln::HtlcAccepted::Request filling in only the details
 * JitRebalancer cares about.
 */
Ln::HtlcAccepted::Request htlc( char const* next_channel
			      , Ln::Amount next_amount
			      , std::uint64_t id
			      ) {
	auto rv = Ln::HtlcAccepted::Request();
	rv.next_channel = next_channel ? Ln::Scid(std::string(next_channel)) : nullptr;
	rv.next_amount = next_amount;
	rv.id = Ln::CommandId::left(id);
	return rv;
}

/* Wait for Boss::Msg::ReleaseHtlcAccepted, which should be
 * continue always.
 */
class ReleaseMonitor {
private:
	S::Bus& bus;
	std::set<std::uint64_t> ids;

	void start() {
		bus.subscribe<Boss::Msg::ReleaseHtlcAccepted
			     >([this](Boss::Msg::ReleaseHtlcAccepted const& m) {
			assert(m.response.is_cont());
			auto id = m.response.id();
			id.cmatch([this](std::uint64_t nid) {
				ids.insert(nid);
			}, [](std::string const& _) {
				assert(false);
			});
			return Ev::yield();
		});
	}
	Ev::Io<void> loop(std::uint64_t start, std::uint64_t expected_id) {
		return Ev::yield().then([this, start, expected_id]() {
			auto it = ids.find(expected_id);
			if (it != ids.end()) {
				ids.erase(it);
				return Ev::lift();
			}
			assert(Ev::now() - start < 5.0); /* Time out.  */
			return loop(start, expected_id);
		});
	}

public:
	explicit
	ReleaseMonitor(S::Bus& bus_) : bus(bus_) { start(); }

	Ev::Io<void> wait_release(std::uint64_t expected_id) {
		return loop(Ev::now(), expected_id);
	}
};

Ev::Io<void> multiyield() {
	return Ev::yield(100);
}

}

int main() {
	using Boss::Msg::RequestMoveFunds;
	using Boss::Msg::ResponseMoveFunds;

	auto bus = S::Bus();

	Cout cout(bus);
	/* Needed utility module.  */
	auto mapper = Boss::Mod::PeerFromScidMapper(bus);

	/* Instantiate the various mocks.  */
	DummyRpc rpc(bus);
	DummyEarningsManager earnings_manager(bus);
	ReleaseMonitor release_monitor(bus);
	Boss::Mod::RebalanceUnmanager unmanager(bus, {
		"02000000000000000000000000000000000000000000000000000000000000FF00",
		"02000000000000000000000000000000000000000000000000000000000000FF01"
	});

	/* Simple facility to check for deferrer function.  */
	auto deferrer = std::function<Ev::Io<bool>(Ln::HtlcAccepted::Request const&)>();
	bus.subscribe<Boss::Msg::ProvideHtlcAcceptedDeferrer
		     >([&](Boss::Msg::ProvideHtlcAcceptedDeferrer const& m) {
		assert(!deferrer);
		deferrer = m.deferrer;
		return Ev::lift();
	});

	/* Simple facility to check for requests to move funds.  */
	auto num_move_funds = std::size_t(0);
	void* requester = nullptr;
	auto source = Ln::NodeId();
	auto destination = Ln::NodeId();
	bus.subscribe< RequestMoveFunds
		     >([&](RequestMoveFunds const& m) {
		++num_move_funds;
		requester = m.requester;
		source = m.source;
		destination = m.destination;
		return Ev::lift();
	});

	/* Module under test.  */
	auto mut = Boss::Mod::JitRebalancer(bus);

	auto code = Ev::lift().then([&] {

		/* Solicit for deferrers --- the JitRebalancer
		 * should provide exactly one.
		 */
		return bus.raise(Boss::Msg::SolicitHtlcAcceptedDeferrer{});
	}).then([&]() {
		/* Check a deferrer was indeed provided.  */
		assert(deferrer);

		/* Raise the ListpeersResult.  */
		auto res = Jsmn::Object::parse_json(listpeers_result);
		auto peers = res["peers"];
		return bus.raise(Boss::Msg::ListpeersResult{
				std::move(Boss::Mod::convert_legacy_listpeers(peers)), true
		});
	}).then([&]() {

		/* If not a forward, JitRebelancer should ignore.  */
		return deferrer(htlc(nullptr, Ln::Amount::msat(42), 1));
	}).then([&](bool flag) {
		assert(flag == false);

		/* If a forward, but amount fits, JitRebalancer should
		 * let it through almost immediately.  */
		return deferrer(htlc("1000x1x0", Ln::Amount::msat(1), 2));
	}).then([&](bool flag) {
		assert(flag == true);
		return release_monitor.wait_release(2);
	}).then([&]() {
		assert(num_move_funds == 0);

		/* Check parallel calls.  */
		auto ids = std::vector<std::uint64_t>{3, 4, 5};
		auto act = Ev::lift();
		/* Perform parallel calls.  */
		act += Ev::concurrent(Ev::map([&](std::uint64_t id) {
			return deferrer(htlc("1000x1x0", Ln::Amount::msat(1), id));
		}, ids).then([&](std::vector<bool> flags) {
			/* Every forward should get in.  */
			for (auto flag : flags)
				assert(flag);
			return Ev::lift();
		}));
		act += Ev::yield();
		act += Ev::foreach([&](std::uint64_t id) {
			return release_monitor.wait_release(id);
		}, ids);
		return act;
	}).then([&]() {
		assert(num_move_funds == 0);

		/* Check for a forward that does not fit.  */
		return deferrer(htlc("1000x1x1", Ln::Amount::msat(90000), 6));
	}).then([&](bool flag) {
		assert(flag == true);
		/* Wait for move-funds request.  */
		return multiyield();
	}).then([&]() {
		assert(num_move_funds == 1);
		/* The 02 would not have fit.  */
		assert(source == Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000"));
		assert(destination == Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001"));
		/* Now respond to the move-funds request.  */
		return bus.raise(ResponseMoveFunds{
			requester,
			Ln::Amount::sat(0),
			Ln::Amount::sat(0)
		});
	}).then([&]() {
		/* We should then release.  */
		return release_monitor.wait_release(6);
	}).then([&]() {

		/* Check for a forward to an unmanaged
		 * node.
		 */
		num_move_funds = 0;
		return deferrer(htlc("9999x1x0", Ln::Amount::msat(900000000), 7));
	}).then([&](bool flag) {
		assert(flag == false);

		return Ev::lift(0);
	});

	return Ev::start(code);
}
