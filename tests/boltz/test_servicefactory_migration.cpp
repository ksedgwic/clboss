#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boltz/Service.hpp"
#include"Boltz/ServiceFactory.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/start.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include<assert.h>
#include<cstdint>
#include<memory>
#include<set>
#include<string>

/* A database from the API v1 code, with a swap in flight, is
 * opened by Boltz::ServiceFactory.  The table must gain the v2
 * columns in place and keep the v1 column, the row must survive
 * with empty leaves, a v2 row and a v1-style row must both insert
 * afterwards (the latter is what an earlier release does on the
 * upgraded database), and a second initialization must change
 * nothing.
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
	Ev::Io<bool> broadcast_tx(Bitcoin::Tx) override {
		return Ev::lift(true);
	}
	Ev::Io<void> logd(std::string) override { return Ev::lift(); }
	Ev::Io<void> loge(std::string) override { return Ev::lift(); }
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

/* The table as the API v1 code created it, with its index and a
 * swap in flight.  */
char const v1_schema[] = R"QRY(
CREATE TABLE "BoltzServiceFactory_rsub"
     ( id INTEGER PRIMARY KEY
     , apiAccess TEXT NOT NULL
     , tweak TEXT NOT NULL
     , preimage TEXT NOT NULL
     , destinationAddress TEXT NOT NULL
     , swapId TEXT NOT NULL
     , redeemScript TEXT NOT NULL
     , timeoutBlockheight INTEGER NOT NULL
     , onchainAmount INTEGER NOT NULL
     , lockedUp INTEGER NOT NULL
     , lockupTxid TEXT NULL
     , lockupOut INTEGER NULL
     , lockupConfirmedHeight INTEGER NULL
     , lockupClaimFees INTEGER NULL
     , comment TEXT
     );
CREATE INDEX "BoltzServiceFactory_rsub_swapid"
    ON "BoltzServiceFactory_rsub"
     ( swapId
     , apiAccess
     );
INSERT INTO "BoltzServiceFactory_rsub"
     ( apiAccess, tweak, preimage, destinationAddress, swapId
     , redeemScript, timeoutBlockheight, onchainAmount, lockedUp
     , comment
     )
VALUES
     ( 'testapi', '02', '03', 'bc1qtest', 'swapV1'
     , '51', 1000, 100000, 0
     , ''
     );
)QRY";

/* What the v1 code inserts: no v2 columns named.  */
char const v1_insert[] = R"QRY(
INSERT INTO "BoltzServiceFactory_rsub"
     ( apiAccess, tweak, preimage, destinationAddress, swapId
     , redeemScript, timeoutBlockheight, onchainAmount, lockedUp
     , comment
     )
VALUES
     ( 'testapi', '02', '03', 'bc1qtest', 'swapV1b'
     , '51', 1200, 300000, 0
     , ''
     );
)QRY";

/* What the v2 code inserts: an empty redeemScript, because the
 * v1 column has no default in a database from before v2.  */
char const v2_insert[] = R"QRY(
INSERT INTO "BoltzServiceFactory_rsub"
     ( apiAccess, tweak, preimage, destinationAddress, swapId
     , redeemScript, claimScript, refundScript, refundPubkey
     , timeoutBlockheight, onchainAmount, lockedUp, comment
     )
VALUES
     ( 'testapi', '02', '03', 'bc1qtest', 'swapV2'
     , '', '82', '20', '02'
     , 1100, 200000, 0, ''
     );
)QRY";

std::set<std::string> columns(Sqlite3::Tx& tx) {
	auto cols = std::set<std::string>();
	auto fetch = tx.query(R"QRY(
	SELECT name FROM pragma_table_info('BoltzServiceFactory_rsub');
	)QRY").execute();
	for (auto& r : fetch)
		cols.insert(r.get<std::string>(0));
	return cols;
}

int count_rows(Sqlite3::Tx& tx) {
	auto rv = -1;
	auto fetch = tx.query(R"QRY(
	SELECT COUNT(*) FROM "BoltzServiceFactory_rsub";
	)QRY").execute();
	for (auto& r : fetch)
		rv = r.get<int>(0);
	return rv;
}

}

int main() {
	Ev::ThreadPool threadpool;
	auto db = Sqlite3::Db(":memory:");
	auto signer = MockSigner();
	auto env = MockEnv();
	auto factory = Boltz::ServiceFactory( threadpool, db, signer, env
					    , "", false
					    );

	auto code = Ev::lift().then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query_execute(v1_schema);
		tx.commit();
		/* Creating a service initializes the table.  */
		return factory.create_service("testapi");
	}).then([&](std::unique_ptr<Boltz::Service>) {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		/* The v2 columns are added and the v1 column stays.  */
		auto cols = columns(tx);
		assert(cols.count("claimScript"));
		assert(cols.count("refundScript"));
		assert(cols.count("refundPubkey"));
		assert(cols.count("createdHeight"));
		assert(cols.count("lockupSearchedHeight"));
		assert(cols.count("redeemScript"));

		/* The swap in flight is still there, with empty leaves.  */
		auto rows = tx.query(R"QRY(
		SELECT swapId, redeemScript, claimScript, refundScript
		     , refundPubkey, timeoutBlockheight, onchainAmount
		  FROM "BoltzServiceFactory_rsub";
		)QRY").execute();
		auto found = 0;
		for (auto& r : rows) {
			++found;
			assert(r.get<std::string>(0) == "swapV1");
			assert(r.get<std::string>(1) == "51");
			assert(r.get<std::string>(2) == "");
			assert(r.get<std::string>(3) == "");
			assert(r.get<std::string>(4) == "");
			assert(r.get<std::uint32_t>(5) == 1000);
			assert(r.get<std::uint64_t>(6) == 100000);
		}
		assert(found == 1);

		/* The index is still on the table.  */
		auto index = tx.query(R"QRY(
		SELECT tbl_name FROM sqlite_master
		 WHERE type = 'index'
		   AND name = 'BoltzServiceFactory_rsub_swapid';
		)QRY").execute();
		found = 0;
		for (auto& r : index) {
			++found;
			assert(r.get<std::string>(0) == "BoltzServiceFactory_rsub");
		}
		assert(found == 1);

		/* Both generations of the code can insert.  */
		tx.query_execute(v2_insert);
		tx.query_execute(v1_insert);
		assert(count_rows(tx) == 3);
		tx.commit();
		return Ev::lift();
	}).then([&]() {
		/* A second factory on the same database finds the
		 * columns and changes nothing.  */
		auto again = std::make_shared<Boltz::ServiceFactory>(
			threadpool, db, signer, env, "", false
		);
		return again->create_service("testapi"
		).then([again](std::unique_ptr<Boltz::Service>) {
			return Ev::lift();
		});
	}).then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		assert(count_rows(tx) == 3);
		assert(columns(tx).size() == 20);
		tx.commit();
		return Ev::lift(0);
	});

	return Ev::start(code);
}
