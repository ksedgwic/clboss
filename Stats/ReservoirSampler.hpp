#ifndef STATS_RESERVOIRSAMPLER_HPP
#define STATS_RESERVOIRSAMPLER_HPP

#include"Util/vector_emplace_back.hpp"
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
 * @desc Implements A-Chao.
 */
template< typename Sample
	, typename Weight = double
	>
class ReservoirSampler {
private:
	std::size_t max_selected;
	std::vector<Sample> selected;
	Weight wsum;

public:
	ReservoirSampler() : max_selected(1), wsum(0) { }
	explicit
	ReservoirSampler(std::size_t max_selected_
			) : max_selected(max_selected_), wsum(0) { }
	ReservoirSampler(ReservoirSampler&&) =default;
	ReservoirSampler(ReservoirSampler const&) =default;

	void clear() { selected.clear(); }
	void clear(std::size_t max_selected_) {
		selected.clear();
		max_selected = max_selected_;
	}

	template<typename Rand>
	void add(Sample s, Weight w, Rand& r) {
		/* Should not happen.  */
		if (max_selected == 0)
			return;

		if (selected.size() == 0)
			wsum = w;
		else
			wsum += w;

		if (selected.size() < max_selected) {
			Util::vector_emplace_back( selected
						 , std::move(s)
						 );
			return;
		}

		auto distw = std::uniform_real_distribution<Weight>(
			0, 1
		);
		auto p = w / wsum;
		auto j = distw(r);
		if (j <= p) {
			/* Randomly replace an existing selection.  */
			auto disti
				= std::uniform_int_distribution<std::size_t>(
				0, selected.size() - 1
			);
			auto i = disti(r);
			selected[i] = std::move(s);
		}
	}

	std::vector<Sample> const& get() const { return selected; }
	std::vector<Sample> finalize()&& {
		return std::move(selected);
	}
};

}

#endif /* STATS_RESERVOIRSAMPLER_HPP */
