#undef NDEBUG
#include"Boltz/Detail/swaptree.hpp"
#include"Ripemd160/Hash.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Util/Str.hpp"
#include<assert.h>
#include<cstdint>
#include<string>
#include<vector>

namespace {

std::string hex(std::vector<std::uint8_t> const& v) {
	return Util::Str::hexdump(&v[0], v.size());
}

}

int main() {
	/* A claim and refund leaf pair as a Boltz backend wrote
	 * them, with the hash, keys and height they carry.  */
	auto hash160 = Ripemd160::Hash(
		"2c2d5441ef4a4469eae063941463e3c65ee926c0"
	);
	auto claim_key = Secp256k1::XonlyPubKey(
		"07262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779"
	);
	auto refund_key = Secp256k1::XonlyPubKey(
		"0a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529e"
	);

	auto claim = Boltz::Detail::claim_leaf(hash160, claim_key);
	assert( hex(claim)
	     == "82012088a9142c2d5441ef4a4469eae063941463e3c65ee926c088"
		"2007262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779ac"
	      );

	auto refund = Boltz::Detail::refund_leaf(refund_key, 649365);
	assert( hex(refund)
	     == "200a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529e"
		"ad0395e809b1"
	      );

	/* The height is a minimal script number: OP_N up to 16,
	 * then the fewest little-endian bytes, with a sign byte
	 * when the top bit would read as negative.  */
	auto height_push = [&](std::uint32_t h) {
		auto leaf = Boltz::Detail::refund_leaf(refund_key, h);
		/* Strip the key push and OP_CHECKSIGVERIFY in front, and
		 * OP_CHECKLOCKTIMEVERIFY behind.  */
		return hex(std::vector<std::uint8_t>( leaf.begin() + 34
						    , leaf.end() - 1
						    ));
	};
	assert(height_push(16) == "60");
	assert(height_push(17) == "0111");
	assert(height_push(255) == "02ff00");
	assert(height_push(65535) == "03ffff00");
	assert(height_push(8388608) == "0400008000");
	assert(height_push(649365) == "0395e809");

	/* The tree root does not depend on which leaf is named
	 * first: the leaves are ordered by hash.  */
	auto root = Boltz::Detail::compute_root_hash(claim, refund);
	assert(root.size() == 32);
	assert(root == Boltz::Detail::compute_root_hash(refund, claim));
	/* Any change to a leaf is another tree.  */
	auto other = Boltz::Detail::refund_leaf(refund_key, 649366);
	assert(root != Boltz::Detail::compute_root_hash(claim, other));

	return 0;
}
