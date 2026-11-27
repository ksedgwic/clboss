#undef NDEBUG
#include"Boss/Mod/ChannelCreateDestroyMonitor.hpp"
#include"Boss/Mod/ConstructedListpeers.hpp"
#include"Boss/Msg/ChannelCreation.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
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

/* A splice is not a channel loss.  The channel goes from
 * CHANNELD_NORMAL to CHANNELD_AWAITING_SPLICE and back, and keeps
 * its channel id.  Neither the notifications nor a listing taken
 * at any point of it may announce a destruction or a creation.
 * A channel that closes during a splice is destroyed once, and a
 * listing from before the close, which still shows the splice
 * state, does not bring it back.
 */

namespace {

auto const peer_str = std::string(
	"020000000000000000000000000000000000000000000000000000000000000000"
);
auto const chan_a = std::string(
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
);

Ev::Io<void> listing( S::Bus& bus
		    , Ln::NodeId const& peer
		    , std::string const& state
		    , bool initial
		    ) {
	auto cpeers = Boss::Mod::ConstructedListpeers();
	cpeers[peer].connected = true;
	auto json = std::string("{\"channel_id\":\"") + chan_a + "\","
		    "\"state\":\"" + state + "\"}";
	cpeers[peer].channels.push_back(
		Jsmn::Object::parse_json(json.c_str()));
	return bus.raise(Boss::Msg::ListpeersResult{
		std::move(cpeers), initial
	}) + Ev::yield() + Ev::yield() + Ev::yield() + Ev::yield();
}

Ev::Io<void> state_changed( S::Bus& bus
			  , std::string const& old_state
			  , std::string const& new_state
			  ) {
	auto json = std::string("{\"channel_state_changed\":{"
				"\"peer_id\":\"") + peer_str + "\","
		    "\"channel_id\":\"" + chan_a + "\","
		    "\"old_state\":\"" + old_state + "\","
		    "\"new_state\":\"" + new_state + "\""
		    "}}";
	auto params = Jsmn::Object::parse_json(json.c_str());
	return bus.raise(Boss::Msg::Notification{
		std::string("channel_state_changed"),
		params
	}) + Ev::yield() + Ev::yield() + Ev::yield() + Ev::yield();
}

}

int main() {
	auto bus = S::Bus();
	auto monitor = Boss::Mod::ChannelCreateDestroyMonitor(bus);

	auto created = std::make_shared<int>(0);
	auto destroyed = std::make_shared<int>(0);
	bus.subscribe<Boss::Msg::ChannelCreation
		     >([created](Boss::Msg::ChannelCreation const& _) {
		++*created;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::ChannelDestruction
		     >([destroyed](Boss::Msg::ChannelDestruction const& _) {
		++*destroyed;
		return Ev::lift();
	});

	auto peer = Ln::NodeId(peer_str);

	auto const normal = std::string("CHANNELD_NORMAL");
	auto const splice = std::string("CHANNELD_AWAITING_SPLICE");
	auto const unilateral = std::string("AWAITING_UNILATERAL");

	auto code = Ev::lift().then([&]() {
		return listing(bus, peer, normal, true);
	}).then([&]() {
		/* The splice transaction is signed.  */
		return state_changed(bus, normal, splice);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 0);
		/* A listing during the splice.  */
		return listing(bus, peer, splice, false);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 0);
		/* A listing requested before the splice arrives
		 * late, the channel still CHANNELD_NORMAL.  */
		return listing(bus, peer, normal, false);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 0);
		/* The new funding locks in.  */
		return state_changed(bus, splice, normal);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 0);
		/* A listing requested before the lock-in arrives
		 * late, the channel still in the splice state.  */
		return listing(bus, peer, splice, false);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 0);
		return listing(bus, peer, normal, false);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 0);
		/* A second splice, and the channel is closed
		 * during it.  */
		return state_changed(bus, normal, splice);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 0);
		return state_changed(bus, splice, unilateral);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 1);
		/* The listing requested before the close arrives,
		 * the channel still in the splice state.  */
		return listing(bus, peer, splice, false);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 1);
		/* A listing from after the close agrees.  */
		return listing(bus, peer, unilateral, false);
	}).then([&]() {
		assert(*created == 0);
		assert(*destroyed == 1);
		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);
	return 0;
}
