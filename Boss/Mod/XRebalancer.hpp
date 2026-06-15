#ifndef BOSS_MOD_XREBALANCER_HPP
#define BOSS_MOD_XREBALANCER_HPP

#include<memory>

namespace Boss { namespace Mod { class Waiter; }}
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::XRebalancer
 *
 * @brief The Track-B ("xrebalance") rebalancer driver: a periodic, Poisson-
 * paced loop that decides WHEN to run a circular-askrene rebalance
 * cycle on top of the XMoveFunds primitive.
 *
 * Cadence is a memoryless Poisson process whose average rate is the
 * `clboss-xrebalance-per-hour` option (0 = paused), tunable at runtime.
 * The loop self-gates on the rebalance mode and only runs a cycle when
 * the active mode is `flow`; in other modes it idles (skips the cycle
 * body) so flipping into flow at runtime starts firing without a
 * restart.
 *
 * For now the cycle body is a stub (it logs); the view -> plan ->
 * xmovefunds pipeline and its accounting hang off this spine.
 */
class XRebalancer {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	XRebalancer() =delete;
	XRebalancer(XRebalancer const&) =delete;

	XRebalancer(XRebalancer&&);
	~XRebalancer();

	explicit
	XRebalancer(S::Bus& bus, Boss::Mod::Waiter& waiter);
};

}}

#endif /* !defined(BOSS_MOD_XREBALANCER_HPP) */
