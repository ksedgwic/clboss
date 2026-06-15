#ifndef BOSS_MOD_XREBALANCEPREDICT_HPP
#define BOSS_MOD_XREBALANCEPREDICT_HPP

#include<cstddef>
#include<cstdint>
#include<string>
#include<vector>

namespace Boss { namespace Mod { namespace XRebalancePredict {

/** Boss::Mod::XRebalancePredict
 *
 * @brief The pure persistence-forecasting algorithm for the
 * xrebalance observation history.
 *
 * @desc No I/O, no bus, no clock -- everything, including `now`,
 * is a parameter, so unit tests and spot-checks are exact and the
 * same code can back both the read-only `clboss-xrebalance-history`
 * predictions block and the live phase-2 predictor.
 *
 * Model: every observation is an interval BOUND on a directed
 * channel's liquidity L, not a measurement -- a success at amount m
 * means L >= m, a failure at attempted amount A means L < A.  A
 * static L is consistent with a record set iff max(successes) <
 * min(failures).  Walking records newest -> oldest and intersecting
 * until contradiction yields the CURRENT REGIME: the longest recent
 * run consistent with one static liquidity in [B_lo, B_hi).
 *
 * The persistence forecast re-asserts those bounds for as long as
 * the demonstrated stability justifies:
 *
 *   horizon = min(horizon_max_secs, horizon_frac * regime_span)
 *
 * where regime_span is the EVIDENCE span (newest - oldest record in
 * the regime) -- it grows only with actual observations, never with
 * mere wall-clock passage.  A side (wall = failure bound, floor =
 * success bound) is asserted only while data_age <= horizon and the
 * regime holds at least min_samples records of that side's kind.
 */

struct Params {
	/* horizon = min(horizon_max_secs, horizon_frac * span).  */
	double horizon_frac;
	std::uint64_t horizon_max_secs;
	/* Minimum records of a side's kind in the regime before that
	 * side is asserted.  */
	std::size_t min_samples;
	/* Multiplier on the wall bound (>= 1.0 biases errors high,
	 * which self-corrects; too-low walls are sticky).  */
	double wall_margin;
	/* Multiplier on the floor bound (<= 1.0 conservative; floors
	 * that are too high cost a failed part to self-correct).
	 * <= 0 disables floor forecasts.  */
	double floor_factor;
};

inline constexpr Params default_params{ 2.0, 86400, 2, 1.0, 0.9 };

/* One bounds-relevant observation.  Callers map observation kinds:
 * success -> is_fail=false; liquidity_fail / policy_fail ->
 * is_fail=true; node_fail is not a per-channel liquidity bound and
 * must be skipped by the caller.  */
struct Bound {
	std::uint64_t time;
	/* true: L < amount_msat at `time`; false: L >= amount_msat.  */
	bool is_fail;
	std::uint64_t amount_msat;
};

/* Verdict for one side (wall or floor) of one directed channel.  */
struct Side {
	/* Would the live predictor assert this side right now?  */
	bool would_assert;
	/* The amount to assert (margin/factor applied); valid only
	 * when has_amount.  */
	bool has_amount;
	std::uint64_t amount_msat;
	/* Records of this side's kind inside the regime.  */
	std::size_t samples;
	/* Empty when would_assert; otherwise why not.  */
	std::string decline_reason;
};

struct Result {
	/* Records inside the current regime (node_fail excluded by
	 * the caller; contradicting older records excluded here).  */
	std::size_t regime_records;
	/* Oldest / span / freshness of the regime evidence.  */
	std::uint64_t regime_start_time;
	std::uint64_t regime_span_secs;
	std::uint64_t data_age_secs;
	std::uint64_t horizon_secs;
	/* True when an older record contradicts the regime (the walk
	 * stopped early): the channel CHANGED at that point.  */
	bool truncated;
	/* Raw regime bounds before margin/factor: L in [floor, wall).
	 * has_amount=false on a side with no record of its kind.  */
	Side wall;
	Side floor;
};

/* `bounds` in any order; sorted internally.  */
Result predict( std::vector<Bound> bounds
	      , std::uint64_t now
	      , Params const& params
	      );

}}}

#endif /* !defined(BOSS_MOD_XREBALANCEPREDICT_HPP) */
