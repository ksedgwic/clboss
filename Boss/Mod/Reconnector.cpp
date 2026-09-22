#include"Boss/Mod/Reconnector.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/NeedsConnect.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Boss/log.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"

namespace Boss { namespace Mod {

class Reconnector::Impl {
private:
	S::Bus& bus;
	Boss::Mod::Rpc *rpc;
	/* lightningd runs with --offline: peers stay disconnected.  */
	bool offline = false;

	void start() {
		bus.subscribe<Msg::Manifestation>([this](Msg::Manifestation const& _) {
			return bus.raise(Msg::ManifestNotification{
				"disconnect"
			});
		});
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			rpc = &init.rpc;
			offline = init.offline;
			return Ev::lift();
		});
		bus.subscribe<Msg::Notification>([this](Msg::Notification const& n) {
			if (n.notification != "disconnect")
				return Ev::lift();
			if (offline)
				return Ev::lift();
			return on_disconnect();
		});
	}

	Ev::Io<void> on_disconnect() {
		if (!rpc)
			return Ev::lift();
		return rpc->command("listpeers", Json::Out::empty_object()
				   ).then([this](Jsmn::Object result) {
			if (!result.is_object() || !result.has("peers"))
				return reconnect();
			auto peers = result["peers"];
			if (!peers.is_array())
				return reconnect();
			for (auto peer : peers) {
				if (!peer.is_object())
					continue;
				if (!peer.has("connected"))
					continue;
				auto connected = peer["connected"];
				if (!connected.is_boolean()
				  || !bool(connected))
					continue;
				/* Oh look, we have some connected peer.
				 * Do nothing.  */
				return Ev::lift();
			}
			return reconnect();
		});
	}

	Ev::Io<void> reconnect() {
		return Boss::log( bus, Info
				, "Reconnector: No connected peers, "
				  "re-triggering connection."
				).then([this]() {
			return bus.raise(Msg::NeedsConnect());
		});
	}

public:
	Impl(S::Bus& bus_) : bus(bus_) { start(); }
};

Reconnector::Reconnector(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus))
	{ }
Reconnector::Reconnector(Reconnector&& o)
	: pimpl(std::move(o.pimpl))
	{ }
Reconnector::~Reconnector() { }

}}
