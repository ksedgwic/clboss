#include"Ln/FeatureBit.hpp"
#include"Util/Str.hpp"
#include<cstdint>
#include<vector>

namespace Ln {

bool feature_bit(std::string const& features_hex, unsigned int bit) {
	auto bytes = std::vector<std::uint8_t>();
	try {
		bytes = Util::Str::hexread(features_hex);
	} catch (Util::Str::HexParseFailure const&) {
		return false;
	}
	auto byte_from_end = std::size_t(bit / 8);
	if (byte_from_end >= bytes.size())
		return false;
	auto b = bytes[bytes.size() - 1 - byte_from_end];
	return ((b >> (bit % 8)) & 1) != 0;
}

}
