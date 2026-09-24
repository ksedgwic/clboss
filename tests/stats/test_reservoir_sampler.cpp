#undef NDEBUG
#include"Boss/random_engine.hpp"
#include"Stats/ReservoirSampler.hpp"
#include<assert.h>
#include<string>

int main() {
	{
		auto smp = Stats::ReservoirSampler<std::string>();
		smp.add("a", 1, Boss::random_engine);
		smp.add("b", 1, Boss::random_engine);
		smp.add("c", 1, Boss::random_engine);
		smp.add("d", 1, Boss::random_engine);

		assert(smp.get().size() == 1);
		auto sels = std::move(smp).finalize();
		assert(sels.size() == 1);
		assert( sels[0] == "a"
		     || sels[0] == "b"
		     || sels[0] == "c"
		     || sels[0] == "d"
		      );
	}

	{
		auto smp = Stats::ReservoirSampler<std::string>(3);
		smp.add("a", 1, Boss::random_engine);
		assert(smp.get().size() == 1);
		assert(smp.get()[0] == "a");
		smp.add("b", 1, Boss::random_engine);
		smp.add("c", 1, Boss::random_engine);
		smp.add("d", 1, Boss::random_engine);
		smp.add("e", 1, Boss::random_engine);
		smp.add("f", 1, Boss::random_engine);

		assert(smp.get().size() == 3);
	}

	/* Weight proportionality and arrival-order independence:
	 * 30 light (w=1) and 6 heavy (w=10) items, m = 3.
	 * Weight-proportional expectation is E[heavy slots] =
	 * 3*60/90 = 2 of 3 per trial.  The previous A-Chao
	 * sampler (with the reservoir-size factor m missing from
	 * the replacement probability) made inclusion
	 * arrival-order-dominated instead.  Deterministic via
	 * per-trial seeded engines.  */
	{
		auto heavy_lights_first = 0;
		auto heavy_heavies_first = 0;
		for (auto trial = 0; trial < 3000; ++trial) {
			auto eng = std::default_random_engine(trial);
			auto smp = Stats::ReservoirSampler<std::string>(3);
			for (auto i = 0; i < 30; ++i)
				smp.add("light", 1.0, eng);
			for (auto i = 0; i < 6; ++i)
				smp.add("heavy", 10.0, eng);
			for (auto const& sel : std::move(smp).finalize()) {
				if (sel == "heavy")
					++heavy_lights_first;
			}
		}
		for (auto trial = 0; trial < 3000; ++trial) {
			auto eng = std::default_random_engine(trial);
			auto smp = Stats::ReservoirSampler<std::string>(3);
			for (auto i = 0; i < 6; ++i)
				smp.add("heavy", 10.0, eng);
			for (auto i = 0; i < 30; ++i)
				smp.add("light", 1.0, eng);
			for (auto const& sel : std::move(smp).finalize()) {
				if (sel == "heavy")
					++heavy_heavies_first;
			}
		}
		/* Heavies win the majority of the ~9000 slots in
		 * both orders (expectation 6000 of 9000).  */
		assert(heavy_lights_first > 4500);
		assert(heavy_heavies_first > 4500);
		/* A-ES is arrival-order independent, so the two
		 * orders agree on the heavy count; an
		 * arrival-biased sampler over-selects heavies
		 * when they arrive first.  */
		auto diff = heavy_lights_first > heavy_heavies_first
			  ? heavy_lights_first - heavy_heavies_first
			  : heavy_heavies_first - heavy_lights_first
			  ;
		assert(diff < 400);
	}

	/* Tiny-weight regime (the distance finder passes costs in
	 * BTC): with u^(1/w) as the key, weights around 1e-5 flush
	 * most keys to exactly 0 and sampling degenerates into
	 * arrival order; log-space keys keep exact proportions at
	 * any weight scale.  Same 1:10 ratio, same expectation
	 * (heavy 6000 of 9000).  */
	{
		auto heavy = 0;
		for (auto trial = 0; trial < 3000; ++trial) {
			auto eng = std::default_random_engine(trial);
			auto smp = Stats::ReservoirSampler<std::string>(3);
			for (auto i = 0; i < 30; ++i)
				smp.add("light", 0.00001, eng);
			for (auto i = 0; i < 6; ++i)
				smp.add("heavy", 0.0001, eng);
			for (auto const& sel : std::move(smp).finalize()) {
				if (sel == "heavy")
					++heavy;
			}
		}
		assert(heavy > 4500);
	}

	return 0;
}
