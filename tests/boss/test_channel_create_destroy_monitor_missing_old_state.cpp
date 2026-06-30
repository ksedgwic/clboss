#undef NDEBUG
#include"Boss/Mod/ChannelCreateDestroyMonitor.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<memory>
#include<string>

/* Regression test for the CLN v26.06 compatibility branch in
 * Boss::Mod::ChannelCreateDestroyMonitor that tolerates
 * channel_state_changed notifications without an old_state field.
 *
 * Pre-v26.06 CLN emitted old_state="unknown" when a channel had
 * no prior state.  v25.05 deprecated that sentinel and v26.06+
 * drops the old_state field entirely on the same case.  The
 * handler must:
 *   - not throw on the missing field,
 *   - not emit a ChannelDestruction event (matching the legacy
 *     "unknown" behaviour, where the CHANNELD_NORMAL /
 *     CHANNELD_AWAITING_LOCKIN check would never have matched
 *     "unknown" either).
 */

int main() {
	auto bus = S::Bus();
	auto monitor = Boss::Mod::ChannelCreateDestroyMonitor(bus);

	auto destruction_count = std::make_shared<int>(0);
	bus.subscribe<Boss::Msg::ChannelDestruction
		     >([destruction_count](Boss::Msg::ChannelDestruction const& _) {
		*destruction_count = *destruction_count + 1;
		return Ev::lift();
	});

	auto const peer_str = std::string(
		"020000000000000000000000000000000000000000000000000000000000000000"
	);
	auto peer = Ln::NodeId(peer_str);

	auto code = Ev::lift().then([&]() {
		/* Seed the monitor's channeled set with `peer` and
		 * mark initted = true.  Without this, the
		 * notification handler would block in
		 * wait_for_true(initted).
		 */
		auto r = Boss::Msg::ListpeersAnalyzedResult{};
		r.connected_channeled.insert(peer);
		r.initial = true;
		return bus.raise(std::move(r));
	}).then([&]() {
		/* Build a channel_state_changed notification with
		 * NO old_state field -- the v26.06+ shape.
		 * new_state arbitrary; if the handler incorrectly
		 * parsed an empty old_state as matching one of the
		 * destruction-trigger states, we would see a
		 * ChannelDestruction event.
		 */
		auto json = std::string(
			"{\"channel_state_changed\":{"
				"\"peer_id\":\""
		) + peer_str + "\","
			"\"new_state\":\"ONCHAIN\""
			"}}";
		auto params = Jsmn::Object::parse_json(json.c_str());
		return bus.raise(Boss::Msg::Notification{
			std::string("channel_state_changed"),
			params
		});
	}).then([&]() {
		/* Pump a few event-loop ticks so the handler's
		 * wait_for_true polling and subsequent body run.
		 */
		return Ev::yield()
		     + Ev::yield()
		     + Ev::yield()
		     + Ev::yield();
	}).then([&]() {
		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);

	/* The destruction handler must NOT have fired.  empty
	 * old_state matches neither "CHANNELD_NORMAL" nor
	 * "CHANNELD_AWAITING_LOCKIN", so the handler returns
	 * Ev::lift() without doing anything.
	 */
	assert(*destruction_count == 0);
	return 0;
}
