#ifndef BOSS_MOD_XREBALANCEHISTORY_HPP
#define BOSS_MOD_XREBALANCEHISTORY_HPP

#include"Ev/now.hpp"
#include<functional>
#include<memory>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::XRebalanceHistory
 *
 * @brief Long-lived store of xrebalance liquidity observations.
 *
 * @desc Records every Boss::Msg::XRebalanceObservation raised by
 * Boss::Mod::XMoveFunds into the `XRebalanceHistory` sqlite table,
 * losslessly (kind, failcode, erring node -- richer than the
 * min/max projection the askrene clboss-xrebalance layer keeps).
 * Rows older than `clboss-xrebalance-history-age-secs` (default one
 * week) are trimmed once per Boss::Msg::TimerRandomHourly tick.
 *
 * The stored series is the evidence base for the persistence
 * forecaster (phase 2) and future per-direction reliability
 * statistics; this module itself only stores, trims, and reports
 * (`clboss-xrebalance-history` command).
 */
class XRebalanceHistory {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	XRebalanceHistory() =delete;

	XRebalanceHistory(XRebalanceHistory&&);
	~XRebalanceHistory();

	explicit
	XRebalanceHistory( S::Bus& bus
			 , std::function<double()> get_now_ = &Ev::now
			 );
};

}}

#endif /* !defined(BOSS_MOD_XREBALANCEHISTORY_HPP) */
