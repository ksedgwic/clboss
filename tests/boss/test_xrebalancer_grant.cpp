#undef NDEBUG

#include"Boss/Mod/RebalanceUnmanager.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Mod/XRebalancer.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/DemandObserved.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/RequestRebalanceMode.hpp"
#include"Boss/Msg/RequestRpcCommand.hpp"
#include"Boss/Msg/ResponseRebalanceMode.hpp"
#include"Boss/Msg/ResponseRpcCommand.hpp"
#include"Boss/RebalanceMode.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include"Net/Connector.hpp"
#include"Net/Fd.hpp"
#include"Net/SocketFd.hpp"
#include"S/Bus.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include<assert.h>
#include<ctime>
#include<iostream>
#include<sstream>
#include<string>
#include<sys/socket.h>

/* Checks the weight of clboss-xrebalance-grant: the credit is one
 * rebalance's worth of volume per side -- fill-loc percent of the
 * peer's capacity on the out side, 100 - drain-loc percent on the
 * in side -- not a whole capacity-turn.  Both peers have a
 * 1_000_000_000 msat channel and 1_000_000_000 msat forwarded on
 * their candidate side.  With grant 100, the default fill-loc 25,
 * and drain-loc 70: A's out side weighs 250_000_000 msat,
 * (1_000_000 + 25_000) / 1.25e9 = 820 ppm; B's in side weighs
 * 300_000_000 msat, (500_000 + 30_000) / 1.3e9 = 407.7 ppm; the
 * demand cycle prices at 1228 ppm.  A whole capacity-turn would
 * give 550 + 300 = 850; fill-loc on both sides, 820 + 420 = 1240.  */

namespace {

/* Peer A: 5% local -> fill candidate; the demand target.  */
auto const node_a = "020000000000000000000000000000000000000000000000000000000000000000";
/* Peer B: 95% local -> the only drain candidate.  */
auto const node_b = "020000000000000000000000000000000000000000000000000000000000000001";

auto const scid_a = "103x1x0";
auto const scid_b = "103x1x1";

auto const listpeerchannels_result = R"JSON(
{
  "channels": [
    {
      "state": "CHANNELD_NORMAL",
      "to_us_msat": "50000000msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "103x1x0",
      "peer_id": "020000000000000000000000000000000000000000000000000000000000000000",
      "peer_connected": true
    },
    {
      "state": "CHANNELD_NORMAL",
      "to_us_msat": "950000000msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "103x1x1",
      "peer_id": "020000000000000000000000000000000000000000000000000000000000000001",
      "peer_connected": true
    }
  ]
}
)JSON";

class DummyConnector : public Net::Connector {
public:
	Net::SocketFd
	connect(std::string const& host, int port) override {
		(void) host;
		(void) port;
		return Net::SocketFd();
	}
};

class DummySigner : public Secp256k1::SignerIF {
public:
	Secp256k1::PubKey
	get_pubkey_tweak(Secp256k1::PrivKey const& tweak) override {
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
	get_privkey_salted_hash(std::uint8_t salt[32]) override {
		if (!salt)
			return Sha256::Hash();
		auto hash = Sha256::Hash();
		hash.from_buffer(salt);
		return hash;
	}
};

bool has_scid(Jsmn::Object const& arr, char const* scid) {
	for (auto i = std::size_t(0); i < arr.size(); ++i)
		if (std::string(arr[i]["scid"]) == scid)
			return true;
	return false;
}

Ev::Io<void> wait_flag(bool& flag, double start) {
	return Ev::yield().then([&flag, start]() {
		if (flag)
			return Ev::lift();
		assert(Ev::now() - start < 10.0); /* Time out.  */
		return wait_flag(flag, start);
	});
}

Ev::Io<void> set_option(S::Bus& bus, char const* name, char const* json) {
	return bus.raise(Boss::Msg::Option{
		name, Jsmn::Object::parse_json(json), nullptr
	});
}

}

int main() {
	auto bus = S::Bus();
	Boss::Mod::Waiter waiter(bus);

	bus.subscribe<Boss::Msg::RequestRebalanceMode
		     >([&](Boss::Msg::RequestRebalanceMode const& m) {
		return bus.raise(Boss::Msg::ResponseRebalanceMode{
			m.requester, Boss::RebalanceMode::xrebalance
		});
	});

	auto log_lines = std::string();
	bus.subscribe<Boss::Msg::JsonCout
		     >([&](Boss::Msg::JsonCout const& m) {
		log_lines += m.obj.output();
		log_lines += "\n";
		return Ev::lift();
	});

	auto xreb_called = false;
	auto xreb_params = std::string();
	bus.subscribe<Boss::Msg::RequestRpcCommand
		     >([&](Boss::Msg::RequestRpcCommand const& m) {
		auto respond = [&](char const* res) {
			return bus.raise(Boss::Msg::ResponseRpcCommand{
				m.requester, true,
				Jsmn::Object::parse_json(res), ""
			});
		};
		if (m.command == "listpeerchannels")
			return respond(listpeerchannels_result);
		if (m.command == "xrebalance") {
			xreb_params = m.params.output();
			xreb_called = true;
			return respond(R"JSON({})JSON");
		}
		std::cerr << "UNMOCKED COMMAND " << m.command << std::endl;
		assert(0);
		return Ev::lift();
	});

	Boss::Mod::RebalanceUnmanager unmanager(bus, {});

	auto mut = Boss::Mod::XRebalancer(bus, waiter);

	auto connector = DummyConnector();
	auto signer = DummySigner();
	auto db = Sqlite3::Db(":memory:");
	int sockets[2];
	auto sockres = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	assert(sockres >= 0);
	auto server_socket = Net::Fd(sockets[0]);
	auto client_socket = Net::Fd(sockets[1]);
	auto rpc = Boss::Mod::Rpc(bus, std::move(client_socket));

	auto code = Ev::lift().then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query_execute(R"QRY(
		CREATE TABLE "EarningsTracker"
		     ( node TEXT NOT NULL
		     , time_bucket REAL NOT NULL
		     , in_earnings INTEGER NOT NULL
		     , in_forwarded INTEGER NOT NULL
		     , in_expenditures INTEGER NOT NULL
		     , out_earnings INTEGER NOT NULL
		     , out_forwarded INTEGER NOT NULL
		     , out_expenditures INTEGER NOT NULL
		     );
		)QRY");
		auto now = double(std::time(nullptr));
		auto insert = [&]( char const* node
				 , std::int64_t in_e, std::int64_t in_f
				 , std::int64_t out_e, std::int64_t out_f
				 ) {
			tx.query(R"QRY(
			INSERT INTO "EarningsTracker"
			VALUES( :node, :time_bucket
			      , :in_e, :in_f, 0
			      , :out_e, :out_f, 0
			      );
			)QRY")
				.bind(":node", node)
				.bind(":time_bucket", now)
				.bind(":in_e", in_e)
				.bind(":in_f", in_f)
				.bind(":out_e", out_e)
				.bind(":out_f", out_f)
				.execute();
		};
		insert(node_a, 0, 0, 1000000, 1000000000); /* out 1000ppm */
		insert(node_b, 500000, 1000000000, 0, 0);  /* in 500ppm */
		tx.commit();

		/* Pause the Poisson loop so only the demand trigger
		 * below can start a cycle.  */
		return set_option(bus, "clboss-xrebalance-per-hour",
				  R"JSON("0")JSON");
	}).then([&]() {
		return set_option(bus, "clboss-xrebalance-grant",
				  R"JSON("100")JSON");
	}).then([&]() {
		return set_option(bus, "clboss-xrebalance-drain-loc",
				  R"JSON("70")JSON");
	}).then([&]() {
		return bus.raise(Boss::Msg::DbResource{db});
	}).then([&]() {
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Regtest,
			rpc,
			Ln::NodeId(node_a),
			db,
			connector,
			signer,
			std::string(),
			false
		});
	}).then([&]() {
		return bus.raise(Boss::Msg::DemandObserved{
			Ln::Scid(std::string(scid_a))
		});
	}).then([&]() {
		return wait_flag(xreb_called, Ev::now());
	}).then([&]() {
		auto req = Jsmn::Object::parse_json(xreb_params.c_str());
		assert(has_scid(req["destinations"], scid_a));
		assert(has_scid(req["sources"], scid_b));

		/* The fee ceiling is the sum of the two side-weighted
		 * adjusted rates.  */
		assert(log_lines.find(
			"maxfee=1228 ppm (target 820.0 + min offered 407.7)")
			!= std::string::npos);

		return bus.raise(Boss::Shutdown{});
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
