#undef NDEBUG
#include"Bitcoin/pubkey_to_scriptPubKey.hpp"
#include"Secp256k1/TapscriptTree.hpp"
#include"Secp256k1/Musig.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Random.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/fun.hpp"
#include"Util/Str.hpp"
#include"external/basicsecure/basicsecure.h"
#include<assert.h>
#include<iostream>
#include<iterator>
#include<sstream>
#include<string>
#include<tuple>
#include<utility>
#include<vector>

using namespace Secp256k1;

class Signer : public Secp256k1::SignerIF {
private:
	Secp256k1::PrivKey sk;

public:
	/* Yeah, everyone totally knows the below private key, so insecure.  */
	Signer() : sk("7FB9E0E687ADA1EEBF7ECFE2F21E73EBDB51A7D450948DFE8D76D7F2D1007671") { }

	Secp256k1::PubKey
	get_pubkey_tweak(Secp256k1::PrivKey const& tweak) override {
		return tweak * Secp256k1::PubKey(sk);
	}
	Secp256k1::Signature
	get_signature_tweak( Secp256k1::PrivKey const& tweak
			   , Sha256::Hash const& m
			   ) override {
		throw std::logic_error("Not expected to use.");
	}
	Sha256::Hash
	get_privkey_salted_hash(std::uint8_t salt[32]) override {
		throw std::logic_error("Not expected to use.");
	}
	Secp256k1::KeyPair
	get_keypair_tweak(Secp256k1::PrivKey const& tweak
			) override {
		return Secp256k1::KeyPair(tweak * sk); 
	}
};


class BIP327MusigSession : public Musig::Session {
public:
	BIP327MusigSession() : Secp256k1::Musig::Session() { }

	BIP327MusigSession(BIP327MusigSession&&) =default;
	BIP327MusigSession& operator= (BIP327MusigSession&&) =default;

	/* disallow copys to prevent the secnonce proliferating.  */
	BIP327MusigSession(BIP327MusigSession const&) =delete;
	BIP327MusigSession& operator=(BIP327MusigSession const&) =delete;

	BIP327MusigSession( Secp256k1::KeyPair const& pair
					  , std::vector<Secp256k1::PubKey> const& pkvec
					  )
		: Musig::Session(pair, pkvec)
	{ }

	~BIP327MusigSession() =default;

	void load_partial( std::string const& psig
					 , Secp256k1::PubKey const& pk
					 ) {
		load_partial_at(psig, pk);
	}
	void load_pubnonce( std::string const& pnonce
					  , PubKey const& pk
					  ) {
		load_pubnonce_at(pnonce, pk);
	}
};

static std::vector<uint8_t> getroothash(std::string const& a, std::string const& b) {
	auto claimscript = Util::Str::hexread(a);
	auto refundscript = Util::Str::hexread(b);

	/* leaves to buffers */
	auto leaf0 = Secp256k1::TapTree::encoded_leaf(claimscript);
	auto leaf1 = Secp256k1::TapTree::encoded_leaf(refundscript);

	/* tagged hashes of leaves */
	auto leafhashes = Secp256k1::TapTree::LeafPair(leaf0, leaf1);

	/* combine leaves into a branch */
	return leafhashes.compute_branch_hash();
}


int main() {
	auto rand = Random();
	auto signer = Signer();

	auto tweak0 = PrivKey(rand);
	auto tweak1 = PrivKey(rand);
	auto tweak2 = PrivKey(rand);
	auto pk0 = signer.get_pubkey_tweak(tweak0);
	auto pk1 = signer.get_pubkey_tweak(tweak1);
	auto pk2 = signer.get_pubkey_tweak(tweak2);
	auto sighash = Sha256::Hash("F95466D086770E689964664219266FE5ED215C92AE20BAB5C9D79ADDDDF3C0CF");
	const std::string leafA = "2044b178d64c32c4a05cc4f4d1407268f764c940d20ce97abfd44db5c3592b72fdac";
	const std::string leafB = "07546170726f6f74";

	auto pkvec = std::vector<PubKey>{pk0, pk1, pk2};

	auto kp0 = signer.get_keypair_tweak(tweak0);
	auto session0 = BIP327MusigSession(kp0, pkvec);
	session0.load_sighash(sighash);
	auto pn0 = session0.generate_local_nonces(rand);

	auto kp1 = signer.get_keypair_tweak(tweak1);
	auto session1 = BIP327MusigSession(kp1, pkvec);
	session1.load_sighash(sighash);
	auto pn1 = session1.generate_local_nonces(rand);

	auto kp2 = signer.get_keypair_tweak(tweak2);
	auto session2 = BIP327MusigSession(kp2, pkvec);
	session2.load_sighash(sighash);
	auto pn2 = session2.generate_local_nonces(rand);

	/* combine leaves into a branch */
	auto root_hash = getroothash(leafA, leafB);
	session0.apply_xonly_tweak(root_hash);
	session1.apply_xonly_tweak(root_hash);
	session2.apply_xonly_tweak(root_hash);
	auto redeemscript0 = Bitcoin::pk_to_scriptpk(session0.get_output_key());
	auto redeemscript2 = Bitcoin::pk_to_scriptpk(session2.get_output_key());

	assert(memcmp(&redeemscript0[0], &redeemscript2[0], 32) == 0);

	session0.load_pubnonce(pn1, pk1);
	session0.load_pubnonce(pn2, pk2);
	session1.load_pubnonce(pn0, pk0);
	session1.load_pubnonce(pn2, pk2);
	session2.load_pubnonce(pn0, pk0);
	session2.load_pubnonce(pn1, pk1);

	session0.aggregate_pubnonces();
	session1.aggregate_pubnonces();
	session2.aggregate_pubnonces();

	session0.sign_partial();
	session1.sign_partial();
	session2.sign_partial();

	auto part0 = session0.serialize_partial();
	auto part1 = session1.serialize_partial();
	auto part2 = session2.serialize_partial();
	session0.load_partial(part1, pk1);
	session0.load_partial(part2, pk2);
	session1.load_partial(part0, pk0);
	session1.load_partial(part2, pk2);
	session2.load_partial(part0, pk0);
	session2.load_partial(part1, pk1);

	session0.verify_part_sigs();
	session1.verify_part_sigs();
	session2.verify_part_sigs();

	auto tweakedagg_xonly = session0.get_output_key();

	std::uint8_t aggsig_buffer[64];
	session0.aggregate_partials(aggsig_buffer);
	auto sig0 = SchnorrSig::from_buffer(aggsig_buffer);

	assert(sig0.valid(tweakedagg_xonly, sighash));

	return 0;
}
