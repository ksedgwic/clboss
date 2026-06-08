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
 *   - `off`        : no autonomous rebalancing at all; also the supported
 *                    way to disable the rebalancer entirely.
 */
enum class RebalanceMode {
	classic,
	off
};

/* Default mode at startup if the operator does not configure one.  */
constexpr RebalanceMode default_rebalance_mode = RebalanceMode::classic;

inline
char const* rebalance_mode_to_string(RebalanceMode m) {
	switch (m) {
	case RebalanceMode::classic:    return "classic";
	case RebalanceMode::off:        return "off";
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
	if (s == "off") {
		out = RebalanceMode::off;
		return true;
	}
	return false;
}

}

#endif /* !defined(BOSS_REBALANCEMODE_HPP) */
