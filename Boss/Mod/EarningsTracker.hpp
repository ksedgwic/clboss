#ifndef BOSS_MOD_EARNINGSTRACKER_HPP
#define BOSS_MOD_EARNINGSTRACKER_HPP

#include"Ev/now.hpp"
#include<functional>
#include<memory>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::EarningsTracker
 *
 * @brief Keeps track of earnings of channels with each node, as well as
 * how much we expended in rebalancing.
 *
 * @desc In addition to keeping track of earnings and rebalancings, this
 * responds to `Boss::Msg::RequestEarningsInfo` messages with its own
 * `Boss::Msg::ResponseEarningsInfo`.
 */
class EarningsTracker {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	EarningsTracker() =delete;

	EarningsTracker(EarningsTracker&&);
	~EarningsTracker();

	explicit
	EarningsTracker(S::Bus& bus, std::function<double()> get_now_ = &Ev::now);
};

}}

#endif /* !defined(BOSS_MOD_EARNINGSTRACKER_HPP) */
