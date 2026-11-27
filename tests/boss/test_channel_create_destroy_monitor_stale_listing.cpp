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
#include<vector>

/* A listing requested just before a channel starts closing can
 * arrive after the `channel_state_changed` notification, and still
 * shows the channel open.  It must not bring the channel back: no
 * ChannelCreation for a peer whose last channel just left, and no
 * missed ChannelDestruction when a peer's other channel goes next.
 */

namespace {

auto const single_str = std::string(
	"020000000000000000000000000000000000000000000000000000000000000000"
);
auto const multi_str = std::string(
	"030000000000000000000000000000000000000000000000000000000000000000"
);
/* `single` has one channel, `multi` two; `chan_new` is a later
 * channel to `single`.  */
auto const chan_single = std::string(
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
);
auto const chan_multi_1 = std::string(
	"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
);
auto const chan_multi_2 = std::string(
	"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
);
auto const chan_new = std::string(
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
);

Jsmn::Object channel(std::string const& id, std::string const& state) {
	auto json = std::string("{\"channel_id\":\"") + id + "\","
		    "\"state\":\"" + state + "\"}";
	return Jsmn::Object::parse_json(json.c_str());
}

/* A listing with each channel in the state given; an empty state
 * leaves the channel out.  */
Ev::Io<void> listing( S::Bus& bus, bool initial
		    , std::string const& single_state
		    , std::string const& multi_1_state
		    , std::string const& multi_2_state
		    , std::string const& new_state
		    ) {
	auto cpeers = Boss::Mod::ConstructedListpeers();
	auto add = [&cpeers]( std::string const& peer
			    , std::string const& chan
			    , std::string const& state
			    ) {
		if (state.empty())
			return;
		auto& p = cpeers[Ln::NodeId(peer)];
		p.connected = true;
		p.channels.push_back(channel(chan, state));
	};
	add(single_str, chan_single, single_state);
	add(multi_str, chan_multi_1, multi_1_state);
	add(multi_str, chan_multi_2, multi_2_state);
	add(single_str, chan_new, new_state);
	return bus.raise(Boss::Msg::ListpeersResult{
		std::move(cpeers), initial
	}) + Ev::yield() + Ev::yield() + Ev::yield() + Ev::yield();
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

	auto created = std::make_shared<std::vector<Ln::NodeId>>();
	bus.subscribe<Boss::Msg::ChannelCreation
		     >([created](Boss::Msg::ChannelCreation const& c) {
		created->push_back(c.peer);
		return Ev::lift();
	});
	auto destroyed = std::make_shared<std::vector<Ln::NodeId>>();
	bus.subscribe<Boss::Msg::ChannelDestruction
		     >([destroyed](Boss::Msg::ChannelDestruction const& d) {
		destroyed->push_back(d.peer);
		return Ev::lift();
	});

	auto single = Ln::NodeId(single_str);
	auto multi = Ln::NodeId(multi_str);

	auto const normal = std::string("CHANNELD_NORMAL");
	auto const shutting = std::string("CHANNELD_SHUTTING_DOWN");
	auto const unilateral = std::string("AWAITING_UNILATERAL");

	auto code = Ev::lift().then([&]() {
		return listing(bus, true, normal, normal, normal, "");
	}).then([&]() {
		/* The periodic check closes `single`'s only channel
		 * and one of `multi`'s two.  */
		return state_changed( bus, single_str, chan_single
				    , normal, shutting
				    );
	}).then([&]() {
		return state_changed( bus, multi_str, chan_multi_1
				    , normal, shutting
				    );
	}).then([&]() {
		assert(destroyed->size() == 1);
		assert((*destroyed)[0] == single);
		/* The listing requested before the closes arrives,
		 * every channel still CHANNELD_NORMAL.  */
		return listing(bus, false, normal, normal, normal, "");
	}).then([&]() {
		/* `single` is not created again.  */
		assert(created->empty());
		assert(destroyed->size() == 1);
		/* `multi`'s last open channel goes: the stale listing
		 * must not have put the closing one back.  */
		return state_changed( bus, multi_str, chan_multi_2
				    , normal, unilateral
				    );
	}).then([&]() {
		assert(destroyed->size() == 2);
		assert((*destroyed)[1] == multi);
		/* A listing from after the closes agrees.  */
		return listing(bus, false, shutting, shutting, unilateral, "");
	}).then([&]() {
		assert(created->empty());
		assert(destroyed->size() == 2);
		/* A new channel to `single` is still a creation.  */
		return listing(bus, false, shutting, shutting, unilateral
			      , normal
			      );
	}).then([&]() {
		assert(created->size() == 1);
		assert((*created)[0] == single);
		assert(destroyed->size() == 2);
		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);
	return 0;
}
