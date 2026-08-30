#undef NDEBUG
#include"Boss/Mod/Dowser.hpp"
#include"Ln/Amount.hpp"
#include<assert.h>

/* floor_probe_amount: the lower bracket of the binary search.
 * Legacy behavior (floor_target == 0): probe/32 + 1 msat.  With a
 * caller relevance threshold: min(probe/32, target/0.985 + 1 sat)
 * + 1 msat -- so flows the caller would accept are never zeroed by
 * the floor gate (the E15-d residual band). */
int main() {
	using La = Ln::Amount;

	/* Legacy: pure integer path, exact.  */
	{
		auto f = Boss::Mod::Dowser::floor_probe_amount(
			La::msat(1543147208), La::sat(0)
		);
		assert(f == La::msat(48223351));
	}

	/* Legacy with a floor_target ABOVE resolution: unchanged.  */
	{
		auto f = Boss::Mod::Dowser::floor_probe_amount(
			La::msat(1543147208), La::msat(600000000)
		);
		assert(f == La::msat(48223351));
	}

	/* E15-d vector: probe 36,548,224,350 (max_channel 36M sat),
	 * floor_target = min_channel 1,044,100,000 -> scaled
	 * 1,060,001,000 < resolution 1,142,132,010 -> floor brackets at
	 * the min-relevant capacity (double math: allow +-1 msat).  */
	{
		auto f = Boss::Mod::Dowser::floor_probe_amount(
			La::msat(36548224350), La::msat(1044100000)
		);
		assert(f.to_msat() > 1060001000 - 2);
		assert(f.to_msat() <= 1060001001 + 1);
	}

	/* Mainnet defaults: probe 17,032,705,568 (max 16,777,215 sat),
	 * floor_target 500,000,000 -> floor ~= 507,615,214 == the
	 * janitor threshold: the [507,615, 532,272) band closes.  */
	{
		auto f = Boss::Mod::Dowser::floor_probe_amount(
			La::msat(17032705568), La::msat(500000000)
		);
		/* resolution = 532,272,049; scaled = 507,615,213; floor
		 * = scaled + 1 (double math: allow +-1 msat).  */
		assert(f.to_msat() > 507615213 - 2);
		assert(f.to_msat() <= 507615214 + 1);
		assert(f.to_msat() < 532272049);
	}

	return 0;
}
