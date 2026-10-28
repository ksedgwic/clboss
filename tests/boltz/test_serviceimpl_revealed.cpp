#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/ServiceImpl.hpp"
#include"Boltz/EnvIF.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<cstdint>
#include<iostream>
#include<map>
#include<memory>
#include<string>
#include<vector>

/* Drives Boltz::Detail::ServiceImpl::on_block over three swaps
 * and checks what the provider's status may and may not do:
 *
 * - a swap whose preimage is revealed is never asked about; it is
 *   tracked from our own node, and once our wallet holds the claim
 *   confirmed its row goes;
 * - a swap not yet revealed whose status says the invoice settled
 *   stays: only our preimage settles the invoice and we never sent
 *   it, so there is nothing to finish;
 * - a swap not yet revealed whose status says it expired goes, as
 *   before.
 */

namespace {

class MockEnv : public Boltz::EnvIF {
public:
	/* The output our wallet holds confirmed, as "txid:outnum";
	 * empty for none.  */
	std::string confirmed;
	Ev::Io<std::uint32_t> get_feerate() override {
		return Ev::lift(std::uint32_t(1000));
	}
	Ev::Io<std::string> get_block(std::uint32_t) override {
		return Ev::lift(std::string());
	}
	Ev::Io<bool> has_confirmed_output( Bitcoin::TxId txid
					 , std::uint32_t outnum
					 ) override {
		auto key = std::string(txid) + ":" + std::to_string(outnum);
		return Ev::lift(key == confirmed);
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

/* Mock provider: answers each swap's status from `statuses` and
 * records every path asked.  */
class MockConnection : public Boltz::ConnectionIF {
public:
	std::map<std::string, std::string> statuses;
	std::vector<std::string> paths_seen;

	Ev::Io<Jsmn::Object>
	api( std::string path
	   , std::unique_ptr<Json::Out> params
	   ) override {
		paths_seen.push_back(path);
		assert(!params);
		auto const prefix = std::string("/v2/swap/");
		assert(path.compare(0, prefix.size(), prefix) == 0);
		auto id = path.substr(prefix.size());
		auto it = statuses.find(id);
		assert(it != statuses.end());
		auto res = std::string("{\"status\":\"") + it->second + "\"}";
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

int count_swaps(Sqlite3::Tx& tx, std::string const& swapId) {
	auto rv = -1;
	auto fetch = tx.query(R"QRY(
	SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
	 WHERE apiAccess = 'testapi'
	   AND swapId = :swapId
	     ;
	)QRY")
		.bind(":swapId", swapId)
		.execute();
	for (auto& r : fetch)
		rv = r.get<int>(0);
	return rv;
}

}

int main() {
	auto db = Sqlite3::Db(":memory:");
	auto signer = MockSigner();
	auto env = MockEnv();
	auto conn = Util::make_unique<MockConnection>();
	auto& provider = *conn;
	provider.statuses["settled"] = "invoice.settled";
	provider.statuses["expired"] = "swap.expired";
	/* Asking about the revealed swap would fail the test: the
	 * mock has no status for it.  */

	auto service = Boltz::Detail::ServiceImpl( db, signer, env
						  , "testapi", std::move(conn)
						  );

	/* A valid key for the refund side; the leaves are only
	 * required to be present.  */
	auto const refund_key = std::string(Secp256k1::PubKey(Secp256k1::PrivKey(
		std::string(
		"0505050505050505050505050505050505050505050505050505050505050505"
		)
	)));
	/* The revealed swap already has a signed claim, so tracking it
	 * only rebroadcasts; the claim itself is the claim handler
	 * test's business.  */
	auto signed_claim = Bitcoin::Tx();
	signed_claim.inputs.resize(1);
	signed_claim.outputs.resize(1);
	/* Another signed claim, for a revealed swap already past its
	 * timeout.  */
	auto late_claim = signed_claim;
	late_claim.nLockTime = 1;
	auto insert = [&]( Sqlite3::Tx& tx, std::string swapId, bool revealed
			 , std::uint32_t timeout, Bitcoin::Tx const& claim
			 ) {
		tx.query(R"QRY(
		INSERT INTO "BoltzServiceFactory_rsub"
		     ( apiAccess, tweak, preimage, destinationAddress, swapId
		     , claimScript, refundScript, refundPubkey
		     , timeoutBlockheight, onchainAmount, createdHeight
		     , lockedUp, preimageRevealed, claimTx, claimFeerate, comment
		     )
		VALUES
		     ( 'testapi'
		     , '0202020202020202020202020202020202020202020202020202020202020202'
		     , '0303030303030303030303030303030303030303030303030303030303030303'
		     , 'bc1qg430dgu75qvphu8nn3kvcp3yg2km2tavpc8lqs', :swapId
		     , '82', '20', :refundPubkey
		     , :timeout, 100000, 90
		     , :revealed, :revealed, :claimTx, :claimFeerate, ''
		     );
		)QRY")
			.bind(":swapId", swapId)
			.bind(":refundPubkey", refund_key)
			.bind(":timeout", timeout)
			.bind(":revealed", revealed ? 1 : 0)
			.bind(":claimTx", revealed ? std::string(claim) : std::string())
			.bind(":claimFeerate", revealed ? 1000 : 0)
			.execute();
	};

	auto code = Ev::lift().then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query_execute(schema);
		insert(tx, "revealed", true, 1000, signed_claim);
		insert(tx, "settled", false, 1000, signed_claim);
		insert(tx, "expired", false, 1000, signed_claim);
		/* Two swaps past their timeout: one with the preimage
		 * out, which stays and is tracked, one without, which
		 * goes without a question to the provider.  */
		insert(tx, "revealed_late", true, 50, late_claim);
		insert(tx, "late", false, 50, signed_claim);
		tx.commit();

		/* First block: our wallet does not hold the claim yet.  */
		return service.on_block(100);
	}).then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		/* The revealed swap was tracked, not asked about, and
		 * stays; the settled one stays; the expired one went.  */
		assert(count_swaps(tx, "revealed") == 1);
		assert(count_swaps(tx, "settled") == 1);
		assert(count_swaps(tx, "expired") == 0);
		assert(count_swaps(tx, "revealed_late") == 1);
		assert(count_swaps(tx, "late") == 0);
		for (auto const& p : provider.paths_seen) {
			assert(p != "/v2/swap/revealed");
			assert(p != "/v2/swap/revealed_late");
			assert(p != "/v2/swap/late");
		}
		assert(provider.paths_seen.size() == 2);
		tx.commit();

		/* Next block our wallet holds the claim confirmed.  */
		env.confirmed = std::string(signed_claim.get_txid()) + ":0";
		return service.on_block(101);
	}).then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		assert(count_swaps(tx, "revealed") == 0);
		assert(count_swaps(tx, "settled") == 1);
		/* The late one has another claim, still unconfirmed.  */
		assert(count_swaps(tx, "revealed_late") == 1);
		for (auto const& p : provider.paths_seen)
			assert(p != "/v2/swap/revealed");
		tx.commit();

		/* Long after its timeout, with the lockup still unspent
		 * as far as our blocks show, the late swap is given up.  */
		return service.on_block(50 + 432);
	}).then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		assert(count_swaps(tx, "revealed_late") == 0);
		assert(count_swaps(tx, "settled") == 1);
		for (auto const& p : provider.paths_seen)
			assert(p != "/v2/swap/revealed_late");
		tx.commit();
		return Ev::lift(0);
	});

	return Ev::start(code);
}
