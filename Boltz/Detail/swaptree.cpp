#include"Boltz/Detail/swaptree.hpp"
#include"Ripemd160/Hash.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/TapscriptTree.hpp"

namespace {

/* Append a script push of the number N in its minimal
 * form: OP_0, OP_1 to OP_16, or a data push of the
 * little-endian magnitude with a sign byte added when
 * the top bit is set, as CScriptNum serializes.  */
void push_number(std::vector<std::uint8_t>& script, std::uint32_t n) {
	if (n == 0) {
		script.push_back(0x00);
		return;
	}
	if (n <= 16) {
		script.push_back(std::uint8_t(0x50 + n));
		return;
	}
	auto bytes = std::vector<std::uint8_t>();
	while (n > 0) {
		bytes.push_back(std::uint8_t(n & 0xff));
		n >>= 8;
	}
	if (bytes.back() & 0x80)
		bytes.push_back(0x00);
	script.push_back(std::uint8_t(bytes.size()));
	script.insert(script.end(), bytes.begin(), bytes.end());
}

void push_key( std::vector<std::uint8_t>& script
	     , Secp256k1::XonlyPubKey const& key
	     ) {
	std::uint8_t buf[32];
	key.to_buffer(buf);
	script.push_back(0x20);
	script.insert(script.end(), buf, buf + 32);
}

}

namespace Boltz { namespace Detail {

std::vector<std::uint8_t> claim_leaf
				( Ripemd160::Hash const& preimage_hash160
				, Secp256k1::XonlyPubKey const& claim_key
				) {
	std::uint8_t hash[20];
	preimage_hash160.to_buffer(hash);

	auto script = std::vector<std::uint8_t>();
	script.push_back(0x82); /* OP_SIZE */
	script.push_back(0x01); /* push one byte: 32 */
	script.push_back(0x20);
	script.push_back(0x88); /* OP_EQUALVERIFY */
	script.push_back(0xa9); /* OP_HASH160 */
	script.push_back(0x14); /* push 20 bytes */
	script.insert(script.end(), hash, hash + 20);
	script.push_back(0x88); /* OP_EQUALVERIFY */
	push_key(script, claim_key);
	script.push_back(0xac); /* OP_CHECKSIG */
	return script;
}

std::vector<std::uint8_t> refund_leaf
				( Secp256k1::XonlyPubKey const& refund_key
				, std::uint32_t timeout_blockheight
				) {
	auto script = std::vector<std::uint8_t>();
	push_key(script, refund_key);
	script.push_back(0xad); /* OP_CHECKSIGVERIFY */
	push_number(script, timeout_blockheight);
	script.push_back(0xb1); /* OP_CHECKLOCKTIMEVERIFY */
	return script;
}

std::vector<std::uint8_t> compute_root_hash
					( std::vector<std::uint8_t> const& claimscript
					, std::vector<std::uint8_t> const& refundscript
					) {
	using namespace Secp256k1;
	/* leaves to buffers */
	auto leaf0 = TapTree::encoded_leaf(claimscript);
	auto leaf1 = TapTree::encoded_leaf(refundscript);

	/* tagged hashes of leaves */
	auto leafhashes = TapTree::LeafPair(leaf0, leaf1);

	/* combine leaves into a branch */
	return leafhashes.compute_branch_hash();
}

}}
