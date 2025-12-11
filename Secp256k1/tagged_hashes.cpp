#include"Secp256k1/tagged_hashes.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/HasherStream.hpp"

namespace Tag {

namespace {

const char *taptagstrings[4] = {
	"TapLeaf",
	"TapBranch",
	"TapTweak",
	"TapSighash"
};

}

const char* str(Tag::tap tag) {
	return taptagstrings[static_cast<uint8_t>(tag)];
}

}

namespace Secp256k1 {

void tagged_hash( std::uint8_t output[32]
				, std::uint8_t const input[]
				, size_t inputsize
				, Tag::tap tag ) {
	if (static_cast<uint8_t>(tag) > 3)
		return; // TODO throw exception

	auto hasher = Sha256::HasherStream(tag);
	for (size_t i{0}; i < inputsize; i++)
		hasher.put(input[i]);

	auto taggedhash = std::move(hasher).finalize();
	taggedhash.to_buffer(&output[0]);
}

}
