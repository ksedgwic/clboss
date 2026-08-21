#ifndef BOSS_MOD_SWAPMANAGER_HPP
#define BOSS_MOD_SWAPMANAGER_HPP

#include<memory>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::SwapManager
 *
 * @brief Handles offchain-to-onchain swaps,
 * keeping information needed to track them
 * in persistent storage.
 */
class SwapManager {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	SwapManager() =delete;

	SwapManager(SwapManager&&);
	SwapManager& operator=(SwapManager&&);
	~SwapManager();

	explicit
	SwapManager(S::Bus& bus);

	/* Test seam: shrink the stalled-loop watchdog window.  */
	void set_stall_timeout_secs(double secs);
};

}}

#endif /* !defined(BOSS_MOD_SWAPMANAGER_HPP) */
