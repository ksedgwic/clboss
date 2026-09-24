#include"Boss/Mod/Connector.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/RequestConnect.hpp"
#include"Boss/Msg/ResponseConnect.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"

namespace Boss { namespace Mod {

void Connector::start() {
	bus.subscribe<Boss::Msg::Init>([this](Boss::Msg::Init const& init) {
		rpc = &init.rpc;
		offline = init.offline;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::RequestConnect>([this](Boss::Msg::RequestConnect const& c) {
		/* Should not happen.  */
		if (!rpc)
			return Boss::log( bus, Warn
					, "Attempt to connect to %s before init?"
					, c.node.c_str()
					);
		/* An explicit `connect` would go through even with
		 * --offline; answer as a failure so that every requester
		 * moves on.  */
		if (offline)
			return Boss::log( bus, Debug
					, "Connector: offline mode, not "
					  "connecting to %s"
					, c.node.c_str()
					).then([this, node = c.node]() {
				return bus.raise(Boss::Msg::ResponseConnect{
					node, false
				});
			});

		return connect(c.node);
	});
}

Ev::Io<void> Connector::connect(std::string const& node) {
	return Ev::lift().then([this, node]() {
		return Boss::log( bus, Debug
				, "Connector: try %s"
				, node.c_str()
				);
	}).then([this, node]() {
		auto params = Json::Out()
			.start_object()
				.field("id", node)
			.end_object()
			;
		return rpc->command("connect", params);
	}).then([this, node](Jsmn::Object result) {
		/* If it returned normally, then everything went fine.  */
		return Boss::log( bus, Info
				, "Connector: connected to %s"
				, node.c_str()
				).then([]() {
			return Ev::lift(true);
		});
	}).catching<Boss::Mod::RpcError>([this, node
					 ](Boss::Mod::RpcError const& _) {
		/* RPC errors are just failures to connect. */
		return Boss::log( bus, Info
				, "Connector: failed to connect to %s"
				, node.c_str()
				).then([]() {
			return Ev::lift(false);
		});
	}).then([this, node](bool success) {
		return bus.raise(Boss::Msg::ResponseConnect{node, success});
	});
}

}}
