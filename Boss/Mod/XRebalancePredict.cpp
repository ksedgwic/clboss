#include"Boss/Mod/XRebalancePredict.hpp"
#include<algorithm>
#include<cmath>
#include<limits>
#include<sstream>

namespace {

auto constexpr no_wall = std::numeric_limits<std::uint64_t>::max();

std::string stale_reason( std::uint64_t data_age
			, std::uint64_t horizon
			, std::uint64_t span
			, std::size_t records
			) {
	auto os = std::ostringstream();
	/* A zero span means all the regime's records carry one
	 * timestamp (typically several MPP parts of a single flow,
	 * informed together) -- many bounds but ONE temporal sample,
	 * so there is no demonstrated persistence to extrapolate.  */
	if (span == 0) {
		if (records == 1)
			os << "a single observation has no time span";
		else
			os << "zero evidence span (all " << records
			   << " records simultaneous)";
		os << "; nothing to extrapolate until the channel is "
		      "observed again later";
		return os.str();
	}
	os << "stale: data age " << data_age
	   << "s exceeds horizon " << horizon << "s";
	return os.str();
}

std::string samples_reason(std::size_t have, std::size_t need) {
	auto os = std::ostringstream();
	os << "insufficient samples (" << have << " < " << need << ")";
	return os.str();
}

}

namespace Boss { namespace Mod { namespace XRebalancePredict {

bool kind_is_bound(std::string const& kind, bool& is_fail) {
	/* "transit" (hop forwarded a part that later failed
	 * downstream and unwound) proves the same lower bound as
	 * "success" (hop on a settled part); they are distinct
	 * kinds only so statistics can separate proven-and-restored
	 * liquidity from proven-and-consumed.  */
	if (kind == "success" || kind == "transit") {
		is_fail = false;
		return true;
	}
	if (kind == "liquidity_fail" || kind == "policy_fail") {
		is_fail = true;
		return true;
	}
	return false;
}

Result predict( std::vector<Bound> bounds
	      , std::uint64_t now
	      , Params const& params
	      ) {
	auto result = Result();
	result.regime_records = 0;
	result.regime_start_time = 0;
	result.regime_span_secs = 0;
	result.data_age_secs = 0;
	result.horizon_secs = 0;
	result.truncated = false;
	result.wall.would_assert = false;
	result.wall.has_amount = false;
	result.wall.amount_msat = 0;
	result.wall.samples = 0;
	result.floor = result.wall;

	/* Newest first; deterministic tiebreak so equal-timestamp
	 * records always walk in the same order.  */
	std::sort( bounds.begin(), bounds.end()
		 , [](Bound const& a, Bound const& b) {
		if (a.time != b.time)
			return a.time > b.time;
		if (a.is_fail != b.is_fail)
			return a.is_fail < b.is_fail;
		return a.amount_msat < b.amount_msat;
	});

	/* Regime walk: intersect bounds newest -> oldest until the
	 * implied interval [lo, hi) for a static liquidity becomes
	 * empty -- the contradiction marks where the channel CHANGED,
	 * and only the newer, mutually-consistent records form the
	 * current regime.  */
	auto lo = std::uint64_t(0);
	auto hi = no_wall;
	auto regime_newest = std::uint64_t(0);
	for (auto const& b : bounds) {
		auto new_lo = lo;
		auto new_hi = hi;
		if (b.is_fail)
			new_hi = std::min(new_hi, b.amount_msat);
		else
			new_lo = std::max(new_lo, b.amount_msat);
		if (new_lo >= new_hi) {
			result.truncated = true;
			break;
		}
		lo = new_lo;
		hi = new_hi;
		if (result.regime_records == 0)
			regime_newest = b.time;
		result.regime_start_time = b.time;
		++result.regime_records;
		if (b.is_fail)
			++result.wall.samples;
		else
			++result.floor.samples;
	}

	if (result.regime_records == 0) {
		result.wall.decline_reason =
			"no bounds-relevant observations";
		result.floor.decline_reason =
			"no bounds-relevant observations";
		return result;
	}

	result.regime_span_secs = regime_newest - result.regime_start_time;
	result.data_age_secs = (now > regime_newest) ? (now - regime_newest)
						     : std::uint64_t(0);
	auto horizon = double(params.horizon_frac)
		     * double(result.regime_span_secs);
	if (horizon > double(params.horizon_max_secs))
		horizon = double(params.horizon_max_secs);
	result.horizon_secs = std::uint64_t(horizon);

	auto fresh = result.data_age_secs <= result.horizon_secs;

	/* Wall: assert "liquidity < amount".  The raw bound is hi
	 * (the smallest attempted amount that failed); the margin
	 * biases errors high, which self-corrects via a fresh failed
	 * part, where a too-low wall is sticky until the horizon.  */
	if (hi != no_wall) {
		result.wall.has_amount = true;
		result.wall.amount_msat = std::uint64_t(
		    std::llround(double(hi) * params.wall_margin));
		if (result.wall.samples < params.min_samples)
			result.wall.decline_reason = samples_reason(
			    result.wall.samples, params.min_samples);
		else if (!fresh)
			result.wall.decline_reason = stale_reason(
			    result.data_age_secs, result.horizon_secs,
			    result.regime_span_secs,
			    result.regime_records);
		else
			result.wall.would_assert = true;
	} else {
		result.wall.decline_reason =
			"no failure observations in regime";
	}

	/* Floor: assert "liquidity >= amount".  */
	if (lo > 0) {
		result.floor.has_amount = true;
		result.floor.amount_msat = std::uint64_t(
		    std::llround(double(lo) * params.floor_factor));
		if (params.floor_factor <= 0)
			result.floor.decline_reason =
				"floor forecasts disabled (factor <= 0)";
		else if (result.floor.samples < params.min_samples)
			result.floor.decline_reason = samples_reason(
			    result.floor.samples, params.min_samples);
		else if (!fresh)
			result.floor.decline_reason = stale_reason(
			    result.data_age_secs, result.horizon_secs,
			    result.regime_span_secs,
			    result.regime_records);
		else
			result.floor.would_assert = true;
	} else {
		result.floor.decline_reason =
			"no success observations in regime";
	}

	return result;
}

}}}
