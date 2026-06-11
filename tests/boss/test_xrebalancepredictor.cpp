#undef NDEBUG

#include"Boss/Mod/XRebalancePredictor.hpp"

#include<cassert>

using Boss::Mod::XRebalancePredictor;
using Row = Boss::Mod::XRebalancePredictor::Row;

namespace {

/* Walls-only live defaults (the planned first enablement), with the
 * master switch open.  */
auto const walls_only = Boss::Mod::XRebalancePredict::Params{
	2.0, 86400, 2, 1.0, 0.0};

}

int main() {
	auto rows = std::vector<Row>{
		/* Stale wall regime: 2 failures spanning 1800s.  */
		{"100x1x0", 0, 1000, "liquidity_fail", 7000},
		{"100x1x0", 0, 2800, "liquidity_fail", 6000},
		/* Fresh data: not a candidate regardless of regime.  */
		{"200x2x0", 1, 4500, "liquidity_fail", 9000},
		{"200x2x0", 1, 3000, "liquidity_fail", 9500},
		/* Stale floor regime: 2 successes spanning 1800s.  */
		{"300x3x0", 0, 1000, "success", 10000},
		{"300x3x0", 0, 2800, "success", 12000},
		/* node_fail only: a candidate, but not a liquidity
		 * bound -- nothing to assert.  */
		{"400x4x0", 0, 500, "node_fail", 1},
		/* Mixed lower bounds: a transit (hop forwarded a part
		 * that failed downstream) and a settled success
		 * combine on the floor side exactly alike; the floor
		 * is the larger of the two.  */
		{"500x5x0", 1, 1000, "transit", 9000},
		{"500x5x0", 1, 2800, "success", 8000},
	};
	auto const cutoff = std::uint64_t(4000);
	/* data age for the stale regimes: 3000s <= horizon 3600s.  */
	auto const now = std::uint64_t(2800 + 3000);

	/* Walls-only: exactly the 100x1x0 wall at the tightest
	 * failure bound.  */
	{
		auto plan = XRebalancePredictor::plan(
			rows, cutoff, now, walls_only);
		assert(plan.directions == 5);
		assert(plan.candidates == 4);
		assert(plan.assertions.size() == 1);
		auto const& a = plan.assertions[0];
		assert(a.scid == "100x1x0");
		assert(a.dir == 0);
		assert(a.is_wall);
		assert(a.amount_msat == 6000);
	}

	/* Enabling floors adds the 300x3x0 floor (12000 * 0.9) and
	 * the 500x5x0 floor, whose bound comes from the TRANSIT
	 * record (9000 > the settled success's 8000): transit
	 * evidence feeds the floor side exactly like success.  */
	{
		auto p = walls_only;
		p.floor_factor = 0.9;
		auto plan = XRebalancePredictor::plan(
			rows, cutoff, now, p);
		assert(plan.assertions.size() == 3);
		assert(!plan.assertions[1].is_wall);
		assert(plan.assertions[1].scid == "300x3x0");
		assert(plan.assertions[1].amount_msat == 10800);
		assert(!plan.assertions[2].is_wall);
		assert(plan.assertions[2].scid == "500x5x0");
		assert(plan.assertions[2].amount_msat == 8100);
	}

	/* Wall margin scales the asserted amount.  */
	{
		auto p = walls_only;
		p.wall_margin = 1.5;
		auto plan = XRebalancePredictor::plan(
			rows, cutoff, now, p);
		assert(plan.assertions.size() == 1);
		assert(plan.assertions[0].amount_msat == 9000);
	}

	/* Master switch: horizon cap 0 means every regime is past
	 * its horizon; nothing asserts (the live module also skips
	 * the cycle entirely).  */
	{
		auto p = walls_only;
		p.horizon_max_secs = 0;
		auto plan = XRebalancePredictor::plan(
			rows, cutoff, now, p);
		assert(plan.assertions.size() == 0);
		assert(plan.candidates == 4);
	}

	/* Later `now`: the same regimes go stale past their horizon
	 * (3600s) and assert nothing.  */
	{
		auto plan = XRebalancePredictor::plan(
			rows, cutoff, std::uint64_t(2800 + 3601),
			walls_only);
		assert(plan.assertions.size() == 0);
	}

	return 0;
}
