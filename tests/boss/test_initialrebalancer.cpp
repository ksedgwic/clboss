#undef NDEBUG
#include"Boss/Mod/InitialRebalancer.hpp"
#include"Boss/Mod/RebalanceUnmanager.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include"external/basicsecure/basicsecure.h"
#include<cstddef>
#include<iostream>
#include<sstream>

namespace {

class DummyEarningsManager {
private:
	S::Bus& bus;

	void start() {
		using Boss::Msg::RequestEarningsInfo;
		using Boss::Msg::ResponseEarningsInfo;
		bus.subscribe< RequestEarningsInfo
			     >([this](RequestEarningsInfo const& m) {
			return bus.raise(ResponseEarningsInfo{
				m.requester,
				m.node,
				Ln::Amount::sat(0),
				Ln::Amount::sat(0),
				Ln::Amount::sat(0),
				Ln::Amount::sat(0)
			});
		});
	}

public:
	DummyEarningsManager( S::Bus& bus_
			    ) : bus(bus_) { start(); }
};

/* First is source, second is destination.
 *
 * Third and fourth are dummy and are added to the unmanaged
 * list to check if InitialRebalancer respects the unmanaged
 * set.
 */
auto const peers = std::string(R"JSON(
[ { "id" : "020000000000000000000000000000000000000000000000000000000000000000"
  , "channels" : [ { "state" : "CHANNELD_NORMAL"
                   , "to_us_msat" : "1000000000msat"
                   , "total_msat" : "1000000000msat"
                   , "htlcs" : []
                   }
                 ]
  }
, { "id" : "020000000000000000000000000000000000000000000000000000000000000001"
  , "channels" : [ { "state" : "CHANNELD_NORMAL"
                   , "to_us_msat" : "0msat"
                   , "total_msat" : "1000000000msat"
                   , "htlcs" : []
                   }
                 ]
  }


, { "id" : "020000000000000000000000000000000000000000000000000000000000000003"
  , "channels" : [ { "state" : "CHANNELD_NORMAL"
                   , "to_us_msat" : "0msat"
                   , "total_msat" : "1000000000msat"
                   , "htlcs" : []
                   }
                 ]
  }
, { "id" : "020000000000000000000000000000000000000000000000000000000000000004"
  , "channels" : [ { "state" : "CHANNELD_NORMAL"
                   , "to_us_msat" : "1000000000msat"
                   , "total_msat" : "1000000000msat"
                   , "htlcs" : []
                   }
                 ]
  }
]
)JSON");

Ev::Io<void> listpeers_result(S::Bus& bus, std::string const& json) {
	using Boss::Msg::ListpeersResult;

	auto is = std::istringstream(json);
	auto js = Jsmn::Object();
	is >> js;

	return bus.raise(ListpeersResult{
				Boss::Mod::convert_legacy_listpeers(js), false});
}

Ev::Io<void> multiyield() {
	return Ev::yield(200);
}

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

}

int main() {
	using Boss::Msg::RequestMoveFunds;
	using Boss::Msg::ResponseMoveFunds;

	auto bus = S::Bus();

	/* Unmanager mock.  */
	Boss::Mod::RebalanceUnmanager unmanager(bus, {
		"020000000000000000000000000000000000000000000000000000000000000003",
		"020000000000000000000000000000000000000000000000000000000000000004"
	});

	DummyEarningsManager dummy1(bus);
	Cout cout(bus);

	auto num_move_funds = std::size_t(0);
	void* last_requester = nullptr;
	bus.subscribe< RequestMoveFunds
		     >([&](RequestMoveFunds const& m) {
		++num_move_funds;
		last_requester = m.requester;
		return Ev::lift();
	});

	auto mut = Boss::Mod::InitialRebalancer(bus);

	auto code = Ev::lift().then([&]() {

		/* Raise ListpeersResult once.  */
		return listpeers_result(bus, peers);
	}).then([&]() {
		/* Let InitialRebalancer work.  */
		return multiyield();
	}).then([&]() {
		assert(num_move_funds == 1);

		/* Raise ListpeersResult again.  */
		return listpeers_result(bus, peers);
	}).then([&]() {
		/* Let InitialRebalancer work.  */
		return multiyield();
	}).then([&]() {
		/* InitialRebalancer should have blocked.  */
		assert(num_move_funds == 1);

		/*  Now simulate the rebalance completing.  */
		return bus.raise(ResponseMoveFunds{
			last_requester,
			Ln::Amount::sat(0),
			Ln::Amount::sat(0)
		});
	}).then([&]() {

		/* Raise ListpeersResult once.  */
		return listpeers_result(bus, peers);
	}).then([&]() {
		/* Let InitialRebalancer work.  */
		return multiyield();
	}).then([&]() {
		/* Since the previous rebalance completed, this
		 * should increment.
		 */
		assert(num_move_funds == 2);

		/* Re-raising again should not cause an increment.  */
		return listpeers_result(bus, peers);
	}).then([&]() {
		/* Let InitialRebalancer work.  */
		return multiyield();
	}).then([&]() {
		/* InitialRebalancer should have blocked.  */
		assert(num_move_funds == 2);

		return Ev::lift(0);
	});

	return Ev::start(code);
}
