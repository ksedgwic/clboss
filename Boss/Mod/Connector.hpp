#ifndef BOSS_MOD_CONNECTOR_HPP
#define BOSS_MOD_CONNECTOR_HPP

#include<string>

namespace Boss { namespace Mod { class Rpc; }}
namespace Ev { template<typename a> class Io; }
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::Connector
 *
 * @brief module that does `connect` commands.
 */
class Connector {
private:
	S::Bus& bus;
	Boss::Mod::Rpc *rpc;
	/* lightningd runs with --offline: refuse every request.  */
	bool offline;

	Ev::Io<void> connect(std::string const& node);
	void start();

public:
	Connector(S::Bus& bus_
		 ) : bus(bus_), rpc(nullptr), offline(false) { start(); }
	Connector(Connector&&) =delete;
};

}}

#endif /* !defined(BOSS_MOD_CONNECTOR_HPP) */
