#include"Boss/Mod/NewaddrHandler.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/RequestNewaddr.hpp"
#include"Boss/Msg/ResponseNewaddr.hpp"
#include"Boss/concurrent.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"

namespace Boss { namespace Mod {

void NewaddrHandler::start() {
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		auto f = [this](void* r) { return newaddr(r); };
		return Boss::concurrent(
			Ev::foreach(f, std::move(pending))
		);
	});
	bus.subscribe<Msg::RequestNewaddr
		     >([this](Msg::RequestNewaddr const& r) {
		auto requester = r.requester;
		if (!rpc) {
			pending.push_back(requester);
			return Ev::lift();
		}
		return Boss::concurrent(newaddr(requester));
	});
}
Ev::Io<void> NewaddrHandler::newaddr(void* requester) {
	/** BREAKING CHANGE:
	 * "newaddr p2tr" NOT compatible with v23.05 and older */
	return rpc->command( "newaddr p2tr"
			   , Json::Out::empty_object()
			   ).then([this, requester](Jsmn::Object res) {
		auto addr = std::string(res["p2tr"]);
		return bus.raise(Msg::ResponseNewaddr{
			std::move(addr), requester
		});
	});
}

}}
