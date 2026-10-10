#undef NDEBUG
#include"Boss/Mod/PeerComplaintsDesk/Main.hpp"
#include"Boss/Mod/PeerComplaintsDesk/Recorder.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/OnchainFee.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"Net/Connector.hpp"
#include"Net/Fd.hpp"
#include"Net/SocketFd.hpp"
#include"S/Bus.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include"Util/stringify.hpp"
#include<algorithm>
#include<array>
#include<assert.h>
#include<cctype>
#include<cstdint>
#include<errno.h>
#include<fcntl.h>
#include<iostream>
#include<memory>
#include<string>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>
#include<vector>

namespace {

/* Mock CLN on the other end of a socketpair: answers
 * listpeerchannels with a switchable peer_connected flag and
 * records close calls.
 */
class MockCln {
private:
	Net::Fd socket;

	Ev::Io<void> writeloop(std::string to_write) {
		return Ev::yield().then([this, to_write]() {
			auto res = write( socket.get()
					, to_write.c_str(), to_write.size()
					);
			if (res < 0 && ( errno == EWOULDBLOCK
				      || errno == EAGAIN
				       ))
				return writeloop(to_write);
			assert(size_t(res) == to_write.size());
			return Ev::yield();
		});
	}
	Ev::Io<std::string> slurp() {
		return Ev::yield().then([this]() -> Ev::Io<std::string> {
			if (done)
				return Ev::lift(std::string());
			auto buf = std::string();
			auto first = true;
			for (;;) {
				char tmp[256];
				auto res = read( socket.get()
					       , tmp, sizeof(tmp)
					       );
				if (res < 0 && ( errno == EWOULDBLOCK
					      || errno == EAGAIN
					       )) {
					if (first)
						/* No data yet.  */
						return slurp();
					break;
				}
				assert(res > 0);
				buf.append(tmp, size_t(res));
				first = false;
			}
			return Ev::lift(buf);
		});
	}

	/* Rpc can queue several requests before the mock reads,
	 * for example the per-channel closes, which run in
	 * parallel, so one read may hold more than one request.
	 * Return the length of the first one.  */
	static std::size_t first_request_size(std::string const& s) {
		auto depth = 0;
		auto in_string = false;
		auto escaped = false;
		for (auto i = std::size_t(0); i < s.size(); ++i) {
			auto c = s[i];
			if (in_string) {
				if (escaped)
					escaped = false;
				else if (c == '\\')
					escaped = true;
				else if (c == '"')
					in_string = false;
			} else if (c == '"')
				in_string = true;
			else if (c == '{')
				++depth;
			else if (c == '}' && --depth == 0)
				return i + 1;
		}
		return s.size();
	}

	Ev::Io<void> handle(std::string req_s) {
		if (req_s.empty())
			return Ev::lift();
		/* Strip the record separators around the request.  */
		while (!req_s.empty() && isspace(req_s.front()))
			req_s.erase(req_s.begin());
		if (req_s.empty())
			return serve();
		auto rest = req_s.substr(first_request_size(req_s));
		req_s.resize(req_s.size() - rest.size());
		while ( isspace(req_s.back())
		      )
			req_s.pop_back();
		auto req = Jsmn::Object::parse_json(req_s.c_str());
		auto id = std::uint64_t(double(req["id"]));
		auto method = std::string(req["method"]);

		auto result = Json::Out();
		auto robj = result.start_object();
		if (method == "listpeerchannels") {
			auto cs = robj.start_array("channels");
			if (channel_ids.empty()) {
				auto c = cs.start_object();
				c.field("peer_connected", connected_flag);
				c.end_object();
			}
			for (auto i = std::size_t(0); i < channel_ids.size(); ++i) {
				auto c = cs.start_object();
				c.field("peer_connected", connected_flag);
				c.field("channel_id", channel_ids[i]);
				c.field("state", i < channel_states.size()
						 ? channel_states[i]
						 : std::string("CHANNELD_NORMAL"));
				c.end_object();
			}
			cs.end_array();
		} else if (method == "close") {
			++close_calls;
			auto params = req["params"];
			last_close_id = std::string(params["id"]);
			close_ids.push_back(last_close_id);
			last_close_timeout = std::uint64_t(double(
				params["unilateraltimeout"]
			));
		} else {
			std::cerr << "Unexpected command: " << method
				  << std::endl;
			assert(false);
		}
		robj.end_object();

		auto js = Json::Out()
			.start_object()
				.field("jsonrpc", std::string("2.0"))
				.field("id", double(id))
				.field("result", std::move(result))
			.end_object()
			.output()
			;
		return writeloop(js).then([this, rest]() {
			if (rest.find_first_not_of(" \t\r\n") == std::string::npos)
				return serve();
			return handle(rest);
		});
	}

public:
	bool done = false;
	bool connected_flag = true;
	std::size_t close_calls = 0;
	std::string last_close_id;
	std::vector<std::string> close_ids;
	/* When set, listpeerchannels reports these channels for the
	 * peer instead of one anonymous channel.  */
	std::vector<std::string> channel_ids;
	/* Per-channel state, CHANNELD_NORMAL where not given.  */
	std::vector<std::string> channel_states;
	std::uint64_t last_close_timeout = 0;

	explicit
	MockCln(Net::Fd socket_) : socket(std::move(socket_)) {
		auto flags = fcntl(socket.get(), F_GETFL);
		flags |= O_NONBLOCK;
		fcntl(socket.get(), F_SETFL, flags);
	}
	MockCln(MockCln&&) =default;
	MockCln(MockCln const&) =delete;

	Ev::Io<void> serve() {
		return slurp().then([this](std::string req_s) {
			return handle(std::move(req_s));
		});
	}
};

class MockConnector : public Net::Connector {
public:
	Net::SocketFd connect(std::string const&, int) override {
		return Net::SocketFd(nullptr);
	}
};

/* Nothing in the close path uses the signer, so an arbitrary
 * fixed key is fine.
 */
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
};

}

int main() {
	namespace Recorder = Boss::Mod::PeerComplaintsDesk::Recorder;

	auto bus = S::Bus();
	auto db = Sqlite3::Db(":memory:");

	auto sockets = std::array<int, 2>();
	auto res = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data());
	assert(res >= 0);
	auto server = MockCln(Net::Fd(sockets[0]));
	auto rpc = Boss::Mod::Rpc(bus, Net::Fd(sockets[1]));

	auto connector = MockConnector();
	auto signer = MockSigner();
	auto self_id = Ln::NodeId(std::string(
		"02000000000000000000000000000000000000000000000000000000000000FFFF"
	));

	/* Module under test.  */
	auto mut = Boss::Mod::PeerComplaintsDesk::Main(bus);

	auto peerA = Ln::NodeId(std::string(
		"0200000000000000000000000000000000000000000000000000000000000000A1"
	));
	auto peerB = Ln::NodeId(std::string(
		"0200000000000000000000000000000000000000000000000000000000000000B2"
	));
	auto peerC = Ln::NodeId(std::string(
		"0200000000000000000000000000000000000000000000000000000000000000C3"
	));
	auto peerD = Ln::NodeId(std::string(
		"0200000000000000000000000000000000000000000000000000000000000000D4"
	));
	auto peerE = Ln::NodeId(std::string(
		"0200000000000000000000000000000000000000000000000000000000000000E5"
	));

	/* Enough non-ignored complaints to cross the close
	 * threshold.
	 */
	auto insert_complaints = [&](Ln::NodeId peer) {
		return db.transact().then([peer](Sqlite3::Tx tx) {
			for (auto i = 0; i < 5; ++i)
				Recorder::add_complaint( tx, peer
						       , "test complaint"
						       );
			tx.commit();
			return Ev::lift();
		});
	};
	/* Drive one 10-minute timer cycle and let the whole
	 * close-check chain run.
	 */
	auto cycle = [&]() {
		return bus.raise(Boss::Msg::Timer10Minutes{}
				).then([]() {
			return Ev::yield(200);
		});
	};
	/* Number of persisted deferred-close records for the
	 * peer (0 or 1).
	 */
	auto pending_count = [&](Ln::NodeId peer) {
		return db.transact().then([peer](Sqlite3::Tx tx) {
			auto n = std::size_t(0);
			auto fetch = tx.query(R"QRY(
			SELECT COUNT(*)
			  FROM "PeerComplaintsDesk_peers" NATURAL JOIN
			       "PeerComplaintsDesk_closepending"
			 WHERE nodeid = :nodeid
			     ;
			)QRY")
				.bind(":nodeid", std::string(peer))
				.execute();
			for (auto& r : fetch)
				n = r.get<std::size_t>(0);
			tx.commit();
			return Ev::lift(n);
		});
	};
	/* Age the peer's deferred-close record into the past.  */
	auto age_pending = [&](Ln::NodeId peer, double secs) {
		return db.transact().then([peer, secs](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			UPDATE "PeerComplaintsDesk_closepending"
			   SET since = since - :secs
			 WHERE peerdbid = (SELECT peerdbid
			                     FROM "PeerComplaintsDesk_peers"
			                    WHERE nodeid = :nodeid)
			     ;
			)QRY")
				.bind(":secs", secs)
				.bind(":nodeid", std::string(peer))
				.execute();
			tx.commit();
			return Ev::lift();
		});
	};

	auto code = Ev::lift().then([&]() {
		return Ev::concurrent(server.serve());
	}).then([&]() {
		return bus.raise(Boss::Msg::DbResource{db});
	}).then([&]() {
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Bitcoin, rpc, self_id, db,
			connector, signer, "", false
		});
	}).then([&]() {
		/* A value that is neither true nor false is refused
		 * from setconfig.  */
		auto reason = std::make_shared<std::string>();
		return bus.raise(Boss::Msg::Option{
			"clboss-auto-close",
			Jsmn::Object::parse_json("\"maybe\""),
			reason
		}).then([reason]() {
			assert(!reason->empty());
			return Ev::lift();
		});
	}).then([&]() {
		/* Enable auto-close, in the string form setconfig
		 * delivers.  */
		auto reason = std::make_shared<std::string>();
		return bus.raise(Boss::Msg::Option{
			"clboss-auto-close",
			Jsmn::Object::parse_json("\"true\""),
			reason
		}).then([reason]() {
			assert(reason->empty());
			return Ev::lift();
		});

	/* A connected peer is closed immediately, regardless of
	 * the feerate (fees_low starts false).
	 */
	}).then([&]() {
		return insert_complaints(peerA);
	}).then([&]() {
		server.connected_flag = true;
		return cycle();
	}).then([&]() {
		assert(server.close_calls == 1);
		assert(server.last_close_id == std::string(peerA));
		assert(server.last_close_timeout == 180);
		/* The channel dying removes the peer from the
		 * candidate set.
		 */
		return bus.raise(Boss::Msg::ChannelDestruction{peerA});
	}).then([&]() {
		return Ev::yield(200);
	}).then([&]() {
		return cycle();
	}).then([&]() {
		/* Not closed again.  */
		assert(server.close_calls == 1);

	/* An offline peer is deferred, and the deferral is
	 * persisted so a restart does not reset patience.
	 */
		return insert_complaints(peerB);
	}).then([&]() {
		server.connected_flag = false;
		return cycle();
	}).then([&]() {
		assert(server.close_calls == 1);
		return pending_count(peerB);
	}).then([&](std::size_t n) {
		assert(n == 1);
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto pendings = Recorder::get_close_pendings(tx);
		tx.commit();
		assert(pendings.count(peerB) == 1);

	/* Still within patience: no close.  */
		return cycle();
	}).then([&]() {
		assert(server.close_calls == 1);

	/* Patience expired, but fees are not low: hold the
	 * unilateral close.
	 */
		return age_pending(peerB, 4 * 24 * 60 * 60);
	}).then([&]() {
		return bus.raise(Boss::Msg::OnchainFee{false, nullptr});
	}).then([&]() {
		return cycle();
	}).then([&]() {
		assert(server.close_calls == 1);

	/* Patience expired and fees low: close anyway.  */
		return bus.raise(Boss::Msg::OnchainFee{true, nullptr});
	}).then([&]() {
		return cycle();
	}).then([&]() {
		assert(server.close_calls == 2);
		assert(server.last_close_id == std::string(peerB));
		return bus.raise(Boss::Msg::ChannelDestruction{peerB});
	}).then([&]() {
		return Ev::yield(200);

	/* A peer that drops below the threshold has its
	 * deferred-close record swept.
	 */
	}).then([&]() {
		server.connected_flag = false;
		return insert_complaints(peerC);
	}).then([&]() {
		return cycle();
	}).then([&]() {
		return pending_count(peerC);
	}).then([&](std::size_t n) {
		assert(n == 1);
		/* Complaints gone (expired or recovered).  */
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query(R"QRY(
		DELETE FROM "PeerComplaintsDesk_complaints";
		)QRY").execute();
		tx.commit();
		return Ev::lift();
	}).then([&]() {
		return cycle();
	}).then([&]() {
		return pending_count(peerC);
	}).then([&](std::size_t n) {
		assert(n == 0);
		assert(server.close_calls == 2);

	/* With auto-close disabled, even a connected peer over
	 * the threshold is not closed.
	 */
		return bus.raise(Boss::Msg::Option{
			"clboss-auto-close",
			Jsmn::Object::parse_json("{\"enabled\": false}")["enabled"]
		});
	}).then([&]() {
		return insert_complaints(peerC);
	}).then([&]() {
		server.connected_flag = true;
		return cycle();
	}).then([&]() {
		assert(server.close_calls == 2);

	/* A peer with two channels: `close id=<node>` would be
	 * refused by lightningd ("Peer has multiple channels"), so
	 * each channel is closed by its channel id (#352).
	 */
		return bus.raise(Boss::Msg::Option{
			"clboss-auto-close",
			Jsmn::Object::parse_json("{\"enabled\": true}")["enabled"]
		});
	}).then([&]() {
		return bus.raise(Boss::Msg::ChannelDestruction{peerC});
	}).then([&]() {
		return Ev::yield(200);
	}).then([&]() {
		server.close_ids.clear();
		server.channel_ids = {
			std::string(64, '1'),
			std::string(64, '2')
		};
		server.connected_flag = true;
		return insert_complaints(peerD);
	}).then([&]() {
		return cycle();
	}).then([&]() {
		assert(server.close_calls == 4);
		auto ids = server.close_ids;
		std::sort(ids.begin(), ids.end());
		assert(ids.size() == 2);
		assert(ids[0] == std::string(64, '1'));
		assert(ids[1] == std::string(64, '2'));
		return bus.raise(Boss::Msg::ChannelDestruction{peerD});
	}).then([&]() {
		return Ev::yield(200);

	/* One closable channel beside one already shutting down:
	 * lightningd still refuses the node id, so the closable
	 * channel is closed by its id and the other is left alone.
	 */
	}).then([&]() {
		server.close_ids.clear();
		server.channel_ids = {
			std::string(64, '3'),
			std::string(64, '4')
		};
		server.channel_states = {
			std::string("CHANNELD_NORMAL"),
			std::string("CHANNELD_SHUTTING_DOWN")
		};
		return insert_complaints(peerE);
	}).then([&]() {
		return cycle();
	}).then([&]() {
		assert(server.close_calls == 5);
		assert(server.close_ids.size() == 1);
		assert(server.close_ids[0] == std::string(64, '3'));

		/* Stop the Rpc watchers so the event loop can
		 * drain and Ev::start can return.
		 */
		server.done = true;
		return bus.raise(Boss::Shutdown());
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
