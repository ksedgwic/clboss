#include"Secp256k1/TapscriptTree.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/tagged_hashes.hpp"
#include"Bitcoin/varint.hpp"
#include<algorithm>
#include<sstream>
#include<string>
#include<utility>
#include<assert.h>
#include<string.h>

namespace Secp256k1 { namespace TapTree {

std::vector<std::uint8_t>
encoded_leaf(std::vector<std::uint8_t> const& script) {
	auto sz = script.size();
	// TODO minimum size 2 for a witness script?

	auto ss = std::ostringstream();
	ss << Bitcoin::varint(sz);
	auto varintstr = ss.str();
	size_t bytes = varintstr.size();

	if (bytes != 1 && bytes != 3 && bytes != 5 && bytes != 9)
		throw InvalidArg("bip341 script leaf incorrect CompactSize encoding");

	auto leafbuf = std::vector<std::uint8_t>();
	leafbuf.resize( 1 /* version */
				  + bytes /* varint byte(s) */
				  + sz );

	leafbuf[0] = 0xc0; /* script leaf version 0xc0 for bip341 script paths */
	memcpy(&leafbuf[1], varintstr.data(), bytes);
	memcpy(&leafbuf[bytes+1], script.data(), sz);

	return leafbuf;
}

LeafPair::LeafPair( std::vector<std::uint8_t> const& script0
				  , std::vector<std::uint8_t> const& script1 )
					: a(std::vector<std::uint8_t>(32))
					, b(std::vector<std::uint8_t>(32))
{
	/* tagged hash of each leaf */
	Secp256k1::tagged_hash( &a[0]
						  , reinterpret_cast<const uint8_t*>(script0.data())
						  , script0.size()
						  , Tag::LEAF );
	Secp256k1::tagged_hash( &b[0]
						  , reinterpret_cast<const uint8_t*>(script1.data())
						  , script1.size()
						  , Tag::LEAF );

	if (!std::lexicographical_compare( a.begin(), a.end(),
									   b.begin(), b.end()) )
		a.swap(b);
}

std::vector<std::uint8_t>
LeafPair::compute_branch_hash() {
	/* tagged hash of the (only) branch.
	 * it is the merkle root of this script tree. */
	unsigned char leaf_hashes[64];
	memcpy(&leaf_hashes[0], a.data(), a.size());
	memcpy(&leaf_hashes[32], b.data(), b.size());

	auto buffer = std::vector<std::uint8_t>(32);
	Secp256k1::tagged_hash( &buffer[0]
						  , leaf_hashes
						  , 64
						  , Tag::BRANCH );
	return buffer;
}

}}

