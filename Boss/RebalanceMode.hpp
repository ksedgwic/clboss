#ifndef BOSS_REBALANCEMODE_HPP
#define BOSS_REBALANCEMODE_HPP

#include<string>

namespace Boss {

/** enum class Boss::RebalanceMode
 *
 * @brief which rebalancing track, if any, is currently active.
 *
 * @desc
 *   - `xrebalance`: the circular askrene min-cost-flow rebalancer
 *                   (the XRebalancer driver; deliberate, non-JIT;
 *                   tuned by the clboss-xrebalance-* options),
 *                   executing through the external `xrebalance`
 *                   plugin (layer-splitting on stock askrene).
 *                   Requires the xrebalance plugin to be loaded
 *                   into lightningd; constraint knowledge and
 *                   failure feedback live in the plugin.
 *   - `off`       : no autonomous rebalancing at all; also the
 *                   supported way to disable the rebalancer entirely.
 */
enum class RebalanceMode {
	xrebalance,
	off
};

/* Default mode at startup if the operator does not configure one.
 * The external-plugin mode: without the xrebalance plugin loaded it
 * idles with a log hint, so upgraders get a clear install-the-plugin
 * message rather than silently no rebalancing.  */
constexpr RebalanceMode default_rebalance_mode = RebalanceMode::xrebalance;

inline
char const* rebalance_mode_to_string(RebalanceMode m) {
	switch (m) {
	case RebalanceMode::xrebalance: return "xrebalance";
	case RebalanceMode::off:         return "off";
	}
	return "off";
}

/* Parse a mode string; returns true and sets `out` on success,
 * false on an unrecognized string (leaving `out` untouched).  */
inline
bool rebalance_mode_from_string(std::string const& s, RebalanceMode& out) {
	if (s == "xrebalance") {
		out = RebalanceMode::xrebalance;
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
