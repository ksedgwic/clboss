#ifndef STATS_RESERVOIRSAMPLER_HPP
#define STATS_RESERVOIRSAMPLER_HPP

#include"Util/vector_emplace_back.hpp"
#include<cmath>
#include<cstdint>
#include<random>
#include<vector>

namespace Stats {

/** class ReservoirSampler<Sample, Weight>
 *
 * @brief selects a number of samples, biased by
 * weight (higher weight items are more likely to
 * be selected).
 *
 * @desc Implements A-ES (Efraimidis and Spirakis,
 * "Weighted random sampling with a reservoir"):
 * every item gets the key u^(1/w), u uniform in
 * (0, 1), and the reservoir keeps the m largest
 * keys (held in log space, ln(u) / w).  Exact
 * weighted sampling without replacement: no
 * overweight special cases, and the sample does
 * not depend on arrival order.
 */
template< typename Sample
	, typename Weight = double
	>
class ReservoirSampler {
private:
	std::size_t max_selected;
	std::vector<Sample> selected;
	/* A-ES key of each selected item, in log space;
	 * `keys[i]` is the key that admitted
	 * `selected[i]`.  The log key ln(u) / w is a
	 * monotonic transform of u^(1/w), so comparing log
	 * keys ranks exactly like comparing keys, and it
	 * cannot underflow: with u^(1/w) the distance
	 * finder's BTC-scale weights (e.g. w = 1e-5)
	 * would flush most keys to exactly 0 and
	 * degenerate sampling into arrival order.  */
	std::vector<Weight> keys;

	template<typename Rand>
	Weight sample_key(Weight w, Rand& r) {
		auto distw = std::uniform_real_distribution<Weight>(
			0, 1
		);
		auto u = distw(r);
		/* u == 0 has negligible probability and yields
		 * the losing key -infinity; no special case.  */
		return std::log(u) / w;
	}

public:
	ReservoirSampler() : max_selected(1) { }
	explicit
	ReservoirSampler(std::size_t max_selected_
			) : max_selected(max_selected_) { }
	ReservoirSampler(ReservoirSampler&&) =default;
	ReservoirSampler(ReservoirSampler const&) =default;

	void clear() {
		selected.clear();
		keys.clear();
	}
	void clear(std::size_t max_selected_) {
		selected.clear();
		keys.clear();
		max_selected = max_selected_;
	}

	template<typename Rand>
	void add(Sample s, Weight w, Rand& r) {
		/* Should not happen.  */
		if (max_selected == 0)
			return;

		auto key = sample_key(w, r);

		if (selected.size() < max_selected) {
			Util::vector_emplace_back( selected
						 , std::move(s)
						 );
			keys.emplace_back(key);
			return;
		}

		/* Find the smallest key; our streams are a few
		 * hundred entries so a linear scan is fine.  */
		auto min_i = std::size_t(0);
		for (auto i = std::size_t(1); i < keys.size(); ++i) {
			if (keys[i] < keys[min_i])
				min_i = i;
		}

		if (keys[min_i] < key) {
			keys[min_i] = key;
			selected[min_i] = std::move(s);
		}
	}

	std::vector<Sample> const& get() const { return selected; }
	std::vector<Sample> finalize()&& {
		return std::move(selected);
	}
};

}

#endif /* STATS_RESERVOIRSAMPLER_HPP */
