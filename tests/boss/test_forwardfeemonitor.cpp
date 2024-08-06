#undef NDEBUG
#include"Boss/Mod/ForwardFeeMonitor.hpp"
#include"Boss/Mod/JsonOutputter.hpp"
#include"Boss/Mod/PeerFromScidMapper.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<sstream>

namespace {

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
           ]
}
)JSON";

}

int main() {
	auto bus = S::Bus();

	/* Utility outputter.  */
	Boss::Mod::JsonOutputter cout(std::cout, bus);

	/* Module under test.  */
	Boss::Mod::ForwardFeeMonitor mut(bus);

	/* Utility.  */
	Boss::Mod::PeerFromScidMapper mapper(bus);

	/* Should occur once.  */
	auto got_manifest_notification = false;
	bus.subscribe<Boss::Msg::ManifestNotification
		     >([&](Boss::Msg::ManifestNotification const& m) {
		assert(!got_manifest_notification);
		assert(m.name == "forward_event");
		got_manifest_notification = true;
		return Ev::lift();
	});

	/* Monitor ForwardFee messages.  */
	auto forwardfee = std::unique_ptr<Boss::Msg::ForwardFee>();
	bus.subscribe<Boss::Msg::ForwardFee
		     >([&](Boss::Msg::ForwardFee const& m) {
		forwardfee = Util::make_unique<Boss::Msg::ForwardFee>(m);
		return Ev::lift();
	});

	/* Test.  */
	auto code = Ev::lift().then([&]() {

		/* Trigger manifestation.  */
		return bus.raise(Boss::Msg::Manifestation{});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(got_manifest_notification);

		/* Give the peers.  */
		return bus.raise(Boss::Msg::ListpeersResult{
				Boss::Mod::convert_legacy_listpeers(Jsmn::Object::parse_json(listpeers_result)["peers"]), true
		});
	}).then([&]() {

		/* Should ignore non-forward_event.  */
		forwardfee = nullptr;
		return bus.raise(Boss::Msg::Notification{
			"not-forward_event", Jsmn::Object::parse_json("{}")
		});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(!forwardfee);

		/* Check legit forward_event.  */
		forwardfee = nullptr;
		return bus.raise(Boss::Msg::Notification{
			"forward_event",
			Jsmn::Object::parse_json(R"JSON(
{
  "forward_event": {
    "payment_hash": "f5a6a059a25d1e329d9b094aeeec8c2191ca037d3f5b0662e21ae850debe8ea2",
    "in_channel": "1000x1x1",
    "out_channel": "1000x1x0",
    "in_msatoshi": 100001001,
    "in_msat": "100001001msat",
    "out_msatoshi": 100000000,
    "out_msat": "100000000msat",
    "fee_msat": "1001msat",
    "status": "settled",
    "received_time": 1560696342.368,
    "resolved_time": 1560696342.556
  }
}
			)JSON")
		});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(forwardfee);
		assert(forwardfee->in_id == Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001"));
		assert(forwardfee->out_id == Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000"));
		assert(forwardfee->fee == Ln::Amount::msat(1001));
		assert(forwardfee->resolution_time == (1560696342.556 - 1560696342.368));

		return Ev::lift(0);
	});

	return Ev::start(code);
}
