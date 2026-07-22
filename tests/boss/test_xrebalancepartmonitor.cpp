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

/* The plugin's Part::json shape, as captured live off a probe
 * subscriber (2026-07-23, lab0-a): plain-number msat fields, real
 * SCIDDs (direction-suffixed) on first_hop / return_hop, label
 * appended by the notifier.  This is the FLAT payload form, which
 * the cln-plugin crate deprecates (removal scheduled for CLN
 * 26.09); the monitor reads the nested form first and falls back
 * to this one.  */
auto const complete_part = R"JSON(
{
  "part_index": 2,
  "payment_hash": "f5a6a059a25d1e329d9b094aeeec8c2191ca037d3f5b0662e21ae850debe8ea2",
  "status": "complete",
  "first_hop": "1000x1x1/1",
  "return_hop": "1000x1x0/0",
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
  "first_hop": "1000x1x1/1",
  "return_hop": "1000x1x0/0",
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

/* The same route in the modern nested-only shape: what remains
 * after the crate drops the deprecated flat copy.  */
auto const nested_complete_part = R"JSON(
{
  "xrebalance_part": {
    "part_index": 3,
    "payment_hash": "a25d1e329d9b094aeeec8c2191ca037d3f5b0662e21ae850debe8ea2f5a6a059",
    "status": "complete",
    "first_hop": "1000x1x1/1",
    "return_hop": "1000x1x0/0",
    "planned_msat": 20000000,
    "delivered_msat": 20000000,
    "sent_msat": 20003000,
    "fee_msat": 3000,
    "hops_short": null,
    "failcode": null,
    "erring_scidd": null,
    "detail": null,
    "label": "ab12cd34"
  }
}
)JSON";

/* Both shapes at once: what cln-plugin 0.7.0 actually sends.  The
 * flat copy diverges here so the assertion proves the nested form
 * takes precedence; a real sender emits identical copies.  */
auto const double_shaped_part = R"JSON(
{
  "status": "complete",
  "first_hop": "1000x1x1/1",
  "return_hop": "1000x1x0/0",
  "delivered_msat": 1,
  "fee_msat": 1,
  "xrebalance_part": {
    "status": "complete",
    "first_hop": "1000x1x1/1",
    "return_hop": "1000x1x0/0",
    "delivered_msat": 30000000,
    "fee_msat": 4000
  }
}
)JSON";

/* A payload-shape change: right topic, expected fields gone.  Must
 * not attribute (the monitor logs it instead of dropping it
 * silently).  */
auto const malformed_part = R"JSON(
{ "surprise": true }
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

		/* The nested-only shape attributes identically.  */
		attribution = nullptr;
		return bus.raise(Boss::Msg::Notification{
			"xrebalance_part",
			Jsmn::Object::parse_json(nested_complete_part)
		});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(attribution);
		assert(attribution->source == Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001"));
		assert(attribution->destination == Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000"));
		assert(attribution->amount_moved == Ln::Amount::msat(20000000));
		assert(attribution->fee_spent == Ln::Amount::msat(3000));

		/* When both shapes are present, the nested one wins.  */
		attribution = nullptr;
		return bus.raise(Boss::Msg::Notification{
			"xrebalance_part",
			Jsmn::Object::parse_json(double_shaped_part)
		});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(attribution);
		assert(attribution->amount_moved == Ln::Amount::msat(30000000));
		assert(attribution->fee_spent == Ln::Amount::msat(4000));

		/* A malformed payload must not attribute (or crash).  */
		attribution = nullptr;
		return bus.raise(Boss::Msg::Notification{
			"xrebalance_part",
			Jsmn::Object::parse_json(malformed_part)
		});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(!attribution);
		return Ev::lift(0);
	});

	return Ev::start(code);
}
