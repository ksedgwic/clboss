#ifndef SECP256K1_TAGGED_HASHES_HPP
#define SECP256K1_TAGGED_HASHES_HPP

#include<cstdint>
#include<string>

namespace Tag {

enum tap : uint8_t { LEAF, BRANCH, TWEAK, SIGHASH };
const char* str(tap);

}

namespace Secp256k1 {

void tagged_hash( std::uint8_t output[32]
				, std::uint8_t const input[]
				, size_t size
				, Tag::tap );

}

#endif /* SECP256K1_TAGGED_HASHES_HPP */
