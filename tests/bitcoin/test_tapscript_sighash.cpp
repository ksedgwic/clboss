#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Bitcoin/sighash.hpp"
#include"Ln/Amount.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/TapscriptTree.hpp"
#include"Sha256/Hash.hpp"
#include"Util/Str.hpp"
#include<algorithm>
#include<assert.h>
#include<cstdint>
#include<string>
#include<utility>
#include<vector>

namespace {

/* Script-path spends from the Bitcoin main chain.  The network
 * accepted the signatures in their witnesses, so each verifies
 * against the key in its leaf only if our message, extension
 * included, is the one the signer hashed.  All use
 * SIGHASH_DEFAULT (64-byte signatures) and a single leaf.  */
struct Vector {
	char const* tx_hex;
	/* The spent output: its scriptPubKey without the length
	 * prefix, and its amount.  */
	char const* prev_scriptPubKey;
	std::uint64_t prev_amount;
	/* Witness item holding the leaf script.  */
	std::size_t script_index;
	/* Witness item of each signature, and the x-only key in
	 * the leaf it was made with.  */
	std::vector<std::pair<std::size_t, char const*>> sigs;
};

/* Block 973579, txid 10bd3e70d85e8e0172f9d28ddb80e21433ded10e2b01ea7e3ff8fd1ab82f25c5.
 * Leaf: <3 bytes> OP_DROP <key> OP_CHECKSIG.  One input, one output.  */
Vector const v1 = {
	"020000000001010e4f4ddaa73fbf1d7c4832f98ac824e42c9c997d670ee314fa"
	"6219d2e1ebd6cd0100000000fdffffff01ceec5e0100000000160014"
	"37d46cc6ed1962d9c9175a9e12154b3cd2a258740340a9c0d76aee27e505"
	"493753b1f1419a2d26abfa7cfe7e15d73367adc9bc1e2003a5e1b5bde2f7"
	"168c752de611d7a4714c33117dcea46f00b3fb1b8666fd24f73e27030e8a"
	"0375207f2252dfc02146effe70487bf49b725e412ab255b41a75913a61ce"
	"360080b53fac21c0eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeee00000000",
	"512016829c968e0b792f7b5bd03ab03ba435cc18f4462caba29942e583fe20f07079",
	23000000,
	1,
	{ {0, "7f2252dfc02146effe70487bf49b725e412ab255b41a75913a61ce360080b53f"} }
};

/* Block 973577, txid eca1e6ffa41904aad4f1b571af8f8e8542ed7caf7a2fc875a640a4b92f19670a.
 * Leaf: <key1> OP_CHECKSIGVERIFY <key2> OP_CHECKSIG, so two
 * signatures over one message; nVersion 3, three outputs, a
 * relative-locktime nSequence.  OP_CHECKSIGVERIFY takes the
 * top item, witness 1, with key1; OP_CHECKSIG takes witness 0
 * with key2.  */
Vector const v2 = {
	"030000000001013b2f52cfb2d22e15d145a623baf0c8231da1bd5e25639dfabb"
	"96a82d7daacd18000000000001a700c003e8030000000000002251202"
	"5a2678ac16ae78d1d10d5c0d617ee723d6cb3ff49b62baae3f52381cdc5"
	"d6f04231020000000000225120c54b8269bac7ff0166f4bf7b0457d2b7ce"
	"6c8b736528d1f6786d1b46faef441c0000000000000000066a5803010101"
	"04408b605ae37c86668d3e5454514ebcc8d70516f6973faf07457a2b1659"
	"e913087f94ac3780bf600360277535ab59d3c34a8dd7054ed6ea686fe33d"
	"359166671157400ae22a1c5ed53f8e70a30011a6cd061145c2d2b85ba4af"
	"0450743b64843e5a0468a1b2ca36127fb3c8e88a094b9cb604794b48e67f"
	"303d8871fe57da729bccf544209436d32c0ab9a598767f0f775e1c838403"
	"7b428110b5ac52b763e3e13f62cc64ad20e8350d2c8efa4a670448404dc6"
	"f230e55d3b06803f1b53172e7afde922495196ac21c050929b74c1a04954"
	"b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac000000000",
	"512025a2678ac16ae78d1d10d5c0d617ee723d6cb3ff49b62baae3f52381cdc5d6f0",
	145582,
	2,
	{ {1, "9436d32c0ab9a598767f0f775e1c8384037b428110b5ac52b763e3e13f62cc64"}
	, {0, "e8350d2c8efa4a670448404dc6f230e55d3b06803f1b53172e7afde922495196"}
	}
};

std::vector<std::uint8_t> prefixed(std::string const& hex) {
	auto spk = Util::Str::hexread(hex);
	auto out = std::vector<std::uint8_t>();
	out.push_back(std::uint8_t(spk.size()));
	out.insert(out.end(), spk.begin(), spk.end());
	return out;
}

Secp256k1::SchnorrSig signature(std::vector<std::uint8_t> const& item) {
	assert(item.size() == 64);
	std::uint8_t buf[64];
	std::copy(item.begin(), item.end(), buf);
	return Secp256k1::SchnorrSig::from_buffer(buf);
}

void check(Vector const& v) {
	auto tx = Bitcoin::Tx(std::string(v.tx_hex));
	assert(tx.inputs.size() == 1);
	auto const& witness = tx.inputs[0].witness.witnesses;

	auto amounts = std::vector<Ln::Amount>{Ln::Amount::sat(v.prev_amount)};
	auto spks = std::vector<std::vector<std::uint8_t>>{
		prefixed(v.prev_scriptPubKey)
	};

	auto leaf = Secp256k1::TapTree::leaf_hash(witness[v.script_index]);
	assert(leaf.size() == 32);
	auto extension = Bitcoin::TapscriptExtension();
	extension.leaf_hash.from_buffer(leaf.data());

	auto message = Bitcoin::p2tr_sighash
		( tx, Bitcoin::SIGHASH_DEFAULT, 0, amounts, spks, extension );
	auto keypath_message = Bitcoin::p2tr_sighash
		( tx, Bitcoin::SIGHASH_DEFAULT, 0, amounts, spks );
	auto other = extension;
	other.codesep_pos = 0;
	auto other_message = Bitcoin::p2tr_sighash
		( tx, Bitcoin::SIGHASH_DEFAULT, 0, amounts, spks, other );

	for (auto const& s : v.sigs) {
		auto sig = signature(witness[s.first]);
		auto key = Secp256k1::XonlyPubKey(std::string(s.second));
		/* The signer hashed the leaf into the message.  */
		assert(sig.valid(key, message));
		/* Without the extension, or with a different
		 * extension, it is another message.  */
		assert(!sig.valid(key, keypath_message));
		assert(!sig.valid(key, other_message));
	}
}

}

int main() {
	check(v1);
	check(v2);

	/* One amount and one scriptPubKey per input, or the
	 * message cannot be formed.  */
	auto tx = Bitcoin::Tx(std::string(v1.tx_hex));
	auto threw = false;
	try {
		Bitcoin::p2tr_sighash
			( tx, Bitcoin::SIGHASH_DEFAULT, 0
			, std::vector<Ln::Amount>()
			, std::vector<std::vector<std::uint8_t>>()
			);
	} catch (Bitcoin::InvalidSighash const&) {
		threw = true;
	}
	assert(threw);

	return 0;
}
