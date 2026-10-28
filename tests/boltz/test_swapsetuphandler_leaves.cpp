#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/SwapSetupHandler.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boltz/SwapInfo.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ripemd160/Hash.hpp"
#include"Ripemd160/Hasher.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Random.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include<assert.h>
#include<cstdint>
#include<iostream>
#include<memory>
#include<string>
#include<vector>

/* Drives Boltz::Detail::SwapSetupHandler against a mock provider
 * that answers the reverse swap creation the way a Boltz API v2
 * backend does, building the tapscript leaves from the request,
 * and checks that the handler:
 *
 * - accepts a swap whose claim leaf pays to our key on our preimage
 *   hash and whose refund leaf carries the provider's refund key
 *   and the timeout, and records it;
 * - rejects a claim leaf with another key, which would leave the
 *   unilateral claim to someone else;
 * - rejects a refund leaf whose key is not the refundPublicKey the
 *   provider declared;
 * - rejects a refund leaf whose height is not pushed as a minimal
 *   script number, since the leaves must match ours byte for byte.
 */

namespace {

class MockEnv : public Boltz::EnvIF {
public:
	Ev::Io<std::uint32_t> get_feerate() override {
		return Ev::lift(std::uint32_t(1000));
	}
	Ev::Io<std::string> get_block(std::uint32_t) override {
		return Ev::lift(std::string());
	}
	Ev::Io<bool> has_confirmed_output(Bitcoin::TxId, std::uint32_t) override {
		return Ev::lift(false);
	}
	Ev::Io<bool> broadcast_tx(Bitcoin::Tx) override {
		return Ev::lift(true);
	}
	Ev::Io<void> logd(std::string) override { return Ev::lift(); }
	Ev::Io<void> loge(std::string msg) override {
		std::cerr << "ERROR " << msg << std::endl;
		return Ev::lift();
	}
};

class MockSigner : public Secp256k1::SignerIF {
private:
	Secp256k1::PrivKey privkey;

public:
	MockSigner()
		: privkey(Secp256k1::PrivKey(std::string(
			"0101010101010101010101010101010101010101010101010101010101010101"
		  )))
		{ }

	Secp256k1::PubKey get_pubkey_tweak(Secp256k1::PrivKey const&) override {
		return Secp256k1::PubKey(privkey);
	}
	Secp256k1::Signature get_signature_tweak( Secp256k1::PrivKey const&
						, Sha256::Hash const& m
						) override {
		return Secp256k1::Signature::create(privkey, m);
	}
	Sha256::Hash get_privkey_salted_hash(std::uint8_t salt[32]) override {
		return Sha256::fun(salt, 32);
	}
	Secp256k1::KeyPair
	get_keypair_tweak(Secp256k1::PrivKey const&) override {
		return Secp256k1::KeyPair(privkey);
	}
};

enum Variant
{ Honest
, ForeignClaimKey
, RefundKeyMismatch
, NonMinimalTimeout
};

std::string hex(std::vector<std::uint8_t> const& v) {
	return Util::Str::hexdump(&v[0], v.size());
}

/* Mock provider: builds the swap tree from the request, as the
 * backend does, with one leaf wrong per VARIANT.
 */
class MockProvider : public Boltz::ConnectionIF {
private:
	Variant variant;
	Secp256k1::PubKey refund_key;
	Secp256k1::PubKey other_key;
	std::uint32_t timeout;

public:
	std::string path_seen;

	MockProvider( Variant variant_
		    , Secp256k1::PubKey const& refund_key_
		    , Secp256k1::PubKey const& other_key_
		    , std::uint32_t timeout_
		    ) : variant(variant_)
		      , refund_key(refund_key_)
		      , other_key(other_key_)
		      , timeout(timeout_)
		      { }

	Ev::Io<Jsmn::Object>
	api( std::string path
	   , std::unique_ptr<Json::Out> params
	   ) override {
		path_seen = path;
		assert(params);
		auto req = Jsmn::Object::parse_json(params->output().c_str());
		auto preimage_hash = Sha256::Hash(std::string(req["preimageHash"]));
		auto claim_key = Secp256k1::PubKey(std::string(req["claimPublicKey"]));
		auto invoice_amount = std::uint64_t((double) req["invoiceAmount"]);

		/* hash160 of the preimage is RIPEMD160 of its SHA-256,
		 * which is what the client sent.  */
		std::uint8_t buf[32];
		preimage_hash.to_buffer(buf);
		auto hasher = Ripemd160::Hasher();
		hasher.feed(buf, 32);
		auto hash160 = std::move(hasher).finalize();

		auto claim_leaf_key = variant == ForeignClaimKey
				    ? other_key : claim_key;
		auto refund_leaf_key = variant == RefundKeyMismatch
				     ? other_key : refund_key;

		/* OP_SIZE 32 OP_EQUALVERIFY OP_HASH160 <hash160>
		 * OP_EQUALVERIFY <key> OP_CHECKSIG  */
		auto claimScript = std::vector<std::uint8_t>(61);
		claimScript[0] = 0x82;
		claimScript[1] = 0x01;
		claimScript[2] = 0x20;
		claimScript[3] = 0x88;
		claimScript[4] = 0xa9;
		claimScript[5] = 0x14;
		hash160.to_buffer(&claimScript[6]);
		claimScript[26] = 0x88;
		claimScript[27] = 0x20;
		Secp256k1::XonlyPubKey::from_ecdsa_pk(claim_leaf_key)
			.to_buffer(&claimScript[28]);
		claimScript[60] = 0xac;

		/* <key> OP_CHECKSIGVERIFY <timeout> OP_CHECKLOCKTIMEVERIFY,
		 * the timeout a minimal script number as the backend
		 * writes it (little-endian, a zero byte added when the
		 * top bit is set), with one needless zero byte more for
		 * NonMinimalTimeout.  */
		auto refundScript = std::vector<std::uint8_t>(34);
		refundScript[0] = 0x20;
		Secp256k1::XonlyPubKey::from_ecdsa_pk(refund_leaf_key)
			.to_buffer(&refundScript[1]);
		refundScript[33] = 0xad;
		auto number = std::vector<std::uint8_t>();
		for (auto n = timeout; n > 0; n >>= 8)
			number.push_back(std::uint8_t(n & 0xff));
		if (number.back() & 0x80)
			number.push_back(0x00);
		if (variant == NonMinimalTimeout)
			number.push_back(0x00);
		refundScript.push_back(std::uint8_t(number.size()));
		refundScript.insert( refundScript.end()
				   , number.begin(), number.end()
				   );
		refundScript.push_back(0xb1);

		auto res = std::string("{")
			 + "\"id\":\"swapA\","
			 + "\"invoice\":\"lnbc1testinvoice\","
			 + "\"swapTree\":{"
			 +   "\"claimLeaf\":{\"version\":192,\"output\":\""
			 +     hex(claimScript) + "\"},"
			 +   "\"refundLeaf\":{\"version\":192,\"output\":\""
			 +     hex(refundScript) + "\"}"
			 + "},"
			 + "\"lockupAddress\":\"bc1ptest\","
			 + "\"refundPublicKey\":\"" + std::string(refund_key) + "\","
			 + "\"timeoutBlockHeight\":" + std::to_string(timeout) + ","
			 + "\"onchainAmount\":" + std::to_string(invoice_amount - 1000)
			 + "}"
			 ;
		return Ev::lift(Jsmn::Object::parse_json(res.c_str()));
	}
};

/* Table schema, copied from Boltz/ServiceFactory.cpp.  */
char const schema[] = R"QRY(
CREATE TABLE "BoltzServiceFactory_rsub"
     ( id INTEGER PRIMARY KEY
     , apiAccess TEXT NOT NULL
     , tweak TEXT NOT NULL
     , preimage TEXT NOT NULL
     , destinationAddress TEXT NOT NULL
     , swapId TEXT NOT NULL
     , redeemScript TEXT NOT NULL DEFAULT ''
     , claimScript TEXT NOT NULL DEFAULT ''
     , refundScript TEXT NOT NULL DEFAULT ''
     , refundPubkey TEXT NOT NULL DEFAULT ''
     , timeoutBlockheight INTEGER NOT NULL
     , onchainAmount INTEGER NOT NULL
     , createdHeight INTEGER NOT NULL DEFAULT 0
     , lockedUp INTEGER NOT NULL
     , lockupTxid TEXT NULL
     , lockupOut INTEGER NULL
     , lockupConfirmedHeight INTEGER NULL
     , lockupSearchedHeight INTEGER NULL
     , spendSearchedHeight INTEGER NULL
     , preimageRevealed INTEGER NOT NULL DEFAULT 0
     , lockupTx TEXT NOT NULL DEFAULT ''
     , claimTx TEXT NOT NULL DEFAULT ''
     , claimFeerate INTEGER NOT NULL DEFAULT 0
     , lockupClaimFees INTEGER NULL
     , comment TEXT
     );
)QRY";

/* Runs one setup against a provider of the given VARIANT and
 * returns the invoice the handler reports, "" on rejection, and
 * the number of rows the handler recorded.
 */
struct Outcome {
	std::string invoice;
	int rows;
};

Ev::Io<Outcome> run_variant( Variant variant
			   , Secp256k1::Random& random
			   , MockSigner& signer
			   , MockEnv& env
			   , Secp256k1::PubKey const& refund_key
			   , Secp256k1::PubKey const& other_key
			   ) {
	auto db = std::make_shared<Sqlite3::Db>(":memory:");
	auto provider = std::make_shared<MockProvider>(
		variant, refund_key, other_key, std::uint32_t(100 + 144)
	);
	auto outcome = std::make_shared<Outcome>();
	return db->transact().then([db](Sqlite3::Tx tx) {
		tx.query_execute(schema);
		tx.commit();
		return Ev::lift();
	}).then([&, db, provider]() {
		auto handler = Boltz::Detail::SwapSetupHandler::create(
			signer, *db, env, "testapi", *provider, random
		      , "bc1qg430dgu75qvphu8nn3kvcp3yg2km2tavpc8lqs"
		      , Ln::Amount::sat(100000)
		      , std::uint32_t(100)
		);
		return handler->run();
	}).then([db, provider, outcome](Boltz::SwapInfo info) {
		assert(provider->path_seen == "/v2/swap/reverse");
		outcome->invoice = info.invoice;
		return db->transact();
	}).then([outcome](Sqlite3::Tx tx) {
		auto count = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub";
		)QRY").execute();
		for (auto& r : count)
			outcome->rows = r.get<int>(0);
		tx.commit();
		return Ev::lift(*outcome);
	});
}

}

int main() {
	auto random = Secp256k1::Random();
	auto signer = MockSigner();
	auto env = MockEnv();
	auto const refund_key = Secp256k1::PubKey(Secp256k1::PrivKey(std::string(
		"0505050505050505050505050505050505050505050505050505050505050505"
	)));
	auto const other_key = Secp256k1::PubKey(Secp256k1::PrivKey(std::string(
		"0707070707070707070707070707070707070707070707070707070707070707"
	)));

	auto code = Ev::lift().then([&]() {
		return run_variant( Honest, random, signer, env
				  , refund_key, other_key
				  );
	}).then([&](Outcome o) {
		/* A correct swap tree is accepted and recorded.  */
		assert(o.invoice == "lnbc1testinvoice");
		assert(o.rows == 1);
		return run_variant( ForeignClaimKey, random, signer, env
				  , refund_key, other_key
				  );
	}).then([&](Outcome o) {
		/* A claim leaf with another key is rejected.  */
		assert(o.invoice == "");
		assert(o.rows == 0);
		return run_variant( RefundKeyMismatch, random, signer, env
				  , refund_key, other_key
				  );
	}).then([&](Outcome o) {
		/* A refund leaf key other than refundPublicKey is
		 * rejected.  */
		assert(o.invoice == "");
		assert(o.rows == 0);
		return run_variant( NonMinimalTimeout, random, signer, env
				  , refund_key, other_key
				  );
	}).then([&](Outcome o) {
		/* The right height in another encoding is another
		 * leaf, rejected.  */
		assert(o.invoice == "");
		assert(o.rows == 0);
		return Ev::lift(0);
	});

	return Ev::start(code);
}
