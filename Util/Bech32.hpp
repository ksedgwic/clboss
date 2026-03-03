#ifndef UTIL_BECH32_HPP
#define UTIL_BECH32_HPP

#include "Compiler.hpp"
#include<cstdint>
#include<stdexcept>
#include<string>
#include<vector>

namespace Util { namespace Bech32 {

/** Util::Bech32::decode
 *
 * @brief decode a bech32 string.
 *
 * @return true if decoding succeeded.
 *
 * @desc This is a ***very simple*** decoder and
 * does ***not*** check the checksum.
 * The assumption is that this software talks to
 * other software and not the user, so the
 * checksum would not validate anything that a
 * piece of software would be incorrect with.
 */
bool decode( std::string& hrp
	   , std::vector<bool>& data
	   , std::string const& bech32
	   );

/** Util::Bech32::bitstream_to_bytes
 *
 * @brief write an `std::vector<bool>` to bytes
 * pointed to by the given iterator.
 */
template<typename OIt>
OIt bitstream_to_bytes( std::vector<bool>::const_iterator b
		      , std::vector<bool>::const_iterator e
		      , OIt oit
		      ) {
	auto bitctr = 0;
	auto byte = std::uint8_t(0);
	while (b != e) {
		byte = byte << 1;
		byte |= *b ? std::uint8_t(1) : std::uint8_t(0);
		++b;
		++bitctr;
		if (bitctr == 8) {
			bitctr = 0;
			*oit = byte;
			++oit;
			byte = 0;
		}
	}

	/* Pad with 0s if there are extra.  */
	if (bitctr != 0) {
		for (;;) {
			byte = byte << 1;
			++bitctr;
			if (bitctr == 8) {
				*oit = byte;
				++oit;
				break;
			}
		}
	}

	return oit;
}

}}

#endif /* !defined(UTIL_BECH32_HPP) */
