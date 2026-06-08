#ifndef BOSS_MOD_REBALANCEMODEMANAGER_HPP
#define BOSS_MOD_REBALANCEMODEMANAGER_HPP

#include<memory>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::RebalanceModeManager
 *
 * @brief Owns the currently-active rebalancing mode (the single
 * source of truth) via the dynamic `clboss-rebalance-mode` option:
 * the config file sets the startup default and
 * `setconfig clboss-rebalance-mode <mode>` switches it at runtime.
 * Answers `Boss::Msg::RequestRebalanceMode` queries from rebalancing
 * modules that self-gate on the mode.
 *
 * Mode is in-memory only: a restart reverts to the configured
 * startup default, giving a known-good baseline on every boot.
 */
class RebalanceModeManager {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	RebalanceModeManager() =delete;
	RebalanceModeManager(RebalanceModeManager const&) =delete;

	RebalanceModeManager(RebalanceModeManager&&);
	~RebalanceModeManager();

	explicit
	RebalanceModeManager(S::Bus& bus);
};

}}

#endif /* !defined(BOSS_MOD_REBALANCEMODEMANAGER_HPP) */
