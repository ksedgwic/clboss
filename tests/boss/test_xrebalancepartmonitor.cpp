#undef NDEBUG
#include"Boss/Mod/JsonOutputter.hpp"
#include"Boss/Mod/PeerFromScidMapper.hpp"
#include"Boss/Mod/XRebalancePartMonitor.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Boss/Msg/XRebalanceAttribution.hpp"
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
                           ]
             }
           ]
}
)JSON";

/* The plugin's Part::json shape: plain-number msat fields, real scids
 * on first_hop / return_hop, label appended by the notifier.  Custom
 * notifications deliver this AS params (lightningd relays the sender's
 * payload verbatim; no topic-key nesting, that is a built-in-topic
 * convention).  */
auto const complete_part = R"JSON(
{
  "part_index": 2,
  "payment_hash": "f5a6a059a25d1e329d9b094aeeec8c2191ca037d3f5b0662e21ae850debe8ea2",
  "status": "complete",
  "first_hop": "1000x1x1",
  "return_hop": "1000x1x0",
  "planned_msat": 10000000,
  "delivered_msat": 10000000,
  "sent_msat": 10005958,
  "fee_msat": 5958,
  "hops_short": null,
  "failcode": null,
  "erring_scidd": null,
  "detail": null,
  "label": "ab12cd34"
}
)JSON";

auto const failed_part = R"JSON(
{
  "part_index": 1,
  "payment_hash": "9d9b094aeeec8c2191ca037d3f5b0662e21ae850debe8ea2f5a6a059a25d1e32",
  "status": "failed",
  "first_hop": "1000x1x1",
  "return_hop": "1000x1x0",
  "planned_msat": 10000000,
  "delivered_msat": 0,
  "sent_msat": 10005958,
  "fee_msat": 0,
  "hops_short": 4,
  "failcode": 4103,
  "erring_scidd": "2000x1x0/1",
  "detail": null,
  "label": "ab12cd34"
}
)JSON";

}

int main() {
	auto bus = S::Bus();

	/* Utility outputter.  */
	Boss::Mod::JsonOutputter cout(std::cout, bus);

	/* Module under test.  */
	Boss::Mod::XRebalancePartMonitor mut(bus);

	/* Utility.  */
	Boss::Mod::PeerFromScidMapper mapper(bus);

	/* Should occur once.  */
	auto got_manifest_notification = false;
	bus.subscribe<Boss::Msg::ManifestNotification
		     >([&](Boss::Msg::ManifestNotification const& m) {
		assert(!got_manifest_notification);
		assert(m.name == "xrebalance_part");
		got_manifest_notification = true;
		return Ev::lift();
	});

	/* Monitor XRebalanceAttribution messages.  */
	auto attribution = std::unique_ptr<Boss::Msg::XRebalanceAttribution>();
	bus.subscribe<Boss::Msg::XRebalanceAttribution
		     >([&](Boss::Msg::XRebalanceAttribution const& m) {
		attribution = Util::make_unique<
			Boss::Msg::XRebalanceAttribution>(m);
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

		/* Should ignore other notifications.  */
		attribution = nullptr;
		return bus.raise(Boss::Msg::Notification{
			"not-xrebalance_part", Jsmn::Object::parse_json("{}")
		});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(!attribution);

		/* Should ignore failed parts.  */
		attribution = nullptr;
		return bus.raise(Boss::Msg::Notification{
			"xrebalance_part",
			Jsmn::Object::parse_json(failed_part)
		});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(!attribution);

		/* A completed part attributes: source = first_hop peer,
		 * destination = return_hop peer.  */
		attribution = nullptr;
		return bus.raise(Boss::Msg::Notification{
			"xrebalance_part",
			Jsmn::Object::parse_json(complete_part)
		});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(attribution);
		assert(attribution->source == Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001"));
		assert(attribution->destination == Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000"));
		assert(attribution->amount_moved == Ln::Amount::msat(10000000));
		assert(attribution->fee_spent == Ln::Amount::msat(5958));
		return Ev::lift(0);
	});

	return Ev::start(code);
}
