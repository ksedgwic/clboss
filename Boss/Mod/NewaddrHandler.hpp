#ifndef BOSS_MOD_NEWADDRHANDLER_HPP
#define BOSS_MOD_NEWADDRHANDLER_HPP

#include"Boss/Msg/RequestNewaddr.hpp"
#include<vector>

namespace Boss { namespace Mod { class Rpc; }}
namespace Ev { template<typename a> class Io; }
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::NewaddrHandler
 *
 * @brief handles `Boss::Msg::RequestNewaddr`
 * and responds with `Boss::Msg::ResponseNewaddr`.
 */
class NewaddrHandler {
private:
	S::Bus& bus;
	Boss::Mod::Rpc* rpc;
	std::vector<Msg::RequestNewaddr> pending;

	void start();
	Ev::Io<void> newaddr(Msg::RequestNewaddr);

public:
	NewaddrHandler() =delete;
	NewaddrHandler(NewaddrHandler&&) =delete;
	NewaddrHandler(NewaddrHandler const&) =delete;

	NewaddrHandler(S::Bus& bus_
		      ) : bus(bus_), rpc(nullptr) { start(); }
};

}}

#endif /* !defined(BOSS_MOD_NEWADDRHANDLER_HPP) */
