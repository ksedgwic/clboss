#ifndef BOSS_MOD_BOLTZSWAPPER_ENV_HPP
#define BOSS_MOD_BOLTZSWAPPER_ENV_HPP

#include<cstdint>
#include"Boltz/EnvIF.hpp"

namespace Boss { namespace Mod { class Rpc; }}
namespace S { class Bus; }

namespace Boss { namespace Mod { namespace BoltzSwapper {

/** class Boss::Mod::BoltzSwapper::Env
 *
 * @brief fulfills the `Boltz::EnvIF` interface,
 * using the RPC and log of CLBOSS.
 */
class Env : public Boltz::EnvIF {
private:
	S::Bus& bus;
	Boss::Mod::Rpc* rpc;

	void start();

public:
	Env() =delete;
	Env(Env&&) =delete;
	Env(Env const&) =delete;

	explicit
	Env(S::Bus& bus_) : bus(bus_), rpc(nullptr) {
		start();
	}

	/* Boltz::EnvIF.  */
	Ev::Io<std::uint32_t> get_feerate() override;
	Ev::Io<bool> broadcast_tx(Bitcoin::Tx) override;
	Ev::Io<void> logd(std::string) override;
	Ev::Io<void> loge(std::string) override;
};

}}}

#endif /* !defined(BOSS_MOD_BOLTZSWAPPER_ENV_HPP) */
