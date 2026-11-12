#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boss/Mod/BoltzSwapper/ServiceCreator.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Block.hpp"
#include"Boss/Msg/Init.hpp"
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

/* A restart with `clboss-swap-providers` set, on a database with
 * swaps in flight.
 *
 * One swap is under the label of a built-in service, which the list
 * names by its clearnet root: the service must run under its
 * built-in label, and list the swap.
 * Another is under the URL of a service configured before and no
 * longer listed: that service must still run, and list its swap.
 * The newly listed service runs with no swap.
 *
 * Then the block at which both swaps time out arrives.  Neither has
 * its preimage revealed, so each service drops its swap: the one that
 * left the list is still given blocks.
 *
 * The swaps are read through `clboss-status`, the way an operator
 * sees them.  Nothing here reaches a service.
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

/* The services are created in the background: ask until all three
 * report, or give up.  */
Ev::Io<void> await_status(S::Bus& bus, std::size_t tries) {
	return yields(50).then([&bus]() {
		status.clear();
		return bus.raise(Boss::Msg::SolicitStatus{});
	}).then([]() {
		return yields(50);
	}).then([&bus, tries]() {
		if (status.size() >= 3 || tries == 0)
			return Ev::lift();
		return await_status(bus, tries - 1);
	});
}

/* A block is handled in the background too: ask until no service
 * lists a swap, or give up.  */
Ev::Io<void> await_empty(S::Bus& bus, std::size_t tries) {
	return yields(50).then([&bus]() {
		status.clear();
		return bus.raise(Boss::Msg::SolicitStatus{});
	}).then([]() {
		return yields(50);
	}).then([&bus, tries]() {
		auto swaps = std::size_t(0);
		for (auto const& e : status)
			swaps += e.second.size();
		if ((status.size() >= 3 && swaps == 0) || tries == 0)
			return Ev::lift();
		return await_empty(bus, tries - 1);
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
			assert(arr.is_array());
			for (auto i = std::size_t(0); i < arr.size(); ++i)
				ids.push_back(std::string(arr[i]["swap_id"]));
		}
		return Ev::lift();
	});

	auto code = db.transact().then([](Sqlite3::Tx tx) {
		tx.query_execute(swaps_in_flight);
		tx.commit();
		return Ev::lift();
	}).then([&]() {
		return bus.raise(Boss::Msg::Option{
			"clboss-swap-providers",
			Jsmn::Object::parse_json(
				"\"https://api.example.org,https://new.example.com\""
			),
			nullptr
		});
	}).then([&]() {
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Regtest, rpc, self_id, db,
			connector, signer, "", false
		});
	}).then([&]() {
		return await_status(bus, 200);
	}).then([&]() {
		assert(status.size() == 3);

		/* Named by its clearnet root, the built-in service
		 * keeps its label, and with it its swap.  */
		assert(status.count("https://api.example.org") == 0);
		assert(status["built-in label"]
		       == std::vector<std::string>{"swapBuiltIn"});

		/* No longer listed, and still there for its swap.  */
		assert(status["https://gone.example.net"]
		       == std::vector<std::string>{"swapGone"});

		assert(status["https://new.example.com"].empty());

		/* Both swaps time out at height 1000 with no preimage
		 * revealed, so a service that is given that block
		 * drops its swap without reaching anyone.  */
		return bus.raise(Boss::Msg::Block{1000});
	}).then([&]() {
		return await_empty(bus, 200);
	}).then([&]() {
		/* The service that left the list still gets blocks:
		 * its swap is gone, as the built-in one's is.  */
		assert(status.size() == 3);
		assert(status["built-in label"].empty());
		assert(status["https://gone.example.net"].empty());

		/* Stop the Rpc watchers so Ev::start can return.  */
		return bus.raise(Boss::Shutdown());
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
