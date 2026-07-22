#ifndef BOSS_REBALANCEMODE_HPP
#define BOSS_REBALANCEMODE_HPP

#include<string>

namespace Boss {

/** enum class Boss::RebalanceMode
 *
 * @brief which rebalancing track, if any, is currently active.
 *
 * @desc
 *   - `classic`    : Track A, the original clboss rebalancer/funds-mover
 *                    ported to getroutes/askrene (heuristic, JIT-capable).
 *   - `xrebalance` : Track B, the circular askrene min-cost-flow
 *                    rebalancer (the XRebalancer driver on top of the
 *                    XMoveFunds primitive -- "xpay, for rebalancing");
 *                    deliberate, non-JIT.  Tuned by the clboss-xrebalance-*
 *                    options.
 *   - `xrebalance2`: the same XRebalancer driver, executing through the
 *                    external `xrebalance` plugin (layer-splitting on
 *                    stock askrene) instead of the in-clboss XMoveFunds
 *                    primitive.  Requires the xrebalance plugin to be
 *                    loaded into lightningd; constraint knowledge and
 *                    failure feedback live in the plugin, so the
 *                    in-clboss layer machinery (including the
 *                    predictor) stays idle.
 *   - `off`        : no autonomous rebalancing at all; also the supported
 *                    way to disable the rebalancer entirely.
 */
enum class RebalanceMode {
	classic,
	xrebalance,
	xrebalance2,
	off
};

/* Default mode at startup if the operator does not configure one.  */
constexpr RebalanceMode default_rebalance_mode = RebalanceMode::classic;

inline
char const* rebalance_mode_to_string(RebalanceMode m) {
	switch (m) {
	case RebalanceMode::classic:     return "classic";
	case RebalanceMode::xrebalance:  return "xrebalance";
	case RebalanceMode::xrebalance2: return "xrebalance2";
	case RebalanceMode::off:         return "off";
	}
	return "classic";
}

/* Parse a mode string; returns true and sets `out` on success,
 * false on an unrecognized string (leaving `out` untouched).  */
inline
bool rebalance_mode_from_string(std::string const& s, RebalanceMode& out) {
	if (s == "classic") {
		out = RebalanceMode::classic;
		return true;
	}
	if (s == "xrebalance") {
		out = RebalanceMode::xrebalance;
		return true;
	}
	if (s == "xrebalance2") {
		out = RebalanceMode::xrebalance2;
		return true;
	}
	if (s == "off") {
		out = RebalanceMode::off;
		return true;
	}
	return false;
}

}

#endif /* !defined(BOSS_REBALANCEMODE_HPP) */
