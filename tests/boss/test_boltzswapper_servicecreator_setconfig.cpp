#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boss/Mod/BoltzSwapper/ServiceCreator.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/Network.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"Net/Connector.hpp"
#include"Net/Fd.hpp"
#include"Net/SocketFd.hpp"
#include"S/Bus.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include<array>
#include<assert.h>
#include<cstdint>
#include<map>
#include<memory>
#include<sstream>
#include<string>
#include<sys/socket.h>
#include<vector>

/* `clboss-swap-providers` changed with setconfig, after Msg::Init.
 *
 * A service newly listed is run and takes the new swaps; one that
 * leaves the list keeps running, for its swaps in flight alone; an
 * empty list gives the built-in services back; a list that does not
 * parse is refused and changes nothing; a built-in service listed
 * by another endpoint stays on the one it was started with.
 *
 * Which services take new swaps is read from the log line each
 * change writes, the services that run from `clboss-status`.
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
	Ev::Io<void> logi(std::string) override { return Ev::lift(); }
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

class DummyConnector : public Net::Connector {
public:
	Net::SocketFd connect(std::string const&, int) override {
		return Net::SocketFd();
	}
};

/* The swap table with two swaps in flight.  */
char const swaps_in_flight[] = R"QRY(
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
INSERT INTO "BoltzServiceFactory_rsub"
     ( apiAccess, tweak, preimage, destinationAddress, swapId
     , redeemScript, timeoutBlockheight, onchainAmount, lockedUp
     , comment
     )
VALUES
     ( 'built-in label', '02', '03', 'bc1qbuiltin', 'swapBuiltIn'
     , '', 1000, 100000, 0
     , ''
     )
   , ( 'https://gone.example.net', '02', '03', 'bc1qgone', 'swapGone'
     , '', 1000, 200000, 0
     , ''
     );
)QRY";

typedef Boss::Mod::BoltzSwapper::Instance Instance;
auto const instances = std::map<Boss::Msg::Network, std::vector<Instance>>
{ { Boss::Msg::Network_Regtest
  , { { "built-in label"
      , "https://api.example.org"
      , "http://exampleorg.onion/api"
      }
    }
  }
};

/* label -> swap ids, from the last `boltz_swaps` status.  */
std::map<std::string, std::vector<std::string>> status;

Ev::Io<void> yields(std::size_t n) {
	auto act = Ev::yield();
	for (auto i = std::size_t(0); i < n; ++i)
		act = std::move(act).then([]() { return Ev::yield(); });
	return act;
}

/* The services are created in the background: ask until `want` of
 * them report, or give up.  */
Ev::Io<void> await_status(S::Bus& bus, std::size_t tries, std::size_t want) {
	return yields(50).then([&bus]() {
		status.clear();
		return bus.raise(Boss::Msg::SolicitStatus{});
	}).then([]() {
		return yields(50);
	}).then([&bus, tries, want]() {
		if (status.size() >= want || tries == 0)
			return Ev::lift();
		return await_status(bus, tries - 1, want);
	});
}


}

int main() {
	auto bus = S::Bus();
	auto threadpool = Ev::ThreadPool();
	auto env = MockEnv();
	auto signer = MockSigner();
	auto connector = DummyConnector();
	auto db = Sqlite3::Db(":memory:");
	auto sockets = std::array<int, 2>();
	auto res = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data());
	assert(res >= 0);
	auto server_end = Net::Fd(sockets[0]);
	auto rpc = Boss::Mod::Rpc(bus, Net::Fd(sockets[1]));
	auto self_id = Ln::NodeId(std::string(
		"020000000000000000000000000000000000000000000000000000000000000000"
	));

	auto creator = Boss::Mod::BoltzSwapper::ServiceCreator
		(bus, threadpool, env, instances);

	bus.subscribe<Boss::Msg::ProvideStatus
		     >([](Boss::Msg::ProvideStatus const& m) {
		if (m.key != "boltz_swaps")
			return Ev::lift();
		auto is = std::istringstream(m.value.output());
		auto js = Jsmn::Object();
		is >> js;
		assert(js.is_object());
		for (auto const& label : js.keys()) {
			auto& ids = status[label];
			auto arr = js[label];
			for (auto i = std::size_t(0); i < arr.size(); ++i)
				ids.push_back(std::string(arr[i]["swap_id"]));
		}
		return Ev::lift();
	});
	auto logs = std::vector<std::string>();
	bus.subscribe<Boss::Msg::JsonCout
		     >([&](Boss::Msg::JsonCout const& m) {
		auto is = std::istringstream(m.obj.output());
		auto js = Jsmn::Object();
		is >> js;
		if ( js.is_object() && js.has("method")
		  && std::string(js["method"]) == "log"
		   )
			logs.push_back(std::string(js["params"]["message"]));
		return Ev::lift();
	});
	auto logged = [&](char const* needle) {
		for (auto const& l : logs)
			if (l.find(needle) != std::string::npos)
				return true;
		return false;
	};
	/* setconfig; gives the reason if refused.  */
	auto setconfig = [&](char const* list) {
		logs.clear();
		auto reason = std::make_shared<std::string>();
		auto json = "\"" + std::string(list) + "\"";
		return bus.raise(Boss::Msg::Option{
			"clboss-swap-providers",
			Jsmn::Object::parse_json(json.c_str()),
			reason
		}).then([reason]() {
			return yields(200);
		}).then([reason]() {
			return Ev::lift(*reason);
		});
	};

	auto code = db.transact().then([](Sqlite3::Tx tx) {
		tx.query_execute(swaps_in_flight);
		tx.commit();
		return Ev::lift();
	}).then([&]() {
		/* Nothing listed at startup: the built-in service, and
		 * the one the database has a swap in flight with.  */
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Regtest, rpc, self_id, db,
			connector, signer, "", false
		});
	}).then([&]() {
		return await_status(bus, 200, 2);
	}).then([&]() {
		assert(status.size() == 2);
		assert(status.count("built-in label") == 1);
		assert(status.count("https://gone.example.net") == 1);

		return setconfig("https://new.example.com");
	}).then([&](std::string reason) {
		assert(reason.empty());
		assert(logged("new swaps go to: https://new.example.com; "
			      "for swaps in flight only: built-in label, "
			      "https://gone.example.net."));
		return await_status(bus, 200, 3);
	}).then([&]() {
		/* The new one runs; none was stopped.  */
		assert(status.size() == 3);
		assert(status["https://new.example.com"].empty());
		assert(status["built-in label"]
		       == std::vector<std::string>{"swapBuiltIn"});

		/* Not a list: refused, nothing changes.  */
		return setconfig("new.example.com");
	}).then([&](std::string reason) {
		assert(!reason.empty());
		assert(logged("keeping the list in use"));
		assert(!logged("new swaps go to"));

		/* Emptied: the built-in service again.  */
		return setconfig("");
	}).then([&](std::string reason) {
		assert(reason.empty());
		assert(logged("new swaps go to: built-in label; "
			      "for swaps in flight only: "
			      "https://gone.example.net, "
			      "https://new.example.com."));

		/* Listed by its onion root: still the built-in
		 * service, on the endpoints it was started with.  */
		return setconfig("http://exampleorg.onion/api");
	}).then([&](std::string reason) {
		assert(reason.empty());
		assert(logged("\"built-in label\" stays on the endpoint"));
		assert(logged("new swaps go to: built-in label; "));
		return await_status(bus, 200, 3);
	}).then([&]() {
		assert(status.size() == 3);
		/* Stop the Rpc watchers so Ev::start can return.  */
		return bus.raise(Boss::Shutdown());
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
