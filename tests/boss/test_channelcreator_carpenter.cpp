#undef NDEBUG
#include"Boss/Mod/ChannelCreator/Carpenter.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Msg/ChannelCreateResult.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
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
#include"Sqlite3.hpp"
#include<array>
#include<assert.h>
#include<cctype>
#include<errno.h>
#include<fcntl.h>
#include<iostream>
#include<map>
#include<sstream>
#include<string>
#include<sys/socket.h>
#include<unistd.h>
#include<vector>

/* The Carpenter checks listpeerchannels again right before
 * multifundchannel: a planned peer that has a channel with us by
 * then is reported as a failure and not funded, and a failed check
 * funds nothing.  */

namespace {

auto const A = Ln::NodeId("0200000000000000000000000000000000000000000000000000000000000000A1");
auto const B = Ln::NodeId("0200000000000000000000000000000000000000000000000000000000000000B2");
auto const C = Ln::NodeId("0200000000000000000000000000000000000000000000000000000000000000C3");

/* A fake lightningd on the far end of the RPC socket.  */
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
	/* Several requests can arrive in one read (the Carpenter
	 * connects to every peer in parallel), so parse them one
	 * JSON value at a time.  */
	Ev::Io<void> handle(std::string req_s) {
		if (req_s.empty())
			return Ev::lift();
		auto is = std::istringstream(std::move(req_s));
		auto out = std::string();
		for (;;) {
			auto req = Jsmn::Object();
			is >> req;
			if (!req.is_object())
				break;
			out += respond(req);
		}
		return writeloop(out).then([this]() {
			return serve();
		});
	}
	std::string respond(Jsmn::Object const& req) {
		auto id = std::uint64_t(double(req["id"]));
		auto method = std::string(req["method"]);
		auto params = req["params"];
		auto result = Json::Out();
		auto robj = result.start_object();
		auto error = false;
		if (method == "connect") {
			robj.field("id", std::string(params["id"]));
		} else if (method == "listpeerchannels") {
			++listpeerchannels_calls;
			if (listpeerchannels_fails) {
				error = true;
			} else {
				auto cs = robj.start_array("channels");
				for (auto const& n : channeled) {
					auto c = cs.start_object();
					c.field("peer_id", std::string(n));
					c.field("peer_connected", true);
					c.field("state", std::string("CHANNELD_NORMAL"));
					c.end_object();
				}
				cs.end_array();
			}
		} else if (method == "multifundchannel") {
			++multifundchannel_calls;
			funded.clear();
			for (auto d : params["destinations"])
				funded.push_back(Ln::NodeId(std::string(d["id"])));
			auto cs = robj.start_array("channel_ids");
			for (auto const& n : funded) {
				auto c = cs.start_object();
				c.field("id", std::string(n));
				c.end_object();
			}
			cs.end_array();
			auto fs = robj.start_array("failed");
			fs.end_array();
		} else {
			std::cerr << "Unexpected command: " << method
				  << std::endl;
			assert(false);
		}
		robj.end_object();
		auto out = Json::Out();
		auto js = out.start_object();
		js.field("jsonrpc", std::string("2.0"))
		  .field("id", double(id));
		if (error)
			js.field("error", Json::Out().start_object()
					.field("code", double(-1))
					.field("message", std::string("mock failure"))
				.end_object()
			);
		else
			js.field("result", std::move(result));
		js.end_object();
		return out.output();
	}

public:
	/* Set to end the serve loop.  */
	bool done = false;
	/* Peers that listpeerchannels reports a channel with.  */
	std::vector<Ln::NodeId> channeled;
	bool listpeerchannels_fails = false;
	std::size_t listpeerchannels_calls = 0;
	std::size_t multifundchannel_calls = 0;
	/* Destinations of the last multifundchannel.  */
	std::vector<Ln::NodeId> funded;

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
/* Nothing in the construction path uses the signer.  */
class MockSigner : public Secp256k1::SignerIF {
public:
	Secp256k1::PubKey get_pubkey_tweak(Secp256k1::PrivKey const&) override {
		return Secp256k1::PubKey();
	}
	Secp256k1::Signature get_signature_tweak( Secp256k1::PrivKey const&
						, Sha256::Hash const&
						) override {
		return Secp256k1::Signature();
	}
	Sha256::Hash get_privkey_salted_hash(std::uint8_t salt[32]) override {
		auto hash = Sha256::Hash();
		if (salt)
			hash.from_buffer(salt);
		return hash;
	}
};

}

int main() {
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

	auto waiter = Boss::Mod::Waiter(bus);
	/* Module under test.  */
	auto carpenter = Boss::Mod::ChannelCreator::Carpenter(bus, waiter);

	/* Every ChannelCreateResult, in order.  */
	auto results = std::vector<std::pair<Ln::NodeId, bool>>();
	bus.subscribe<Boss::Msg::ChannelCreateResult
		     >([&](Boss::Msg::ChannelCreateResult const& r) {
		results.push_back(std::make_pair(r.node, r.success));
		return Ev::lift();
	});
	auto result_of = [&](Ln::NodeId n) {
		for (auto const& r : results)
			if (r.first == n)
				return std::string(r.second ? "ok" : "failed");
		return std::string("none");
	};

	auto code = Ev::lift().then([&]() {
		return Ev::concurrent(server.serve());
	}).then([&]() {
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Bitcoin, rpc, self_id, db,
			connector, signer, "", false
		});
	}).then([&]() {
		/* A has a channel with us by the time the plan is
		 * funded; C was planned at 0.  Only B is funded.  */
		server.channeled = {A};
		auto plan = std::map<Ln::NodeId, Ln::Amount>();
		plan[A] = Ln::Amount::sat(500000);
		plan[B] = Ln::Amount::sat(600000);
		plan[C] = Ln::Amount::sat(0);
		return carpenter.construct(std::move(plan));
	}).then([&]() {
		return Ev::yield(200);
	}).then([&]() {
		assert(server.listpeerchannels_calls == 1);
		assert(server.multifundchannel_calls == 1);
		assert(server.funded == std::vector<Ln::NodeId>{B});
		assert(results.size() == 3);
		assert(result_of(A) == "failed");
		assert(result_of(B) == "ok");
		assert(result_of(C) == "failed");

		/* The check fails: nothing is funded, and no result is
		 * reported for B, so it stays a candidate.  */
		results.clear();
		server.listpeerchannels_fails = true;
		auto plan = std::map<Ln::NodeId, Ln::Amount>();
		plan[B] = Ln::Amount::sat(600000);
		return carpenter.construct(std::move(plan));
	}).then([&]() {
		return Ev::yield(200);
	}).then([&]() {
		assert(server.listpeerchannels_calls == 2);
		assert(server.multifundchannel_calls == 1);
		assert(results.empty());

		/* Stop the Rpc watchers so the event loop can drain
		 * and Ev::start can return.  */
		server.done = true;
		return bus.raise(Boss::Shutdown());
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
