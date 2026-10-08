#undef NDEBUG
#include"Boss/Mod/ChannelCreateDestroyMonitor.hpp"
#include"Boss/Mod/ConstructedListpeers.hpp"
#include"Boss/Mod/ListpeersAnalyzer.hpp"
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

/* Regression test for #366: a peer whose only channel is shutting
 * down was destroyed by the notification, re-created by the next
 * listing (ListpeersAnalyzer counts CHANNELD_SHUTTING_DOWN as
 * channeled) and destroyed again by the listing after the close.
 * The monitor must report one destruction and no creation.
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
	if (!state.empty()) {
		auto json = std::string("{\"channel_id\":\"") + chan_a + "\","
			    "\"state\":\"" + state + "\"}";
		cpeers[peer].channels.push_back(
			Jsmn::Object::parse_json(json.c_str()));
	}
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
	/* The analyzer runs alongside, as in clboss itself.  */
	auto analyzer = Boss::Mod::ListpeersAnalyzer(bus);
	(void) analyzer;
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

	auto code = Ev::lift().then([&]() {
		return listing(bus, peer, "CHANNELD_NORMAL", true);
	}).then([&]() {
		/* Cooperative close starts.  */
		return state_changed( bus
				    , "CHANNELD_NORMAL"
				    , "CHANNELD_SHUTTING_DOWN"
				    );
	}).then([&]() {
		assert(*destroyed == 1);
		/* Next ten-minute listing: still shutting down.  */
		return listing(bus, peer, "CHANNELD_SHUTTING_DOWN", false);
	}).then([&]() {
		assert(*created == 0);
		/* Shutdown completes.  */
		return state_changed( bus
				    , "CHANNELD_SHUTTING_DOWN"
				    , "CLOSINGD_SIGEXCHANGE"
				    );
	}).then([&]() {
		/* The listing after the close no longer has it.  */
		return listing(bus, peer, "CLOSINGD_COMPLETE", false);
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
