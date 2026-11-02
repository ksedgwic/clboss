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
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RequestRebalanceMode.hpp"
#include"Boss/Msg/RequestRpcCommand.hpp"
#include"Boss/Msg/ResponseRebalanceMode.hpp"
#include"Boss/Msg/ResponseRpcCommand.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
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
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include<assert.h>
#include<cstddef>
#include<ctime>
#include<functional>
#include<iostream>
#include<stdexcept>
#include<string>
#include<sys/socket.h>
#include<vector>

/* Exercises the plugin-health reporting of XRebalancer: the
 * not-loaded warning (first cycle, hourly after that, debug in
 * between), the recovery line, an ordinary plugin error, and the
 * `xrebalancer` entry of clboss-status.  Cycles are driven by
 * Msg::DemandObserved with the Poisson loop paused; the RPC stub
 * answers `xrebalance` as the test directs, and a fake clock stands
 * in for the hour.  */

namespace {

/* Peer A: 5% local -> fill candidate; the demand target.  */
auto const node_a = "020000000000000000000000000000000000000000000000000000000000000000";
/* Peer B: 95% local -> drain candidate.  */
auto const node_b = "020000000000000000000000000000000000000000000000000000000000000001";
/* Peer C: 95% local, unmanaged (keeps the setup identical to
 * test_xrebalancer, which is known to produce a cycle).  */
auto const node_c = "020000000000000000000000000000000000000000000000000000000000000002";

auto const scid_a = "103x1x0";

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

/* What lightningd answers for a command no plugin registers.  */
auto const unknown_command_error = R"JSON(
{"code": -32601, "message": "Unknown command 'xrebalance'"}
)JSON";
/* An error from the plugin itself.  */
auto const refused_error = R"JSON(
{"code": -32602, "message": "amount_msat: should be a positive amount"}
)JSON";

double mock_now = 1700000000.0;
double mock_get_now() { return mock_now; }

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
	Secp256k1::KeyPair
	get_keypair_tweak(Secp256k1::PrivKey const&) override {
		throw std::logic_error("Not expected to use.");
	}
};

struct LogLine {
	std::string level;
	std::string message;
};

Ev::Io<void> wait_until(std::function<bool()> pred, double start) {
	return Ev::yield().then([pred, start]() {
		if (pred())
			return Ev::lift();
		assert(Ev::now() - start < 10.0); /* Time out.  */
		return wait_until(pred, start);
	});
}

/* Let the cycle's continuation run to its end (it clears the
 * in-flight flag after logging), so the next demand is not
 * discarded.  */
Ev::Io<void> settle(unsigned n) {
	if (n == 0)
		return Ev::lift();
	return Ev::yield().then([n]() {
		return settle(n - 1);
	});
}

enum class Answer { Ok, Absent, Refused };

}

int main() {
	auto bus = S::Bus();
	Boss::Mod::Waiter waiter(bus);

	/* Mode stub: always xrebalance.  */
	bus.subscribe<Boss::Msg::RequestRebalanceMode
		     >([&](Boss::Msg::RequestRebalanceMode const& m) {
		return bus.raise(Boss::Msg::ResponseRebalanceMode{
			m.requester, Boss::RebalanceMode::xrebalance
		});
	});

	/* Collect log lines with their levels (Boss::log emits
	 * JsonCout).  */
	auto logs = std::vector<LogLine>();
	bus.subscribe<Boss::Msg::JsonCout
		     >([&](Boss::Msg::JsonCout const& m) {
		auto js = Jsmn::Object::parse_json(m.obj.output().c_str());
		if ( js.is_object() && js.has("method")
		  && js["method"].is_string()
		  && std::string(js["method"]) == "log"
		   ) {
			auto p = js["params"];
			logs.push_back(LogLine{
				std::string(p["level"]),
				std::string(p["message"])
			});
		}
		return Ev::lift();
	});
	auto count = [&](char const* level, char const* needle) {
		auto n = std::size_t(0);
		for (auto const& l : logs)
			if ( l.level == level
			  && l.message.find(needle) != std::string::npos
			   )
				++n;
		return n;
	};

	/* Status capture.  */
	auto status_text = std::string();
	bus.subscribe<Boss::Msg::ProvideStatus
		     >([&](Boss::Msg::ProvideStatus const& m) {
		if (m.key == "xrebalancer")
			status_text = m.value.output();
		return Ev::lift();
	});
	auto get_status = [&]() {
		status_text.clear();
		return bus.raise(Boss::Msg::SolicitStatus{}
		).then([&]() {
			assert(!status_text.empty());
			return Ev::lift(Jsmn::Object::parse_json(
				status_text.c_str()
			));
		});
	};

	/* RPC stub: `xrebalance` answers as directed.  */
	auto answer = Answer::Ok;
	auto xreb_calls = std::size_t(0);
	bus.subscribe<Boss::Msg::RequestRpcCommand
		     >([&](Boss::Msg::RequestRpcCommand const& m) {
		auto respond = [&](bool ok, char const* res) {
			return bus.raise(Boss::Msg::ResponseRpcCommand{
				m.requester, ok,
				Jsmn::Object::parse_json(res), m.command
			});
		};
		if (m.command == "listpeerchannels")
			return respond(true, listpeerchannels_result);
		if (m.command == "xrebalance") {
			++xreb_calls;
			switch (answer) {
			case Answer::Ok:
				return respond(true, R"JSON({})JSON");
			case Answer::Absent:
				return respond(false, unknown_command_error);
			case Answer::Refused:
				return respond(false, refused_error);
			}
		}
		std::cerr << "UNMOCKED COMMAND " << m.command << std::endl;
		assert(0);
		return Ev::lift();
	});

	/* One demand-triggered cycle, run to completion.  */
	auto cycle = [&]() {
		auto before = xreb_calls;
		return bus.raise(Boss::Msg::DemandObserved{
			Ln::Scid(std::string(scid_a))
		}).then([&, before]() {
			return wait_until([&, before]() {
				return xreb_calls > before;
			}, Ev::now());
		}).then([]() {
			return settle(20);
		});
	};

	Boss::Mod::RebalanceUnmanager unmanager(bus, {node_c});

	/* Module under test, on the fake clock.  */
	auto mut = Boss::Mod::XRebalancer(bus, waiter, &mock_get_now);

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
		insert(node_c, 2000000, 1000000000, 0, 0); /* in 2000ppm */
		tx.commit();

		/* Pause the Poisson loop so only demand triggers run
		 * cycles.  */
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
		/* Before any cycle: nothing known.  */
		return get_status();
	}).then([&](Jsmn::Object s) {
		assert(std::string(s["plugin"]) == "unknown");
		assert(s["last_response"].is_null());
		assert(s["last_response_human"].is_null());
		assert(double(s["consecutive_failures"]) == 0.0);
		assert(s["last_error"].is_null());

		/* First cycle finds the plugin missing: one Warn.  */
		answer = Answer::Absent;
		return cycle();
	}).then([&]() {
		assert(count("warn", "xrebalance plugin not loaded") == 1);
		assert(count("warn", "clboss-rebalance-mode=off") == 1);
		assert(count("debug", "still not loaded") == 0);
		assert(count("info", "did not execute") == 0);

		/* Second cycle within the hour: Debug only.  */
		return cycle();
	}).then([&]() {
		assert(count("warn", "xrebalance plugin not loaded") == 1);
		assert(count("debug", "still not loaded") == 1);
		return get_status();
	}).then([&](Jsmn::Object s) {
		assert(std::string(s["plugin"]) == "absent");
		assert(s["last_response"].is_null());
		assert(double(s["consecutive_failures"]) == 2.0);
		assert(std::string(s["last_error"]).find("Unknown command")
			!= std::string::npos);

		/* An hour later, still missing: the Warn repeats.  */
		mock_now += 3601.0;
		return cycle();
	}).then([&]() {
		assert(count("warn", "xrebalance plugin not loaded") == 2);
		assert(count("debug", "still not loaded") == 1);

		/* The plugin answers again.  */
		answer = Answer::Ok;
		return cycle();
	}).then([&]() {
		assert(count("info", "back after 3 absent cycle(s)") == 1);
		return get_status();
	}).then([&](Jsmn::Object s) {
		assert(std::string(s["plugin"]) == "present");
		assert(double(s["last_response"]) == mock_now);
		assert(s["last_response_human"].is_string());
		assert(std::string(s["last_response_human"]).find("UTC")
			!= std::string::npos);
		assert(double(s["consecutive_failures"]) == 0.0);
		assert(s["last_error"].is_null());

		/* The plugin is loaded but refuses the request: the
		 * ordinary per-cycle Info line, no Warn.  */
		answer = Answer::Refused;
		return cycle();
	}).then([&]() {
		assert(count("info", "did not execute: amount_msat") == 1);
		assert(count("warn", "xrebalance plugin not loaded") == 2);
		return get_status();
	}).then([&](Jsmn::Object s) {
		assert(std::string(s["plugin"]) == "present");
		assert(double(s["consecutive_failures"]) == 1.0);
		assert(std::string(s["last_error"]).find("amount_msat")
			!= std::string::npos);

		/* A fresh outage after a recovery warns at once.  */
		answer = Answer::Absent;
		return cycle();
	}).then([&]() {
		assert(count("warn", "xrebalance plugin not loaded") == 3);
		return get_status();
	}).then([&](Jsmn::Object s) {
		assert(std::string(s["plugin"]) == "absent");
		/* The refusal counted too: two failures in a row.  */
		assert(double(s["consecutive_failures"]) == 2.0);
		/* The last answer's time survives the outage.  */
		assert(double(s["last_response"]) == mock_now);

		return bus.raise(Boss::Shutdown{});
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
