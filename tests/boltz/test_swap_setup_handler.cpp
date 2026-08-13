#undef NDEBUG
#include"Bitcoin/hash160.hpp"
#include"Bitcoin/Tx.hpp"
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/construct_redeemscript.hpp"
#include"Boltz/Detail/SwapSetupHandler.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boltz/SwapInfo.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/Preimage.hpp"
#include"Ripemd160/Hash.hpp"
#include"Ripemd160/Hasher.hpp"
#include"Secp256k1/G.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Random.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<memory>
#include<string>

namespace {

class DummySigner : public Secp256k1::SignerIF {
public:
	Secp256k1::PubKey
	get_pubkey_tweak( Secp256k1::PrivKey const& tweak
			     ) override {
		(void) tweak;
		return Secp256k1::PubKey();
	}

	Secp256k1::Signature
	get_signature_tweak( Secp256k1::PrivKey const& tweak
			 , Sha256::Hash const& m
			 ) override {
		(void) tweak;
		(void) m;
		return Secp256k1::Signature();
	}

	Sha256::Hash
	get_privkey_salted_hash( std::uint8_t salt[32]
			       ) override {
		if (!salt)
			return Sha256::Hash();
		auto hash = Sha256::Hash();
		hash.from_buffer(salt);
		return hash;
	}
};

class MockEnv : public Boltz::EnvIF {
public:
	Ev::Io<std::uint32_t> get_feerate() override {
		return Ev::lift(std::uint32_t(1000));
	}
	Ev::Io<bool> broadcast_tx(Bitcoin::Tx) override {
		return Ev::lift(true);
	}
	Ev::Io<void> logd(std::string) override { return Ev::lift(); }
	Ev::Io<void> loge(std::string) override { return Ev::lift(); }
};

/* Builds a createswap response containing a redeemScript that matches
 * the parameters extracted from the request.  Corrupt the script or
 * adjust onchainAmount to trigger rejection paths.
 */
class MockConn : public Boltz::ConnectionIF {
public:
	enum class Mode { Valid, BadScript, LowOnchain };

private:
	Mode mode;
	std::uint32_t current_height;
	std::string their_pubkey_hex;
	std::uint32_t timeout_delta;

public:
	MockConn( Mode mode_
		, std::uint32_t current_height_
		, std::string their_pubkey_hex_
		, std::uint32_t timeout_delta_ = 100
		) : mode(mode_)
		  , current_height(current_height_)
		  , their_pubkey_hex(std::move(their_pubkey_hex_))
		  , timeout_delta(timeout_delta_)
	{ }

	Ev::Io<Jsmn::Object>
	api( std::string api
	   , std::unique_ptr<Json::Out> params
	   ) override {
		assert(api == "/createswap");

		auto timeout = current_height + timeout_delta;

		auto params_str = params->output();
		auto parser = Jsmn::Parser();
		auto parsed = parser.feed(params_str);
		auto& req = parsed.front();

		auto preimageHash_hex = std::string(req["preimageHash"]);
		auto claimPubkey_hex = std::string(req["claimPublicKey"]);

		auto ph_bytes = Util::Str::hexread(preimageHash_hex);
		auto hasher = Ripemd160::Hasher();
		hasher.feed(ph_bytes.data(), ph_bytes.size());
		auto hash160 = std::move(hasher).finalize();

		auto claim_pubkey = Secp256k1::PubKey(claimPubkey_hex);
		auto their_pubkey = Secp256k1::PubKey(their_pubkey_hex);

		auto script = Boltz::Detail::construct_redeemscript(
			hash160, claim_pubkey, timeout, their_pubkey
		);

		if (mode == Mode::BadScript) {
			script[10] ^= 0xFF;
		}

		auto script_hex = Util::Str::hexdump(
			script.data(), script.size()
		);

		auto offchain_sat = std::uint64_t(
			double(req["invoiceAmount"])
		);
		auto onchain_sat = offchain_sat;
		if (mode == Mode::LowOnchain)
			onchain_sat = offchain_sat / 2;

		auto response = std::string();
		response = Json::Out()
			.start_object()
				.field("id", std::string("test-swap-id"))
				.field("redeemScript", script_hex)
				.field("invoice", std::string("lnbc1testinvoice"))
				.field("timeoutBlockHeight", double(timeout))
				.field("onchainAmount", double(onchain_sat))
			.end_object()
			.output();
		return Ev::lift(Jsmn::Object::parse_json(response.c_str()));
	}
};

Ev::Io<void> init_db(Sqlite3::Db& db) {
	return db.transact().then([](Sqlite3::Tx tx) {
		tx.query_execute(R"QRY(
		CREATE TABLE IF NOT EXISTS "BoltzServiceFactory_rsub"
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
		)QRY");
		tx.commit();
		return Ev::lift();
	});
}

void run_test( MockConn::Mode mode
	     , std::uint32_t current_height
	     , std::uint32_t timeout_delta = 100
	     ) {
	auto random = Secp256k1::Random();
	auto signer = DummySigner();
	auto env = MockEnv();
	auto their_pubkey_hex = std::string(
		"0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798"
	);
	auto conn = MockConn(mode, current_height, their_pubkey_hex, timeout_delta);
	auto db = Sqlite3::Db(":memory:");

	auto offchain = Ln::Amount::sat(100000);
	auto handler = Boltz::Detail::SwapSetupHandler::create(
		signer, db, env, "http://test.boltz",
		conn, random,
		std::string("bc1qtestdestination"),
		offchain, current_height
	);

	auto got = std::make_shared<Boltz::SwapInfo>();
	auto code = init_db(db).then([&handler, got]() {
		return handler->run();
	}).then([got](Boltz::SwapInfo info) {
		*got = std::move(info);
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);

	if (mode == MockConn::Mode::Valid) {
		assert(!got->invoice.empty());
		assert(got->timeout == current_height + timeout_delta);
	} else {
		assert(got->invoice.empty());
		assert(got->timeout == 0);
	}
}

} // namespace

int main() {
	auto const height = std::uint32_t(800000);

	run_test(MockConn::Mode::Valid, height);
	run_test(MockConn::Mode::BadScript, height);
	run_test(MockConn::Mode::LowOnchain, height);

	return 0;
}
