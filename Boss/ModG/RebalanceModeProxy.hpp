#ifndef BOSS_MODG_REBALANCEMODEPROXY_HPP
#define BOSS_MODG_REBALANCEMODEPROXY_HPP

#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/RequestRebalanceMode.hpp"
#include"Boss/Msg/ResponseRebalanceMode.hpp"
#include"Boss/RebalanceMode.hpp"

namespace Boss { namespace ModG {

/** class Boss::ModG::RebalanceModeProxy
 *
 * @brief a proxy for Boss::Mod::RebalanceModeManager,
 * allowing a rebalancing module to query the currently-active
 * mode and self-gate on it.
 */
class RebalanceModeProxy {
private:
	ReqResp< Msg::RequestRebalanceMode
	       , Msg::ResponseRebalanceMode
	       > core;

public:
	RebalanceModeProxy() =delete;
	RebalanceModeProxy(RebalanceModeProxy const&) =delete;
	RebalanceModeProxy(RebalanceModeProxy&&) =delete;

	explicit
	RebalanceModeProxy(S::Bus& bus)
		: core(bus)
		{ }

	Ev::Io<RebalanceMode>
	get_mode() {
		return core.execute(Msg::RequestRebalanceMode{
			nullptr
		}).then([](Msg::ResponseRebalanceMode m) {
			return Ev::lift(m.mode);
		});
	}

	/* Convenience: is Track A (classic) the active mode?  */
	Ev::Io<bool>
	is_classic() {
		return get_mode().then([](RebalanceMode m) {
			return Ev::lift(m == RebalanceMode::classic);
		});
	}
};

}}

#endif /* !defined(BOSS_MODG_REBALANCEMODEPROXY_HPP) */
