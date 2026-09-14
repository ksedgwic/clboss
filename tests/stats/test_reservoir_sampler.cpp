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

	/* Chao weight-proportionality (regression for the missing
	 * reservoir-size factor m): the pre-fix acceptance p = w/wsum
	 * under-samples late arrivals by a factor of m, making inclusion
	 * arrival-order-dominated.  Scenario isolating exactly that:
	 * 30 early weight-1 items, then 6 late weight-10 items, m = 3.
	 * Correct Chao: E[heavy slots] = 3*60/90 = 2 of 3 per trial,
	 * lights ~1.  Pre-fix: late p = 10/wsum keeps lights on top.
	 * Deterministic via per-trial seeded engines.  */
	{
		auto heavy_trials = 0;
		auto light_trials = 0;
		for (auto trial = 0; trial < 3000; ++trial) {
			auto eng = std::default_random_engine(trial);
			auto smp = Stats::ReservoirSampler<std::string>(3);
			for (auto i = 0; i < 30; ++i)
				smp.add("light", 1.0, eng);
			for (auto i = 0; i < 6; ++i)
				smp.add("heavy", 10.0, eng);
			for (auto const& sel : std::move(smp).finalize()) {
				if (sel == "heavy")
					++heavy_trials;
				else
					++light_trials;
			}
		}
		/* ~6000 vs ~3000 expected; a simple majority margin is
		 * robust for any sane engine.  Pre-fix inverts this.  */
		assert(heavy_trials > light_trials);
	}

	return 0;
}
