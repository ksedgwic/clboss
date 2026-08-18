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

/* Exercises the XRebalancer demand-cycle pipeline end to end on the
 * bus, checking that a peer unmanaged for "balance" is excluded from
 * both the fill and drain pools: peer C below is deliberately the
 * best-paying drain candidate, so without the exclusion it would top
 * the source list of every cycle.  */

namespace {

/* Peer A: 5% local -> fill candidate; the demand target.  */
auto const node_a = "020000000000000000000000000000000000000000000000000000000000000000";
/* Peer B: 95% local -> drain candidate.  */
auto const node_b = "020000000000000000000000000000000000000000000000000000000000000001";
/* Peer C: 95% local, best in_net -> drain candidate, but unmanaged.  */
auto const node_c = "020000000000000000000000000000000000000000000000000000000000000002";

auto const scid_a = "103x1x0";
auto const scid_b = "103x1x1";
auto const scid_c = "103x2x0";

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
    },
    {
      "state": "CHANNELD_NORMAL",
      "to_us_msat": "950000000msat",
      "total_msat": "1000000000msat",
      "short_channel_id": "103x2x0",
      "peer_id": "020000000000000000000000000000000000000000000000000000000000000002",
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

/* The plugin request carries sources/destinations as arrays of
 * {scid, max_msat} objects.  */
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

}

int main() {
	auto bus = S::Bus();
	Boss::Mod::Waiter waiter(bus);

	/* Mode stub: always xrebalance (the cycle surfaces as an
	 * `xrebalance` plugin RPC we can capture).  */
	bus.subscribe<Boss::Msg::RequestRebalanceMode
		     >([&](Boss::Msg::RequestRebalanceMode const& m) {
		return bus.raise(Boss::Msg::ResponseRebalanceMode{
			m.requester, Boss::RebalanceMode::xrebalance
		});
	});

	/* Collect log output (Boss::log emits JsonCout).  */
	auto log_lines = std::string();
	bus.subscribe<Boss::Msg::JsonCout
		     >([&](Boss::Msg::JsonCout const& m) {
		log_lines += m.obj.output();
		log_lines += "\n";
		return Ev::lift();
	});

	/* RPC stub.  */
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

	Boss::Mod::RebalanceUnmanager unmanager(bus, {node_c});

	/* Module under test.  */
	auto mut = Boss::Mod::XRebalancer(bus, waiter);

	/* Init carries references the XRebalancer never touches; the
	 * Rpc rides on an idle socketpair.  */
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
		/* The columns the XRebalancer NetPpm query reads.  All
		 * three peers get a positive net on the side that makes
		 * them pool candidates; C pays the most on the drain
		 * side.  */
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
		insert(node_c, 2000000, 1000000000, 0, 0); /* in 2000ppm */
		tx.commit();

		/* Pause the Poisson loop so only the demand trigger
		 * below can start a cycle.  */
		return bus.raise(Boss::Msg::Option{
			"clboss-xrebalance-per-hour",
			Jsmn::Object::parse_json(R"JSON("0")JSON"),
			nullptr
		});
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
		/* A forward just spent A's outgoing liquidity.  */
		return bus.raise(Boss::Msg::DemandObserved{
			Ln::Scid(std::string(scid_a))
		});
	}).then([&]() {
		return wait_flag(xreb_called, Ev::now());
	}).then([&]() {
		auto req = Jsmn::Object::parse_json(xreb_params.c_str());
		auto srcs = req["sources"];
		auto dsts = req["destinations"];

		/* The demand target fills A.  */
		assert(has_scid(dsts, scid_a));
		/* B is the only remaining drain source.  */
		assert(has_scid(srcs, scid_b));
		/* The unmanaged C appears on neither side, even though
		 * it out-pays B.  */
		assert(!has_scid(srcs, scid_c));
		assert(!has_scid(dsts, scid_c));

		/* The exclusion is named in the cycle's log.  */
		assert(log_lines.find(std::string(
			"unmanaged (balance), excluded: ") + node_c)
			!= std::string::npos);

		return bus.raise(Boss::Shutdown{});
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
