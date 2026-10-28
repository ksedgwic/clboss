#include"Bitcoin/Tx.hpp"
#include"Boltz/Connection.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boltz/Service.hpp"
#include"Boltz/ServiceFactory.hpp"
#include"Boltz/SwapInfo.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/runcmd.hpp"
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
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include<algorithm>
#include<cctype>
#include<cstdlib>
#include<basicsecure.h>
#include<iomanip>
#include<iostream>
#include<iterator>
#include<sstream>
#include<string>
#include<vector>

namespace {

std::string trim(std::string s) {
	while (!s.empty() && std::isspace((unsigned char) s.back()))
		s.pop_back();
	auto i = std::size_t(0);
	while (i < s.size() && std::isspace((unsigned char) s[i]))
		++i;
	return s.substr(i);
}

/* BITCOIN_CLI split on spaces: the command and its arguments.  */
std::vector<std::string> bitcoin_cli_command() {
	auto env = std::getenv("BITCOIN_CLI");
	auto is = std::istringstream(std::string(env ? env : "bitcoin-cli"));
	auto rv = std::vector<std::string>();
	auto word = std::string();
	while (is >> word)
		rv.push_back(word);
	if (rv.empty())
		rv.push_back("bitcoin-cli");
	return rv;
}

/* Fees and broadcasts go to the provider's chain endpoints, and
 * blocks come from bitcoin-cli; in CLBOSS itself all three go to
 * lightningd.  */
class Env : public Boltz::EnvIF {
private:
	Boltz::Connection conn;

public:
	Env( Ev::ThreadPool& threadpool
	   , std::string const& base
	   , std::string const& proxy
	   ) : conn(threadpool, base, proxy) { }

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
		return conn.api("/v2/chain/fees", nullptr
			       ).then([](Jsmn::Object res) {
			/* sat/vbyte to sat/kw.  */
			auto pervb = (double) res["BTC"];
			auto perkw = std::uint32_t(pervb * 1000 / 4);
			return Ev::lift(perkw);
		});
	}

	Ev::Io<std::string> get_block(std::uint32_t height) override {
		auto cli = bitcoin_cli_command();
		auto command = cli.front();
		auto args = std::vector<std::string>(cli.begin() + 1, cli.end());
		auto hash_args = args;
		hash_args.push_back("getblockhash");
		hash_args.push_back(std::to_string(height));
		return Ev::runcmd(command, hash_args
				 ).then([this, command, args, height](std::string hash) {
			hash = trim(hash);
			if (hash.size() != 64 || !Util::Str::ishex(hash)) {
				return logd( std::string("get_block: no block hash for height ")
					   + std::to_string(height)
					   + " from " + command
					   + " (set BITCOIN_CLI)."
					   ).then([]() {
					return Ev::lift(std::string());
				});
			}
			auto block_args = args;
			block_args.push_back("getblock");
			block_args.push_back(hash);
			block_args.push_back("0");
			return Ev::runcmd(command, block_args
					 ).then([](std::string block) {
				block = trim(block);
				if (block.empty() || !Util::Str::ishex(block))
					return Ev::lift(std::string());
				return Ev::lift(block);
			});
		});
	}

	/* gettxout answers from the UTXO set: a spent or unknown
	 * output prints nothing.  A claim spent again during a run
	 * would read as unconfirmed; this tool's destination is
	 * not spent during a run.  */
	Ev::Io<bool> has_confirmed_output( Bitcoin::TxId txid
					 , std::uint32_t outnum
					 ) override {
		auto cli = bitcoin_cli_command();
		auto command = cli.front();
		auto args = std::vector<std::string>(cli.begin() + 1, cli.end());
		args.push_back("gettxout");
		args.push_back(std::string(txid));
		args.push_back(std::to_string(outnum));
		return Ev::runcmd(command, args).then([](std::string out) {
			auto found = false;
			try {
				auto res = Jsmn::Object::parse_json(out.c_str());
				if ( res.has("confirmations")
				  && res["confirmations"].is_number()
				   )
					found = double(res["confirmations"]) >= 1;
			} catch (std::exception const&) {
				/* No node, or no such output: not found.  */
			}
			return Ev::lift(found);
		});
	}

	Ev::Io<bool> broadcast_tx(Bitcoin::Tx tx) override {
		auto str = std::string(tx);
		auto params = Json::Out()
			.start_object()
				.field("hex", str)
			.end_object()
			;
		return conn.api( "/v2/chain/BTC/transaction"
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
			if (res.has("id")) {
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

/* Drives one reverse swap against a provider without CLBOSS or
 * lightningd: `swap` creates it and prints the invoice to pay by
 * other means, `on-block` polls it and claims once the lockup has
 * confirmed.  State is in data.dev-boltz in the current directory.
 *
 * Environment overrides, the first two as for dev-boltz-api:
 *   BOLTZ_API_BASE  API root (default: https://api.boltz.exchange)
 *   BOLTZ_PROXY     SOCKS5 proxy (e.g. socks5h://127.0.0.1:9050)
 *   BITCOIN_CLI     command that fetches blocks, with any arguments
 *                   (default: bitcoin-cli; e.g. "bitcoin-cli -signet")
 */
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

	auto base = std::string("https://api.boltz.exchange");
	if (auto e = std::getenv("BOLTZ_API_BASE")) base = e;
	auto proxy = std::string("");
	if (auto e = std::getenv("BOLTZ_PROXY")) proxy = e;

	Ev::ThreadPool threadpool;
	auto db = Sqlite3::Db("data.dev-boltz");
	auto signer = Signer();
	auto env = Env(threadpool, base, proxy);
	auto factory = Boltz::ServiceFactory
		( threadpool
		, db
		, signer
		, env
		, proxy
		, !proxy.empty()
		);

	auto service = std::shared_ptr<Boltz::Service>();

	auto code = Ev::lift().then([&]() {

		/* Create service.  */
		return factory.create_service(base);
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
