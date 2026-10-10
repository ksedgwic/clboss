#undef NDEBUG
#include"Jsmn/Detail/Str.hpp"
#include<cassert>
#include<limits>
#include<string>

int main() {
	using Jsmn::Detail::Str::from_double;
	using Jsmn::Detail::Str::to_double;

	/* A timestamp with milliseconds keeps every digit.  The stream
	 * default of six significant digits printed it as 1.72164e+09,
	 * which read as 1721640000 (issue #224).  */
	assert(from_double(1721639381.632) == "1721639381.632");
	assert(from_double(1721639381) == "1721639381");

	/* Short values stay short.  */
	assert(from_double(0) == "0");
	assert(from_double(0.5) == "0.5");
	assert(from_double(0.1) == "0.1");
	assert(from_double(-2.25) == "-2.25");
	assert(from_double(1000000) == "1000000");

	/* Values that need every digit still read back exactly.  */
	auto x = 0.1 + 0.2;
	assert(to_double(from_double(x)) == x);
	auto third = 1.0 / 3.0;
	assert(to_double(from_double(third)) == third);

	/* The limits of the range: a fifteen-digit rounding of the
	 * largest double lies outside the range and must not be
	 * accepted; the seventeen-digit text reads back exactly.  */
	auto big = std::numeric_limits<double>::max();
	assert(from_double(big) == "1.7976931348623157e+308");
	assert(to_double(from_double(big)) == big);
	auto low = std::numeric_limits<double>::lowest();
	assert(to_double(from_double(low)) == low);
	auto tiny = std::numeric_limits<double>::min();
	assert(to_double(from_double(tiny)) == tiny);

	return 0;
}
