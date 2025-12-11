#include"Bitcoin/Tx.hpp"
#include"Boltz/Connection.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boltz/Service.hpp"
#include"Boltz/ServiceFactory.hpp"
#include"Boltz/SwapInfo.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/Hasher.hpp"
#include"Sqlite3/Db.hpp"
#include"Util/make_unique.hpp"
#include<algorithm>
#include<basicsecure.h>
#include<iomanip>
#include<iostream>
#include<iterator>
#include<sstream>
#include<string>
#include<vector>

namespace {

class Env : public Boltz::EnvIF {
private:
	Boltz::Connection conn;

public:
	explicit
	Env(Ev::ThreadPool& threadpool) : conn(threadpool) { }

	Ev::Io<void> logd(std::string msg) override {
		return Ev::lift().then([msg]() {
			std::cout << "DEBUG  " << msg << std::endl;
			return Ev::lift();
		});
	}
	Ev::Io<void> loge(std::string msg) override {
		return Ev::lift().then([msg]() {
			std::cout << "ERRROR " << msg << std::endl;
			return Ev::lift();
		});
	}

	Ev::Io<std::uint32_t> get_feerate() override {
		return conn.api("/getfeeestimation", nullptr
			       ).then([](Jsmn::Object res) {
			auto perb = (double) res["BTC"];
			auto perkw = std::uint32_t(perb * 1000 / 4);
			return Ev::lift(perkw);
		});
	}

	Ev::Io<bool> broadcast_tx(Bitcoin::Tx tx) override {
		auto str = std::string(tx);
		auto params = Json::Out()
			.start_object()
				.field("currency", "BTC")
				.field("transactionHex", str)
			.end_object()
			;
		return conn.api( "/broadcasttransaction"
			       , Util::make_unique<Json::Out>(
					std::move(params)
				 )
			       ).then([this](Jsmn::Object res) {
			if (res.has("error")) {
				return loge( std::string("broadcast_tx: ")
					   + std::string(res["error"])
					   ).then([]() {
					return Ev::lift(false);
				});
			}
			if (res.has("transactionId")) {
				return Ev::lift(true);
			}
			return Ev::lift(false);
		});
	}
};

class Signer : public Secp256k1::SignerIF {
private:
	Secp256k1::PrivKey sk;

public:
	/* Yeah, everyone totally knows the below private key, so insecure.  */
	Signer() : sk("54e28e74151d6b0f30453288d6842f240820dc7553afb424d4f68ae57eef28fa") { }

	Secp256k1::PubKey
	get_pubkey_tweak(Secp256k1::PrivKey const& tweak) override {
		return Secp256k1::PubKey(tweak * sk);
	}
	Secp256k1::Signature
	get_signature_tweak( Secp256k1::PrivKey const& tweak
			   , Sha256::Hash const& m
			   ) override {
		return Secp256k1::Signature::create
			( tweak * sk
			, m
			);
	}
	Sha256::Hash
	get_privkey_salted_hash(std::uint8_t salt[32]) override {
		auto hasher = Sha256::Hasher();
		hasher.feed(salt, 32);
		std::uint8_t buf[32];
		sk.to_buffer(buf);
		hasher.feed(buf, 32);
		basicsecure_clear(buf, sizeof(buf));
		return std::move(hasher).finalize();
	}
	Secp256k1::KeyPair
	get_keypair_tweak(Secp256k1::PrivKey const& tweak
			) override {
		return Secp256k1::KeyPair(tweak * sk);
	}
};

std::uint64_t to_number(std::string const& s) {
	auto is = std::istringstream(s);
	auto rv = std::uint64_t();
	is >> std::dec >> rv;
	return rv;
}

}

int main(int argc, char** c_argv) {
	auto argv = std::vector<std::string>();
	std::copy( c_argv, c_argv + argc
		 , std::back_inserter(argv)
		 );
	if (argv.size() < 3) {
		std::cout << "Need argument:" << std::endl
			  << "  get-quotation $satoshis" << std::endl
			  << "  on-block $currentblockheight" << std::endl
			  << "  swap $satoshis $addr $currentblockheight" << std::endl
			   ;
		return 0;
	}

	Ev::ThreadPool threadpool;
	auto db = Sqlite3::Db("data.dev-boltz");
	auto signer = Signer();
	auto env = Env(threadpool);
	auto factory = Boltz::ServiceFactory
		( threadpool
		, db
		, signer
		, env
		);

	auto service = std::shared_ptr<Boltz::Service>();

	auto code = Ev::lift().then([&]() {

		/* Create service.  */
		return factory.create_service("https://boltz.exchange/api");
	}).then([&](std::unique_ptr<Boltz::Service> n_service) {
		service = std::move(n_service);

		if (argv[1] == "get-quotation") {
			auto num = to_number(argv[2]);
			auto value = Ln::Amount::sat(num);
			return service->get_quotation(value
			).then([](std::unique_ptr<Ln::Amount> fee) {
				if (fee)
					std::cout << fee->to_sat()
						  << std::endl
						   ;
				return Ev::lift(0);
			});
		} else if (argv[1] == "on-block") {
			auto num = to_number(argv[2]);
			return service->on_block(num).then([]() {
				return Ev::lift(0);
			});
		} else if (argv[1] == "swap" && argv.size() >= 5) {
			auto num = to_number(argv[2]);
			auto value = Ln::Amount::sat(num);
			auto addr = argv[3];
			auto blockheight = std::uint32_t(to_number(argv[4]));
			return service->swap(value, addr, blockheight
			).then([](Boltz::SwapInfo result) {
				auto invoice = result.invoice;
				auto hash = result.hash;
				auto timeout = result.timeout;
				std::cout << invoice << std::endl;
				std::cout << hash << std::endl;
				std::cout << timeout << std::endl;
				return Ev::lift(0);
			});
		}

		std::cerr << "argument error" << std::endl;
		return Ev::lift(1);
	});
	return Ev::start(code);
}
