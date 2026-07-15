#ifndef BOSS_MSG_TRACKRECORD_HPP
#define BOSS_MSG_TRACKRECORD_HPP

#include<cstdint>

namespace Boss { namespace Msg {

/** enum Boss::Msg::TrackRecordVerdict
 *
 * @brief Judgment of a node's earnings track record from
 * past channels with us, used to rank channel-open
 * candidates.
 */
enum class TrackRecordVerdict {
	/* Earned well for us before; we definitely want it back.  */
	Keeper,
	/* Not enough history to judge.  */
	NoRecord,
	/* We had history with it and it earned poorly; open only
	 * if no better candidate can absorb the funds.  */
	Underperformer
};

inline
char const* track_record_verdict_name(TrackRecordVerdict v) {
	switch (v) {
	case TrackRecordVerdict::Keeper: return "keeper";
	case TrackRecordVerdict::NoRecord: return "no-record";
	case TrackRecordVerdict::Underperformer: return "underperformer";
	}
	return "unknown";
}

/** struct Boss::Msg::TrackRecord
 *
 * @brief A node's earnings track record over the configured
 * window, as judged by `Boss::Mod::PeerTrackRecord`.
 */
struct TrackRecord {
	TrackRecordVerdict verdict;
	/* Annualized net return on liquidity, in basis points:
	 * net earnings relative to the average balance held in
	 * the channel, scaled to a year.  Only meaningful when
	 * `verdict` is not `NoRecord`.  */
	double tral_bps;
	/* Days the channel was observed operating within the
	 * window.  */
	double op_days;
	/* Net earnings (earnings minus rebalance expenditures,
	 * both directions) within the window, in millisatoshi.  */
	std::int64_t net_msat;
};

}}

#endif /* !defined(BOSS_MSG_TRACKRECORD_HPP) */
