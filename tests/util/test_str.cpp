#undef NDEBUG
#include"Util/Str.hpp"
#include<assert.h>
#include<cstdint>

int main() {
	using Util::Str::group_digits;

	/* Unsigned.  */
	assert(group_digits(std::uint64_t(0)) == "0");
	assert(group_digits(std::uint64_t(999)) == "999");
	assert(group_digits(std::uint64_t(1000)) == "1_000");
	assert(group_digits(std::uint64_t(18851040)) == "18_851_040");
	assert(group_digits(std::uint64_t(220708066)) == "220_708_066");
	assert( group_digits(std::uint64_t(18446744073709551615ull))
	     == "18_446_744_073_709_551_615");

	/* Signed.  */
	assert(group_digits(std::int64_t(-1)) == "-1");
	assert(group_digits(std::int64_t(-18851040)) == "-18_851_040");
	assert( group_digits(std::int64_t(INT64_MIN))
	     == "-9_223_372_036_854_775_808");

	return 0;
}
