#include"Boss/Mod/ChannelCreateDestroyMonitor.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/Msg/ChannelCreateResult.hpp"
#include"Boss/Msg/ChannelCreation.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"S/Bus.hpp"
#include"Util/stringify.hpp"
#include<algorithm>
#include<iterator>
#include<vector>

namespace {

Ev::Io<void> creation(S::Bus& bus, Ln::NodeId const& n) {
	auto act = Ev::lift();
	act += Boss::log( bus, Boss::Info
			, "ChannelCreation: %s"
			, std::string(n).c_str()
			);
	act += bus.raise(Boss::Msg::ChannelCreation{n});
	return Boss::concurrent(act);
}
Ev::Io<void> destruction(S::Bus& bus, Ln::NodeId const& n) {
	auto act = Ev::lift();
	act += Boss::log( bus, Boss::Info
			, "ChannelDestruction: %s"
			, std::string(n).c_str()
			);
	act += bus.raise(Boss::Msg::ChannelDestruction{n});
	return Boss::concurrent(act);
}

Ev::Io<void> wait_for_true(bool& flag) {
	return Ev::yield().then([&flag]() {
		if (flag)
			return Ev::lift();
		return wait_for_true(flag);
	});
}

}

namespace Boss { namespace Mod {

void ChannelCreateDestroyMonitor::start() {
	bus.subscribe<Msg::ListpeersAnalyzedResult
		     >([this](Msg::ListpeersAnalyzedResult const& r) {
		/* First gather the current channeled peers.  */
		auto curr_channeled = std::set<Ln::NodeId>();
		std::set_union( r.connected_channeled.begin()
			      , r.connected_channeled.end()
			      , r.disconnected_channeled.begin()
			      , r.disconnected_channeled.end()
			      , std::inserter( curr_channeled
					     , curr_channeled.begin()
					     )
			      );

		/* If initial, we have to initialize our channeled.  */
		if (r.initial) {
			channeled = std::move(curr_channeled);
			initted = true;
			return Ev::lift();
		}

		/* This is true most of the time, which avoid doing
		 * the 2x O(n) traversals to build creations and
		 * destructions, at the cost of a O(n) traversal.
		 */
		if (channeled == curr_channeled)
			return Ev::lift();

		/* Get the created and destructed channels.  */
		/* creations = curr_channeled - channeled.  */
		auto creations = std::vector<Ln::NodeId>();
		std::set_difference( curr_channeled.begin()
				   , curr_channeled.end()
				   , channeled.begin()
				   , channeled.end()
				   , std::back_inserter(creations)
				   );
		/* destructions = channeled - curr_channeled.  */
		auto destructions = std::vector<Ln::NodeId>();
		std::set_difference( channeled.begin()
				   , channeled.end()
				   , curr_channeled.begin()
				   , curr_channeled.end()
				   , std::back_inserter(creations)
				   );

		/* Combine.  */
		auto cf = [this](Ln::NodeId n) {
			return creation(bus, n);
		};
		auto df = [this](Ln::NodeId n) {
			return destruction(bus, n);
		};
		auto act = Ev::foreach(std::move(cf), std::move(creations))
			 + Ev::foreach(std::move(df), std::move(destructions))
			 ;

		channeled = std::move(curr_channeled);
		return act;
	});

	/* The Boss::Mod::ChannelCreator emits this message when it
	 * creates a new channel, so speed up our reaction to known
	 * new channels by this mechanism.
	 */
	bus.subscribe<Msg::ChannelCreateResult
		     >([this](Msg::ChannelCreateResult const& r) {
		if (!r.success)
			return Ev::lift();
		auto it = channeled.find(r.node);
		if (it != channeled.end())
			/* Already known.  */
			return Ev::lift();
		/* Else have to update our channeled set and emit creation
		 * event.  */
		channeled.insert(r.node);
		return creation(bus, r.node);
	});

	/* Also monitor by notifications `channel_opened` (for channels
	 * initiated by our peers) and `channel_state_changed` (for
	 * channel closures).
	 */
	bus.subscribe<Msg::Manifestation
		     >([this](Msg::Manifestation const& _) {
		return bus.raise(Msg::ManifestNotification{
			"channel_opened"
		       })
		     + bus.raise(Msg::ManifestNotification{
			"channel_state_changed"
		       })
		     ;
	});
	auto on_channel_opened = [this](Jsmn::Object const& params) {
		auto n = Ln::NodeId();
		try {
			auto payload = params["channel_opened"];
			n = Ln::NodeId(std::string(payload["id"]));
		} catch (std::runtime_error const& err) {
			return Boss::log( bus, Error
					, "ChannelCreateDestroyMonitor: "
					  "Unexpected channel_opened "
					  "payload: %s: %s"
					, Util::stringify(params).c_str()
					, err.what()
					);
		}
		/* Is it already in channeled?  */
		auto it = channeled.find(n);
		if (it != channeled.end())
			/* Do nothing.  */
			return Ev::lift();
		channeled.insert(n);
		return creation(bus, n);
	};
	auto on_channel_state_changed = [this](Jsmn::Object const& params) {
		auto n = Ln::NodeId();
		auto old_state = std::string();
		auto new_state = std::string();
		try {
			auto payload = params["channel_state_changed"];
			n = Ln::NodeId(std::string(payload["peer_id"]));
			old_state = std::string(payload["old_state"]);
			new_state = std::string(payload["new_state"]);
		} catch (std::runtime_error const& err) {
			return Boss::log( bus, Error
					, "ChannelCreateDestroyMonitor: "
					  "Unexpected channel_state_changed "
					  "payload: %s: %s"
					, Util::stringify(params).c_str()
					, err.what()
					);
		}
		/* Only continue if we are leaving the CHANNELD_NORMAL
		 * state, or leaving from CHANNELD_AWAITING_LOCKIN to
		 * any state that is not CHANNELD_NORMAL.
		 */
		if ( !( old_state == "CHANNELD_NORMAL"
		     || ( old_state == "CHANNELD_AWAITING_LOCKIN"
		       && new_state != "CHANNELD_NORMAL"
			)
		      ))
			return Ev::lift();

		/* Is it already gone from the channeled set?  */
		auto it = channeled.find(n);
		if (it == channeled.end())
			return Ev::lift();

		channeled.erase(it);
		return destruction(bus, n);
	};
	bus.subscribe<Msg::Notification
		     >([=, this](Msg::Notification const& n) {
		if (n.notification == "channel_opened")
			return on_channel_opened(n.params);
		else if (n.notification == "channel_state_changed") {
			auto params = n.params;
			return wait_for_true(this->initted).then([=]() {
				return on_channel_state_changed(params);
			});
		}
		return Ev::lift();
	});
}

}}
