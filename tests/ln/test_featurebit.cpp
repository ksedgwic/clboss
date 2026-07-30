#undef NDEBUG
#include"Ln/FeatureBit.hpp"
#include<assert.h>

int main() {
	using Ln::feature_bit;

	/* Bit 0 is the least-significant bit of the last byte.  */
	assert( feature_bit("01", 0));
	assert(!feature_bit("01", 1));
	assert( feature_bit("02", 1));
	assert(!feature_bit("02", 0));

	/* Bits beyond the field are unset.  */
	assert(!feature_bit("01", 8));
	assert(!feature_bit("", 0));

	/* Multi-byte: bit 8 is the least-significant bit of the
	 * second-to-last byte.  */
	assert( feature_bit("0100", 8));
	assert(!feature_bit("0100", 0));

	/* option_splice bits 62/63 need an 8-byte field; bit 62 is
	 * 0x40 of the leading byte, bit 63 is 0x80.  */
	assert( feature_bit("4000000000000000", 62));
	assert(!feature_bit("4000000000000000", 63));
	assert( feature_bit("8000000000000000", 63));
	assert(!feature_bit("8000000000000000", 62));

	/* Longer fields keep bit positions anchored at the tail.  */
	assert( feature_bit("00004000000000000000", 62));
	assert( feature_bit("888a4000000000000000", 62));

	/* Malformed hex is just "no features".  */
	assert(!feature_bit("xyz", 0));
	assert(!feature_bit("0", 0));

	return 0;
}
