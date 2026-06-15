#undef NDEBUG

#include"Boss/Mod/XRebalancePredict.hpp"

#include<cassert>

using Boss::Mod::XRebalancePredict::Bound;
using Boss::Mod::XRebalancePredict::default_params;
using Boss::Mod::XRebalancePredict::predict;

namespace {

/* success: liquidity >= amount.  */
Bound ok(std::uint64_t time, std::uint64_t amount) {
	return Bound{time, false, amount};
}
/* failure: liquidity < amount.  */
Bound fail(std::uint64_t time, std::uint64_t amount) {
	return Bound{time, true, amount};
}

}

int main() {
	/* Empty history: nothing to say.  */
	{
		auto r = predict({}, 1000, default_params);
		assert(r.regime_records == 0);
		assert(!r.wall.would_assert);
		assert(!r.floor.would_assert);
		assert(!r.wall.has_amount);
		assert(!r.floor.has_amount);
	}

	/* One point: no prediction (min_samples).  */
	{
		auto r = predict({fail(1000, 5000)}, 1000, default_params);
		assert(r.regime_records == 1);
		assert(r.regime_span_secs == 0);
		assert(r.horizon_secs == 0);
		assert(r.wall.has_amount);
		assert(r.wall.amount_msat == 5000);
		assert(!r.wall.would_assert);
		assert(r.wall.decline_reason
		    == "insufficient samples (1 < 2)");
		assert(!r.floor.would_assert);
		assert(r.floor.decline_reason
		    == "no success observations in regime");

		/* With min_samples overridden to 1 the same single
		 * point hits the span gate instead, with the singular
		 * wording.  */
		auto p = default_params;
		p.min_samples = 1;
		r = predict({fail(1000, 5000)}, 2000, p);
		assert(!r.wall.would_assert);
		assert(r.wall.decline_reason
		    == "a single observation has no time span; "
		       "nothing to extrapolate until the channel is "
		       "observed again later");
	}

	/* Ken's calibration: 2 consistent points 1h apart -> predict
	 * up to 2h past the newest; one second later it is stale.  */
	{
		auto bounds = std::vector<Bound>{
			fail(1000, 5000), fail(4600, 6000)};
		auto r = predict(bounds, 4600 + 7200, default_params);
		assert(r.regime_records == 2);
		assert(r.regime_span_secs == 3600);
		assert(r.horizon_secs == 7200);
		assert(r.data_age_secs == 7200);
		assert(r.wall.would_assert);
		/* The wall is the TIGHTEST failure bound.  */
		assert(r.wall.amount_msat == 5000);

		r = predict(bounds, 4600 + 7201, default_params);
		assert(!r.wall.would_assert);
		assert(r.wall.decline_reason
		    == "stale: data age 7201s exceeds horizon 7200s");
	}

	/* 3 points spanning 2h -> horizon 4h.  */
	{
		auto r = predict(
		    {fail(1000, 5000), fail(4600, 5500), fail(8200, 5200)},
		    8200, default_params);
		assert(r.regime_span_secs == 7200);
		assert(r.horizon_secs == 14400);
		assert(r.wall.would_assert);
		assert(r.wall.amount_msat == 5000);
	}

	/* Cap: a 13h span would give a 26h horizon; capped at 24h.  */
	{
		auto r = predict(
		    {fail(0, 5000), fail(46800, 5000)},
		    46800, default_params);
		assert(r.regime_span_secs == 46800);
		assert(r.horizon_secs == 86400);
	}

	/* Contradiction: an older failure BELOW a newer success means
	 * the channel changed; the regime is only the newer record,
	 * and the walk reports the truncation.  */
	{
		auto r = predict(
		    {fail(1000, 50000), ok(2000, 60000)},
		    2000, default_params);
		assert(r.truncated);
		assert(r.regime_records == 1);
		assert(r.regime_start_time == 2000);
		assert(r.wall.samples == 0);
		assert(r.floor.samples == 1);
		assert(!r.wall.would_assert);
		assert(r.wall.decline_reason
		    == "no failure observations in regime");
		assert(!r.floor.would_assert);
	}

	/* Mixed consistent regime: both sides assert; floor takes the
	 * factor, wall the margin.  L in [12000, 70000).  */
	{
		auto bounds = std::vector<Bound>{
			ok(1000, 10000), fail(2000, 80000),
			ok(3000, 12000), fail(4000, 70000)};
		auto r = predict(bounds, 4000, default_params);
		assert(!r.truncated);
		assert(r.regime_records == 4);
		assert(r.wall.samples == 2);
		assert(r.floor.samples == 2);
		assert(r.wall.would_assert);
		assert(r.wall.amount_msat == 70000);
		assert(r.floor.would_assert);
		/* 12000 * 0.9 */
		assert(r.floor.amount_msat == 10800);

		/* Margin biases the wall HIGH (self-correcting).  */
		auto p = default_params;
		p.wall_margin = 1.5;
		r = predict(bounds, 4000, p);
		assert(r.wall.amount_msat == 105000);

		/* floor_factor <= 0 disables floors.  */
		p = default_params;
		p.floor_factor = 0.0;
		r = predict(bounds, 4000, p);
		assert(!r.floor.would_assert);
		assert(r.floor.decline_reason
		    == "floor forecasts disabled (factor <= 0)");

		/* min_samples is per side.  */
		p = default_params;
		p.min_samples = 3;
		r = predict(bounds, 4000, p);
		assert(!r.wall.would_assert);
		assert(r.wall.decline_reason
		    == "insufficient samples (2 < 3)");
	}

	/* Several MPP parts of ONE flow failing on the same hop in
	 * the same second: many bounds, one temporal sample.  The
	 * span is 0, so nothing extrapolates -- and the message says
	 * so rather than reading as a confusing "stale ... 0s".  */
	{
		auto r = predict(
		    { fail(5000, 155654219)
		    , fail(5000, 344655846)
		    , fail(5000, 355773647)},
		    5000 + 2675, default_params);
		assert(r.regime_records == 3);
		assert(r.regime_span_secs == 0);
		assert(r.horizon_secs == 0);
		assert(r.wall.samples == 3);
		assert(r.wall.amount_msat == 155654219);
		assert(!r.wall.would_assert);
		assert(r.wall.decline_reason
		    == "zero evidence span (all 3 records simultaneous); "
		       "nothing to extrapolate until the channel is "
		       "observed again later");

		/* A later re-observation unlocks it: span becomes
		 * real and the horizon follows.  */
		auto r2 = predict(
		    { fail(5000, 155654219)
		    , fail(5000, 344655846)
		    , fail(5000, 355773647)
		    , fail(6800, 160000000)},
		    6800 + 2675, default_params);
		assert(r2.regime_span_secs == 1800);
		assert(r2.horizon_secs == 3600);
		assert(r2.wall.would_assert);
		assert(r2.wall.amount_msat == 155654219);
	}

	/* Policy exclusions look like an ultra-stable zero-width
	 * wall: failures at 1 msat re-assert at 1 msat.  */
	{
		auto r = predict(
		    {fail(1000, 1), fail(4600, 1)},
		    4600, default_params);
		assert(r.wall.would_assert);
		assert(r.wall.amount_msat == 1);
		assert(!r.floor.has_amount);
	}

	/* A success above an older success only raises the floor;
	 * no contradiction.  */
	{
		auto r = predict(
		    {ok(1000, 5000), ok(2000, 9000)},
		    2000, default_params);
		assert(!r.truncated);
		assert(r.floor.would_assert);
		/* 9000 * 0.9 */
		assert(r.floor.amount_msat == 8100);
	}

	return 0;
}
