#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Bitcoin/TxId.hpp"
#include"Bitcoin/hash160.hpp"
#include"Bitcoin/pubkey_to_scriptPubKey.hpp"
#include"Bitcoin/sighash.hpp"
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/ClaimTxHandler.hpp"
#include"Boltz/Detail/compute_preimage.hpp"
#include"Boltz/Detail/swaptree.hpp"
#include"Boltz/EnvIF.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/Preimage.hpp"
#include"Ripemd160/Hash.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/Musig.hpp"
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

/* Drives Boltz::Detail::ClaimTxHandler through one cooperative
 * (key-path) claim against a mock provider that co-signs the way
 * a Boltz API v2 backend does, and checks:
 *
 * - the claim request carries the real preimage, the one whose
 *   SHA-256 the swap was created with, not the in-database
 *   material it is derived from;
 * - the broadcast claim transaction is a valid BIP341 key-path
 *   spend: one witness item, a 64-byte signature that verifies
 *   against the lockup output key over the transaction's sighash;
 * - the final database update stamps only the claimed swap, not a
 *   sibling swap in flight on the same provider, nor a swap with
 *   the same id on another provider.
 */

namespace {

/* Mock environment: fixed feerate, broadcast always succeeds
 * and keeps the transaction it was handed.
 */
class MockEnv : public Boltz::EnvIF {
private:
	bool& broadcast_called;
	Bitcoin::Tx& broadcast_tx_seen;

public:
	MockEnv( bool& broadcast_called_
	       , Bitcoin::Tx& broadcast_tx_seen_
	       ) : broadcast_called(broadcast_called_)
		 , broadcast_tx_seen(broadcast_tx_seen_)
		 { }

	Ev::Io<std::uint32_t> get_feerate() override {
		return Ev::lift(std::uint32_t(1000));
	}
	Ev::Io<bool> broadcast_tx(Bitcoin::Tx tx) override {
		broadcast_called = true;
		broadcast_tx_seen = std::move(tx);
		return Ev::lift(true);
	}
	Ev::Io<void> logd(std::string) override { return Ev::lift(); }
	Ev::Io<void> loge(std::string msg) override {
		/* Visible when the handler stops early.  */
		std::cerr << "ERROR " << msg << std::endl;
		return Ev::lift();
	}
};

/* Mock signer backed by a fixed privkey; the tweak is ignored,
 * so the pubkey and the keypair agree with each other.
 */
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

/* A MuSig2 session with the provider's role: its key first, the
 * client's second, as the Boltz API v2 aggregates them.
 */
class ProviderSession : public Secp256k1::Musig::Session {
private:
	Secp256k1::PubKey client;

public:
	ProviderSession( Secp256k1::KeyPair const& provider_keys
		       , Secp256k1::PubKey const& client_
		       ) : Secp256k1::Musig::Session
				( provider_keys
				, {provider_keys.pub(), client_}
				)
			 , client(client_)
			 { }

	void load_client_pubnonce(std::string const& pn) {
		load_pubnonce_at(pn, client);
	}
};

/* Mock provider: answers the cooperative claim request with its
 * public nonce and partial signature over the sighash of the
 * transaction the client sent, and keeps what the client sent.
 */
class MockProvider : public Boltz::ConnectionIF {
private:
	Secp256k1::Random& random;
	Secp256k1::KeyPair const& keys;
	Secp256k1::PubKey const& client;
	std::vector<std::uint8_t> const& roothash;
	std::vector<std::uint8_t> const& lockup_scriptPubKey;
	Ln::Amount lockup_amount;

public:
	std::string path_seen;
	std::string preimage_seen;

	MockProvider( Secp256k1::Random& random_
		    , Secp256k1::KeyPair const& keys_
		    , Secp256k1::PubKey const& client_
		    , std::vector<std::uint8_t> const& roothash_
		    , std::vector<std::uint8_t> const& lockup_scriptPubKey_
		    , Ln::Amount lockup_amount_
		    ) : random(random_)
		      , keys(keys_)
		      , client(client_)
		      , roothash(roothash_)
		      , lockup_scriptPubKey(lockup_scriptPubKey_)
		      , lockup_amount(lockup_amount_)
		      { }

	Ev::Io<Jsmn::Object>
	api( std::string path
	   , std::unique_ptr<Json::Out> params
	   ) override {
		path_seen = path;
		assert(params);
		auto req = Jsmn::Object::parse_json(params->output().c_str());
		preimage_seen = std::string(req["preimage"]);
		auto client_pubnonce = std::string(req["pubNonce"]);
		auto tx = Bitcoin::Tx(std::string(req["transaction"]));
		assert(std::uint32_t((double) req["index"]) == 0);

		/* Sighash the way the provider computes it, from the
		 * transaction and the output being spent.  */
		auto prefixed = std::vector<std::uint8_t>(1);
		prefixed[0] = std::uint8_t(lockup_scriptPubKey.size());
		prefixed.insert( prefixed.end()
			       , lockup_scriptPubKey.begin()
			       , lockup_scriptPubKey.end()
			       );
		auto sighash = Bitcoin::p2tr_sighash
			( tx
			, Bitcoin::SIGHASH_DEFAULT
			, 0
			, std::vector<Ln::Amount>{lockup_amount}
			, std::vector<std::vector<std::uint8_t>>{prefixed}
			, Bitcoin::KEYPATH
			);

		auto session = ProviderSession(keys, client);
		session.load_sighash(sighash);
		auto pubnonce = session.generate_local_nonces(random);
		session.apply_xonly_tweak(roothash);
		session.load_client_pubnonce(client_pubnonce);
		session.aggregate_pubnonces();
		session.sign_partial();
		auto partial = session.serialize_partial();

		auto res = Json::Out()
			.start_object()
				.field("pubNonce", pubnonce)
				.field("partialSignature", partial)
			.end_object()
			;
		return Ev::lift(Jsmn::Object::parse_json(res.output().c_str()));
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
     , claimScript TEXT NOT NULL
     , refundScript TEXT NOT NULL
     , refundPubkey TEXT NOT NULL
     , timeoutBlockheight INTEGER NOT NULL
     , onchainAmount INTEGER NOT NULL
     , lockedUp INTEGER NOT NULL
     , lockupTxid TEXT NULL
     , lockupOut INTEGER NULL
     , lockupConfirmedHeight INTEGER NULL
     , lockupClaimFees INTEGER NULL
     , comment TEXT
     );
)QRY";

std::string hex(std::vector<std::uint8_t> const& v) {
	return Util::Str::hexdump(&v[0], v.size());
}

}

int main() {
	auto db = Sqlite3::Db(":memory:");
	auto random = Secp256k1::Random();

	auto const api = std::string("testapi");
	auto const tweak = std::string(
		"0202020202020202020202020202020202020202020202020202020202020202"
	);
	auto const preimage = std::string(
		"0303030303030303030303030303030303030303030303030303030303030303"
	);
	auto const addr = std::string(
		"bc1qg430dgu75qvphu8nn3kvcp3yg2km2tavpc8lqs"
	);
	auto const timeout = std::uint32_t(1000);
	auto const amount = Ln::Amount::sat(100000);
	auto const blockheight = std::uint32_t(100);

	auto signer = MockSigner();
	auto const client_pub = signer.get_pubkey_tweak(
		Secp256k1::PrivKey(tweak)
	);
	auto const provider_keys = Secp256k1::KeyPair(Secp256k1::PrivKey(
		std::string(
		"0505050505050505050505050505050505050505050505050505050505050505"
		)
	));

	/* The real preimage, as the handler derives it, and its
	 * hash160 for the claim leaf.  */
	auto const real_preimage = Boltz::Detail::compute_preimage(
		signer, Ln::Preimage(preimage)
	);
	std::uint8_t preimage_buf[32];
	real_preimage.to_buffer(preimage_buf);
	auto const preimage_hash160 = Bitcoin::hash160(preimage_buf, 32);

	/* The two tapscript leaves of a Boltz reverse swap.  */
	std::uint8_t key_buf[32];
	auto claimScript = std::vector<std::uint8_t>{
		0x82, 0x01, 0x20, 0x88, 0xa9, 0x14
	};
	claimScript.resize(claimScript.size() + 20);
	preimage_hash160.to_buffer(&claimScript[6]);
	claimScript.push_back(0x88);
	claimScript.push_back(0x20);
	Secp256k1::XonlyPubKey::from_ecdsa_pk(client_pub).to_buffer(key_buf);
	claimScript.insert(claimScript.end(), key_buf, key_buf + 32);
	claimScript.push_back(0xac);

	auto refundScript = std::vector<std::uint8_t>{0x20};
	Secp256k1::XonlyPubKey::from_ecdsa_pk(provider_keys.pub())
		.to_buffer(key_buf);
	refundScript.insert(refundScript.end(), key_buf, key_buf + 32);
	refundScript.push_back(0xad);
	refundScript.push_back(0x03);
	refundScript.push_back(std::uint8_t(timeout & 0xff));
	refundScript.push_back(std::uint8_t((timeout >> 8) & 0xff));
	refundScript.push_back(std::uint8_t((timeout >> 16) & 0xff));
	refundScript.push_back(0xb1);

	auto const roothash = Boltz::Detail::compute_root_hash(
		claimScript, refundScript
	);

	/* The lockup output key: the aggregate of both keys, tweaked
	 * with the tree root.  */
	auto output_key = Secp256k1::XonlyPubKey();
	{
		auto session = ProviderSession(provider_keys, client_pub);
		session.apply_xonly_tweak(roothash);
		output_key = session.get_output_key();
	}
	auto const lockup_scriptPubKey = Bitcoin::pk_to_scriptpk(output_key);

	auto lockup_tx = Bitcoin::Tx();
	lockup_tx.inputs.resize(1);
	lockup_tx.inputs[0].prevTxid = Bitcoin::TxId(std::string(
		"0404040404040404040404040404040404040404040404040404040404040404"
	));
	lockup_tx.inputs[0].prevOut = 0;
	lockup_tx.outputs.resize(1);
	lockup_tx.outputs[0].scriptPubKey = lockup_scriptPubKey;
	lockup_tx.outputs[0].amount = amount;

	auto broadcast_called = bool(false);
	auto broadcast_tx = Bitcoin::Tx();
	auto env = MockEnv(broadcast_called, broadcast_tx);
	auto provider = MockProvider( random
				    , provider_keys
				    , client_pub
				    , roothash
				    , lockup_scriptPubKey
				    , amount
				    );

	auto insert_swap = [&](std::string apiAccess, std::string swapId) {
		return db.transact().then([&, apiAccess, swapId](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			INSERT INTO "BoltzServiceFactory_rsub"
			     ( apiAccess
			     , tweak
			     , preimage
			     , destinationAddress
			     , swapId
			     , claimScript
			     , refundScript
			     , refundPubkey
			     , timeoutBlockheight
			     , onchainAmount
			     , lockedUp
			     , comment
			     )
			VALUES
			     ( :apiAccess
			     , :tweak
			     , :preimage
			     , :destinationAddress
			     , :swapId
			     , :claimScript
			     , :refundScript
			     , :refundPubkey
			     , :timeoutBlockheight
			     , :onchainAmount
			     , 0
			     , ''
			     );
			)QRY")
				.bind(":apiAccess", apiAccess)
				.bind(":tweak", tweak)
				.bind(":preimage", preimage)
				.bind(":destinationAddress", addr)
				.bind(":swapId", swapId)
				.bind(":claimScript", hex(claimScript))
				.bind(":refundScript", hex(refundScript))
				.bind(":refundPubkey", std::string(provider_keys.pub()))
				.bind(":timeoutBlockheight", timeout)
				.bind(":onchainAmount", amount.to_sat())
				.execute();
			tx.commit();
			return Ev::lift();
		});
	};

	auto code = Ev::lift().then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query_execute(schema);
		tx.commit();
		/* Two swaps in flight at once.  */
		return insert_swap(api, "swapA");
	}).then([&]() {
		return insert_swap(api, "swapB");
	}).then([&]() {
		/* A third swap with the same swapId under a different
		 * apiAccess, so the test fails if the apiAccess
		 * predicate is dropped from the update.
		 */
		return insert_swap("other-api", "swapA");
	}).then([&]() {
		/* Claim the first swap.  */
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapA", blockheight, lockup_tx
		);
		return handler->run();
	}).then([&]() {
		/* The handler must have reached the broadcast,
		 * otherwise the assertions below are vacuous.
		 */
		assert(broadcast_called);

		/* The claim request went to the v2 endpoint with the
		 * real preimage.  */
		assert(provider.path_seen == "/v2/swap/reverse/swapA/claim");
		assert(provider.preimage_seen == std::string(real_preimage));

		/* A key-path spend of the lockup output: one witness
		 * item, a 64-byte signature valid for the output key
		 * over the sighash of the broadcast transaction.  */
		assert(broadcast_tx.inputs.size() == 1);
		assert(broadcast_tx.inputs[0].prevTxid == lockup_tx.get_txid());
		assert(broadcast_tx.inputs[0].prevOut == 0);
		auto const& witness = broadcast_tx.inputs[0].witness.witnesses;
		assert(witness.size() == 1);
		assert(witness[0].size() == 64);
		auto prefixed = std::vector<std::uint8_t>(1);
		prefixed[0] = std::uint8_t(lockup_scriptPubKey.size());
		prefixed.insert( prefixed.end()
			       , lockup_scriptPubKey.begin()
			       , lockup_scriptPubKey.end()
			       );
		auto sighash = Bitcoin::p2tr_sighash
			( broadcast_tx
			, Bitcoin::SIGHASH_DEFAULT
			, 0
			, std::vector<Ln::Amount>{amount}
			, std::vector<std::vector<std::uint8_t>>{prefixed}
			, Bitcoin::KEYPATH
			);
		std::uint8_t sig_buf[64];
		std::copy(witness[0].begin(), witness[0].end(), sig_buf);
		auto sig = Secp256k1::SchnorrSig::from_buffer(sig_buf);
		assert(sig.valid(output_key, sighash));

		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		/* Sanity: the claimed swap was stamped.  */
		auto stamped = tx.query(R"QRY(
		SELECT lockedUp, lockupTxid
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapA'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		auto found = false;
		for (auto& r : stamped) {
			found = true;
			assert(r.get<bool>(0));
			assert(!r.get<std::string>(1).empty());
		}
		assert(found);

		/* Regression: the sibling swap must be untouched.  */
		auto sibling = tx.query(R"QRY(
		SELECT lockedUp
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapB'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		found = false;
		for (auto& r : sibling) {
			found = true;
			assert(!r.get<bool>(0));
		}
		assert(found);

		/* Exactly one row may be stamped overall.  */
		auto count = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
		 WHERE lockedUp <> 0
		    OR lockupTxid IS NOT NULL
		     ;
		)QRY").execute();
		found = false;
		for (auto& r : count) {
			found = true;
			assert(r.get<int>(0) == 1);
		}
		assert(found);

		tx.commit();
		return Ev::lift(0);
	});

	return Ev::start(code);
}
