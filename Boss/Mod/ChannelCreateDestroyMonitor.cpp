#include"Boss/Mod/ChannelCreateDestroyMonitor.hpp"
#include"Boss/Mod/ChanneledPeers.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/Msg/ChannelCreateResult.hpp"
#include"Boss/Msg/ChannelCreation.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
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

/* Whether a channel in this state still counts as one we have
 * with the peer: opening or open, but not shutting down.  */
bool open_state(std::string const& state) {
	return Boss::Mod::channeled_state(state)
	    && state != "CHANNELD_SHUTTING_DOWN"
	     ;
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
	/* Rebuild the per-channel view from each listing, and
	 * reconcile the channeled peers against it.  The peer-level
	 * set uses the same rule as the notification path: a peer is
	 * channeled while it has an open channel, and a channel
	 * shutting down does not count.  ListpeersAnalyzer still
	 * counts it, which is right for its other consumers, so we
	 * do not use its bins here (#366).  */
	bus.subscribe<Msg::ListpeersResult
		     >([this](Msg::ListpeersResult const& r) {
		auto curr = std::map<Ln::NodeId, std::set<std::string>>();
		for (auto const& peer : r.cpeers) {
			auto unidentified = std::size_t(0);
			for (auto chan : peer.second.channels) {
				if (!chan.is_object() || !chan.has("state"))
					continue;
				auto state_j = chan["state"];
				if (!state_j.is_string())
					continue;
				if (!open_state(std::string(state_j)))
					continue;
				/* An opening channel can be listed before it
				 * has a channel_id.  Keep it under a
				 * placeholder until a later listing names it,
				 * so it still holds the peer open.  */
				if (!chan.has("channel_id")
				 || !chan["channel_id"].is_string()) {
					curr[peer.first].insert(
						"unidentified-"
						+ std::to_string(unidentified++)
					);
					continue;
				}
				curr[peer.first].insert(
					std::string(chan["channel_id"])
				);
			}
		}

		/* The current channeled peers.  */
		auto curr_channeled = std::set<Ln::NodeId>();
		for (auto const& e : curr)
			curr_channeled.insert(e.first);
		open_channels = std::move(curr);

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
				   , std::back_inserter(destructions)
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
		auto channel_id = std::string();
		auto old_state = std::string();
		auto new_state = std::string();
		try {
			auto payload = params["channel_state_changed"];
			n = Ln::NodeId(std::string(payload["peer_id"]));
			if (payload.has("channel_id"))
				channel_id = std::string(payload["channel_id"]);
			/* `old_state` may be omitted: as of CLN v26.06 the
			 * previously-deprecated sentinel value "unknown"
			 * is no longer emitted; the field is simply
			 * absent instead.  Leave old_state as the empty
			 * string in that case -- the CHANNELD_NORMAL /
			 * CHANNELD_AWAITING_LOCKIN check below will not
			 * match, so we skip silently, matching the
			 * original intent for the "unknown" value.
			 */
			if (payload.has("old_state"))
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
		/* A channel is live in CHANNELD_NORMAL and in
		 * CHANNELD_AWAITING_SPLICE: a splice keeps the channel
		 * open and forwarding until the new funding locks in,
		 * and it returns to CHANNELD_NORMAL with a new short
		 * channel id.  Only continue if we are leaving the live
		 * states for some other state, or leaving
		 * CHANNELD_AWAITING_LOCKIN for a state that is not live.
		 */
		auto live = [](std::string const& state) {
			return state == "CHANNELD_NORMAL"
			    || state == "CHANNELD_AWAITING_SPLICE"
			     ;
		};
		auto leaving = ( live(old_state) && !live(new_state) )
			    || ( old_state == "CHANNELD_AWAITING_LOCKIN"
			      && !live(new_state)
			       );

		/* Keep the per-channel view current, so a later closure
		 * of another channel to this peer sees this one.  */
		if (!channel_id.empty()) {
			if (!leaving && open_state(new_state))
				open_channels[n].insert(channel_id);
			else {
				auto oit = open_channels.find(n);
				if (oit != open_channels.end()) {
					oit->second.erase(channel_id);
					if (oit->second.empty())
						open_channels.erase(oit);
				}
			}
		}

		if (!leaving)
			return Ev::lift();

		/* Another channel to the same peer is still open, so the
		 * peer is still channeled: nothing was destroyed at the
		 * peer level.  */
		if (!channel_id.empty() && open_channels.count(n) != 0)
			return Boss::log( bus, Debug
					, "ChannelCreateDestroyMonitor: "
					  "channel %s to %s left %s, "
					  "but the peer still has an "
					  "open channel"
					, channel_id.c_str()
					, std::string(n).c_str()
					, old_state.c_str()
					);

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
