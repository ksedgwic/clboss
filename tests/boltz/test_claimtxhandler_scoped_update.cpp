#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Bitcoin/Block.hpp"
#include"Bitcoin/addr_to_scriptPubKey.hpp"
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
#include"Secp256k1/TapscriptTree.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include<algorithm>
#include<assert.h>
#include<cstdint>
#include<iostream>
#include<map>
#include<memory>
#include<sstream>
#include<set>
#include<string>
#include<vector>

/* Drives Boltz::Detail::ClaimTxHandler through one cooperative
 * (key-path) claim against a mock provider that co-signs the way
 * a Boltz API v2 backend does, then through one script-path claim
 * against the same provider refusing, and checks:
 *
 * - the claim request carries the real preimage, the one whose
 *   SHA-256 the swap was created with, not the in-database
 *   material it is derived from;
 * - the broadcast claim transaction is a valid BIP341 key-path
 *   spend: one witness item, a 64-byte signature that verifies
 *   against the lockup output key over the transaction's sighash;
 * - the final database update stamps only the claimed swap, not a
 *   sibling swap in flight on the same provider, nor a swap with
 *   the same id on another provider;
 * - the preimage is sent only once our own blocks hold the lockup
 *   three deep, and the reveal is recorded before the request;
 * - a signed claim is rebroadcast each block until our wallet holds
 *   it confirmed, when the swap's row is removed;
 * - a provider that keeps the preimage and refuses to sign gets a
 *   script-path claim in the same block: our signature, the
 *   preimage, the claim leaf and its control block, valid for the
 *   lockup output; it is rebroadcast without asking the provider;
 * - once revealed, the swap ends when our blocks show the lockup
 *   spent: by a transaction paying our address, done; by any
 *   other, lost; or when nothing spends it long after the timeout.
 */

namespace {

/* Mock environment: fixed feerate, broadcast always succeeds
 * and keeps the transaction it was handed, the blocks the test
 * put in `blocks` are our node's chain, and `confirmed` holds
 * the outputs our wallet has in a block, as "txid:outnum".
 */
class MockEnv : public Boltz::EnvIF {
private:
	bool& broadcast_called;
	Bitcoin::Tx& broadcast_tx_seen;

public:
	std::map<std::uint32_t, std::string> blocks;
	std::set<std::string> confirmed;
	std::string last_asked;
	std::uint32_t feerate = 1000;

	MockEnv( bool& broadcast_called_
	       , Bitcoin::Tx& broadcast_tx_seen_
	       ) : broadcast_called(broadcast_called_)
		 , broadcast_tx_seen(broadcast_tx_seen_)
		 { }

	Ev::Io<std::uint32_t> get_feerate() override {
		return Ev::lift(feerate);
	}
	Ev::Io<std::string> get_block(std::uint32_t height) override {
		auto it = blocks.find(height);
		if (it == blocks.end())
			return Ev::lift(std::string());
		return Ev::lift(it->second);
	}
	Ev::Io<bool> has_confirmed_output( Bitcoin::TxId txid
					 , std::uint32_t outnum
					 ) override {
		last_asked = std::string(txid) + ":" + std::to_string(outnum);
		return Ev::lift(confirmed.count(last_asked) != 0);
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
 * With `fail` set it keeps the preimage and answers an error, the
 * way a provider that pockets the preimage would.
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
	bool fail = false;

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
		if (fail)
			return Ev::lift(Jsmn::Object::parse_json(
				"{\"error\":\"refused\"}"
			));

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
	/* The swap was created two blocks ago.  */
	auto const created_height = std::uint32_t(98);

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
	auto output_key_odd = bool();
	{
		auto session = ProviderSession(provider_keys, client_pub);
		session.apply_xonly_tweak(roothash);
		output_key = session.get_output_key();
		output_key_odd = session.get_output_key_parity();
	}
	auto const internal_key = Secp256k1::Musig::Session::pubkey_aggregate(
		{provider_keys.pub(), client_pub}
	);
	auto const refund_leaf_hash = Secp256k1::TapTree::leaf_hash(refundScript);
	auto const claim_leaf_hash = Secp256k1::TapTree::leaf_hash(claimScript);
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

	/* Our node's blocks: one holding the lockup, one without.  */
	auto block_hex = [](std::vector<Bitcoin::Tx> txs) {
		auto block = Bitcoin::Block();
		block.txs = std::move(txs);
		auto os = std::ostringstream();
		os << block;
		auto bytes = os.str();
		return Util::Str::hexdump(bytes.data(), bytes.size());
	};
	/* Two more lockups of the same shape for the swaps whose
	 * spend tracking is tested; each swap needs its own output
	 * for the search to find spends of.  */
	auto lockup_tx2 = lockup_tx;
	lockup_tx2.inputs[0].prevTxid = Bitcoin::TxId(std::string(
		"0505050505050505050505050505050505050505050505050505050505050505"
	));
	auto lockup_tx3 = lockup_tx;
	lockup_tx3.inputs[0].prevTxid = Bitcoin::TxId(std::string(
		"0606060606060606060606060606060606060606060606060606060606060606"
	));
	auto const lockup_block_hex = block_hex({lockup_tx, lockup_tx2, lockup_tx3});
	auto const empty_block_hex = block_hex({});

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
			     , createdHeight
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
			     , :createdHeight
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
				.bind(":createdHeight", created_height)
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
		/* The provider says the lockup confirmed, but our node
		 * has only the blocks up to the one before the tip,
		 * none with the lockup.  The search starts a margin
		 * below the creation height, so those blocks exist.  */
		for (auto h = created_height - 6; h < blockheight; ++h)
			env.blocks[h] = empty_block_hex;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapA", blockheight, lockup_tx
		);
		return handler->run();
	}).then([&]() {
		/* Nothing was sent to the provider or the chain, and
		 * the search remembers where it stopped.  */
		assert(provider.path_seen == "");
		assert(!broadcast_called);
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto rows = tx.query(R"QRY(
		SELECT COALESCE(lockupSearchedHeight, 0)
		     , COALESCE(lockupConfirmedHeight, 0)
		     , lockedUp
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapA'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		auto found = false;
		for (auto& r : rows) {
			found = true;
			assert(r.get<std::uint32_t>(0) == created_height + 1);
			assert(r.get<std::uint32_t>(1) == 0);
			assert(!r.get<bool>(2));
		}
		assert(found);
		tx.commit();

		/* The block at the tip arrives with the lockup in it.
		 * The search resumes there and finds it, but one block
		 * is not deep enough to claim.  */
		env.blocks[blockheight] = lockup_block_hex;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapA", blockheight, lockup_tx
		);
		return handler->run();
	}).then([&]() {
		assert(provider.path_seen == "");
		assert(!broadcast_called);
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto rows = tx.query(R"QRY(
		SELECT COALESCE(lockupConfirmedHeight, 0), lockedUp
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapA'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		auto found = false;
		for (auto& r : rows) {
			found = true;
			assert(r.get<std::uint32_t>(0) == blockheight);
			assert(!r.get<bool>(1));
		}
		assert(found);
		tx.commit();

		/* Two blocks later the lockup is three deep, and the
		 * provider co-signs.  */
		provider.path_seen = "";
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapA", blockheight + 2, lockup_tx
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

		/* Replaceable, and valid from the block after the one
		 * it was built at.  */
		assert(broadcast_tx.inputs[0].nSequence == 0xFFFFFFFD);
		assert(broadcast_tx.nLockTime == blockheight + 2);

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
			);
		std::uint8_t sig_buf[64];
		std::copy(witness[0].begin(), witness[0].end(), sig_buf);
		auto sig = Secp256k1::SchnorrSig::from_buffer(sig_buf);
		assert(sig.valid(output_key, sighash));

		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		/* The claimed swap was stamped with the reveal, the
		 * lockup it was for, and the signed claim; the lockup's
		 * block was recorded by the search.  */
		auto stamped = tx.query(R"QRY(
		SELECT lockedUp
		     , claimTx
		     , claimFeerate
		     , COALESCE(lockupConfirmedHeight, 0)
		     , preimageRevealed
		     , COALESCE(lockupTxid, '')
		     , lockupTx
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
			assert(r.get<std::string>(1) == std::string(broadcast_tx));
			assert(r.get<std::uint32_t>(2) == 1000);
			assert(r.get<std::uint32_t>(3) == blockheight);
			assert(r.get<bool>(4));
			assert(r.get<std::string>(5) == std::string(lockup_tx.get_txid()));
			assert(r.get<std::string>(6) == std::string(lockup_tx));
		}
		assert(found);

		/* Regression: the sibling swap must be untouched.  */
		auto sibling = tx.query(R"QRY(
		SELECT lockedUp, preimageRevealed
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
			assert(!r.get<bool>(1));
		}
		assert(found);

		/* Exactly one row may be touched overall.  */
		auto count = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
		 WHERE lockedUp <> 0
		    OR preimageRevealed <> 0
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

		/* Next block the going feerate has doubled: the claim is
		 * rebuilt at it, co-signed again, and replaces the
		 * recorded one.  */
		env.feerate = 2000;
		broadcast_called = false;
		provider.path_seen = "";
		auto old_claim = broadcast_tx;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapA", blockheight + 3, Bitcoin::Tx()
		);
		return handler->run().then([&, old_claim]() {
			assert(broadcast_called);
			assert(provider.path_seen == "/v2/swap/reverse/swapA/claim");
			assert(broadcast_tx != old_claim);
			assert(broadcast_tx.inputs[0].prevTxid == lockup_tx.get_txid());
			assert(broadcast_tx.inputs[0].witness.witnesses.size() == 1);
			assert(broadcast_tx.inputs[0].nSequence == 0xFFFFFFFD);
			assert(broadcast_tx.nLockTime == blockheight + 3);
			assert( broadcast_tx.outputs[0].amount
			     < old_claim.outputs[0].amount
			      );
			return db.transact();
		});
	}).then([&](Sqlite3::Tx tx) {
		auto rows = tx.query(R"QRY(
		SELECT claimTx, claimFeerate, COALESCE(lockupClaimFees, 0)
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapA'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		auto found = false;
		for (auto& r : rows) {
			found = true;
			assert(r.get<std::string>(0) == std::string(broadcast_tx));
			assert(r.get<std::uint32_t>(1) == 2000);
			assert( r.get<std::uint64_t>(2)
			     == (amount - broadcast_tx.outputs[0].amount).to_sat()
			      );
		}
		assert(found);
		tx.commit();

		/* While the claim is unconfirmed and the feerate has not
		 * risen, each block rebroadcasts the stored transaction
		 * and asks the provider nothing.  */
		broadcast_called = false;
		provider.path_seen = "";
		auto signed_claim = broadcast_tx;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapA", blockheight + 4, Bitcoin::Tx()
		);
		return handler->run().then([&, signed_claim]() {
			assert(broadcast_called);
			assert(broadcast_tx == signed_claim);
			assert(provider.path_seen == "");
			return Ev::lift();
		});
	}).then([&]() {
		/* A confirmed payment from someone else to the destination
		 * is not the claim: the row stays and the claim is
		 * rebroadcast.  The handler asks about its own claim.  */
		env.confirmed.insert(
			"0000000000000000000000000000000000000000000000000000000000000001:0"
		);
		broadcast_called = false;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapA", blockheight + 5, Bitcoin::Tx()
		);
		return handler->run();
	}).then([&]() {
		assert(broadcast_called);
		assert( env.last_asked
		     == std::string(broadcast_tx.get_txid()) + ":0"
		      );
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto still = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapA'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		for (auto& r : still)
			assert(r.get<int>(0) == 1);
		tx.commit();

		/* Once our wallet holds the claim itself confirmed, the
		 * swap is done and its row goes; the other rows stay.  */
		env.confirmed.insert(std::string(broadcast_tx.get_txid()) + ":0");
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapA", blockheight + 6, Bitcoin::Tx()
		);
		return handler->run();
	}).then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto gone = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapA'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		for (auto& r : gone)
			assert(r.get<int>(0) == 0);
		auto rest = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub";
		)QRY").execute();
		for (auto& r : rest)
			assert(r.get<int>(0) == 2);
		tx.commit();

		/* A new swap on the same material, against a provider
		 * that keeps the preimage and refuses to co-sign.  */
		return insert_swap(api, "swapC");
	}).then([&]() {
		env.feerate = 1000;
		provider.fail = true;
		provider.path_seen = "";
		provider.preimage_seen = "";
		broadcast_called = false;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapC", blockheight + 7, lockup_tx
		);
		return handler->run();
	}).then([&]() {
		/* The provider was asked first and got the preimage;
		 * the claim went out anyway, through the script path.  */
		assert(provider.path_seen == "/v2/swap/reverse/swapC/claim");
		assert(provider.preimage_seen == std::string(real_preimage));
		assert(broadcast_called);
		assert(broadcast_tx.inputs.size() == 1);
		assert(broadcast_tx.inputs[0].prevTxid == lockup_tx.get_txid());
		assert(broadcast_tx.inputs[0].prevOut == 0);
		assert(broadcast_tx.outputs.size() == 1);
		assert(broadcast_tx.outputs[0].scriptPubKey
		    == Bitcoin::addr_to_scriptPubKey(addr));

		/* Witness: our signature, the preimage, the claim leaf,
		 * the control block.  */
		auto const& witness = broadcast_tx.inputs[0].witness.witnesses;
		assert(witness.size() == 4);
		assert(witness[0].size() == 64);
		assert(witness[1].size() == 32);
		assert(std::equal( witness[1].begin(), witness[1].end()
				 , preimage_buf
				 ));
		assert(witness[2] == claimScript);
		assert(witness[3].size() == 65);
		assert(witness[3][0] == std::uint8_t(0xc0 | (output_key_odd ? 1 : 0)));
		std::uint8_t internal_buf[32];
		internal_key.to_buffer(internal_buf);
		assert(std::equal( witness[3].begin() + 1, witness[3].begin() + 33
				 , internal_buf
				 ));
		assert(std::equal( witness[3].begin() + 33, witness[3].end()
				 , refund_leaf_hash.begin()
				 ));

		/* The signature is over the script-path sighash, which
		 * commits to the claim leaf, and verifies for our key,
		 * the one in that leaf.  */
		auto prefixed = std::vector<std::uint8_t>(1);
		prefixed[0] = std::uint8_t(lockup_scriptPubKey.size());
		prefixed.insert( prefixed.end()
			       , lockup_scriptPubKey.begin()
			       , lockup_scriptPubKey.end()
			       );
		auto extension = Bitcoin::TapscriptExtension();
		extension.leaf_hash.from_buffer(claim_leaf_hash.data());
		auto sighash = Bitcoin::p2tr_sighash
			( broadcast_tx
			, Bitcoin::SIGHASH_DEFAULT
			, 0
			, std::vector<Ln::Amount>{amount}
			, std::vector<std::vector<std::uint8_t>>{prefixed}
			, extension
			);
		std::uint8_t sig_buf[64];
		std::copy(witness[0].begin(), witness[0].end(), sig_buf);
		auto sig = Secp256k1::SchnorrSig::from_buffer(sig_buf);
		assert(sig.valid( Secp256k1::XonlyPubKey::from_ecdsa_pk(client_pub)
				, sighash
				));
		/* The script path pays for its larger witness.  */
		assert(broadcast_tx.outputs[0].amount < amount);

		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto stamped = tx.query(R"QRY(
		SELECT preimageRevealed
		     , lockedUp
		     , claimTx
		     , claimFeerate
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapC'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		auto found = false;
		for (auto& r : stamped) {
			found = true;
			assert(r.get<bool>(0));
			assert(r.get<bool>(1));
			assert(r.get<std::string>(2) == std::string(broadcast_tx));
			assert(r.get<std::uint32_t>(3) == 1000);
		}
		assert(found);
		tx.commit();

		/* Next block the going feerate is a tenth higher and the
		 * provider co-signs after all.  The key-path replacement
		 * is smaller than the script-path claim, so at a tenth
		 * more it would pay less in all and no mempool would
		 * take it; the handler raises the feerate until the
		 * replacement pays more than the claim it replaces.  */
		env.feerate = 1100;
		provider.fail = false;
		provider.path_seen = "";
		broadcast_called = false;
		auto old_claim = broadcast_tx;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapC", blockheight + 8, Bitcoin::Tx()
		);
		return handler->run().then([&, old_claim]() {
			assert(broadcast_called);
			assert(provider.path_seen == "/v2/swap/reverse/swapC/claim");
			assert(broadcast_tx.inputs[0].witness.witnesses.size() == 1);
			auto old_fee = amount - old_claim.outputs[0].amount;
			auto new_fee = amount - broadcast_tx.outputs[0].amount;
			/* More than the old fee plus a satoshi per vbyte of
			 * a key-path claim, which is over a hundred.  */
			assert(new_fee > old_fee + Ln::Amount::sat(100));
			provider.fail = true;
			return Ev::lift(broadcast_tx);
		});
	}).then([&](Bitcoin::Tx signed_claim) {
		/* The claim is mined, but our wallet has not reported it:
		 * the search of our blocks finds the spend of the lockup
		 * paying our address, and the swap is done.  */
		for (auto h = blockheight + 1; h <= blockheight + 12; ++h)
			env.blocks[h] = empty_block_hex;
		env.blocks[blockheight + 9] = block_hex({signed_claim});
		broadcast_called = false;
		provider.path_seen = "";
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapC", blockheight + 9, Bitcoin::Tx()
		);
		return handler->run();
	}).then([&]() {
		assert(!broadcast_called);
		assert(provider.path_seen == "");
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto gone = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapC'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		for (auto& r : gone)
			assert(r.get<int>(0) == 0);
		tx.commit();

		/* A swap on the second lockup, claimed through the
		 * script path, whose lockup the provider then spends
		 * another way: the loss is logged and the row goes.  */
		return insert_swap(api, "swapD");
	}).then([&]() {
		provider.path_seen = "";
		broadcast_called = false;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapD", blockheight + 10, lockup_tx2
		);
		return handler->run();
	}).then([&]() {
		assert(broadcast_called);
		assert(broadcast_tx.inputs[0].prevTxid == lockup_tx2.get_txid());
		assert(broadcast_tx.inputs[0].witness.witnesses.size() == 4);

		auto refund = Bitcoin::Tx();
		refund.inputs.resize(1);
		refund.inputs[0].prevTxid = lockup_tx2.get_txid();
		refund.inputs[0].prevOut = 0;
		refund.outputs.resize(1);
		refund.outputs[0].scriptPubKey = lockup_scriptPubKey;
		refund.outputs[0].amount = amount - Ln::Amount::sat(1000);
		env.blocks[blockheight + 11] = block_hex({refund});
		broadcast_called = false;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapD", blockheight + 11, Bitcoin::Tx()
		);
		return handler->run();
	}).then([&]() {
		assert(!broadcast_called);
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto gone = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapD'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		for (auto& r : gone)
			assert(r.get<int>(0) == 0);
		auto rest = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub";
		)QRY").execute();
		for (auto& r : rest)
			assert(r.get<int>(0) == 2);
		tx.commit();

		/* A swap on the third lockup, claimed, then nothing
		 * spends the lockup for long after the timeout: the
		 * swap is given up.  */
		return insert_swap(api, "swapE");
	}).then([&]() {
		broadcast_called = false;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapE", blockheight + 12, lockup_tx3
		);
		return handler->run();
	}).then([&]() {
		assert(broadcast_called);
		broadcast_called = false;
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, provider, random
		      , "swapE", timeout + 432, Bitcoin::Tx()
		);
		return handler->run();
	}).then([&]() {
		assert(!broadcast_called);
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto gone = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapE'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		for (auto& r : gone)
			assert(r.get<int>(0) == 0);
		tx.commit();
		return Ev::lift(0);
	});

	return Ev::start(code);
}
