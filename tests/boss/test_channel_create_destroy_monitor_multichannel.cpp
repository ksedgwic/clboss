#undef NDEBUG
#include"Boss/Mod/ChannelCreateDestroyMonitor.hpp"
#include"Boss/Mod/ConstructedListpeers.hpp"
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
#include<vector>

/* Regression test for #354: with two channels to one peer, closing
 * one of them must not announce a ChannelDestruction for the peer.
 * Only the closure of the peer's last open channel does.
 */

namespace {

auto const peer_str = std::string(
	"020000000000000000000000000000000000000000000000000000000000000000"
);
auto const other_str = std::string(
	"030000000000000000000000000000000000000000000000000000000000000000"
);
auto const chan_a = std::string(
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
);
auto const chan_b = std::string(
	"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
);
auto const chan_c = std::string(
	"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
);
auto const opening_str = std::string(
	"020000000000000000000000000000000000000000000000000000000000000001"
);
auto const chan_d = std::string(
	"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
);

Jsmn::Object unnamed_channel(std::string const& state) {
	auto json = std::string("{\"state\":\"") + state + "\"}";
	return Jsmn::Object::parse_json(json.c_str());
}

Jsmn::Object channel(std::string const& id, std::string const& state) {
	auto json = std::string("{\"channel_id\":\"") + id + "\","
		    "\"state\":\"" + state + "\"}";
	return Jsmn::Object::parse_json(json.c_str());
}

Ev::Io<void> state_changed( S::Bus& bus
			  , std::string const& peer
			  , std::string const& chan
			  , std::string const& old_state
			  , std::string const& new_state
			  ) {
	auto json = std::string("{\"channel_state_changed\":{"
				"\"peer_id\":\"") + peer + "\","
		    "\"channel_id\":\"" + chan + "\","
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

	auto destroyed = std::make_shared<std::vector<Ln::NodeId>>();
	bus.subscribe<Boss::Msg::ChannelDestruction
		     >([destroyed](Boss::Msg::ChannelDestruction const& d) {
		destroyed->push_back(d.peer);
		return Ev::lift();
	});

	auto peer = Ln::NodeId(peer_str);
	auto other = Ln::NodeId(other_str);
	auto opening = Ln::NodeId(opening_str);

	auto code = Ev::lift().then([&]() {
		/* `peer` has two open channels, `other` has one.  */
		auto cpeers = Boss::Mod::ConstructedListpeers();
		cpeers[peer].connected = true;
		cpeers[peer].channels.push_back(
			channel(chan_a, "CHANNELD_NORMAL"));
		cpeers[peer].channels.push_back(
			channel(chan_b, "CHANNELD_NORMAL"));
		cpeers[other].connected = true;
		cpeers[other].channels.push_back(
			channel(chan_c, "CHANNELD_NORMAL"));
		/* `opening` has one open channel and one still
		 * opening, listed without a channel_id.  */
		cpeers[opening].connected = true;
		cpeers[opening].channels.push_back(
			channel(chan_d, "CHANNELD_NORMAL"));
		cpeers[opening].channels.push_back(
			unnamed_channel("DUALOPEND_AWAITING_LOCKIN"));
		return bus.raise(Boss::Msg::ListpeersResult{
			std::move(cpeers), true
		});
	}).then([&]() {
		/* First channel to `peer` starts closing.  */
		return state_changed( bus, peer_str, chan_a
				    , "CHANNELD_NORMAL"
				    , "CHANNELD_SHUTTING_DOWN"
				    );
	}).then([&]() {
		/* Channel b is still open: no destruction.  */
		assert(destroyed->empty());
		/* Now the peer's last channel goes.  */
		return state_changed( bus, peer_str, chan_b
				    , "CHANNELD_NORMAL"
				    , "AWAITING_UNILATERAL"
				    );
	}).then([&]() {
		assert(destroyed->size() == 1);
		assert((*destroyed)[0] == peer);
		/* A single-channel peer is destroyed on its first
		 * closure, as before.  */
		return state_changed( bus, other_str, chan_c
				    , "CHANNELD_NORMAL"
				    , "CHANNELD_SHUTTING_DOWN"
				    );
	}).then([&]() {
		assert(destroyed->size() == 2);
		assert((*destroyed)[1] == other);
		/* The opening channel without an id still holds
		 * `opening` open when its other channel closes.  */
		return state_changed( bus, opening_str, chan_d
				    , "CHANNELD_NORMAL"
				    , "AWAITING_UNILATERAL"
				    );
	}).then([&]() {
		assert(destroyed->size() == 2);
		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);
	return 0;
}
