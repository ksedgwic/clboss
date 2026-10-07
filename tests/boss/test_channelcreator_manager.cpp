#undef NDEBUG
#include"Boss/Mod/ChannelCandidateInvestigator/Main.hpp"
#include"Boss/Mod/ChannelCreator/Carpenter.hpp"
#include"Boss/Mod/ChannelCreator/Manager.hpp"
#include"Boss/Mod/InternetConnectionMonitor.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Msg/AmountSettings.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/RequestChannelCreation.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/now.hpp"
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
#include<cstdint>
#include<errno.h>
#include<fcntl.h>
#include<functional>
#include<iostream>
#include<sstream>
#include<string>
#include<sys/socket.h>
#include<unistd.h>
#include<vector>

/* The Manager runs one creation cycle at a time: a request that
 * arrives while a cycle runs is skipped, and a later request starts
 * a cycle again once the running one has ended, by completion or by
 * a caught exception.  A cycle is held open at its first RPC call,
 * getinfo, which the fake lightningd answers on demand.  With no
 * candidates on record a released cycle ends without funding.  */

namespace {

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
	/* Several requests can arrive in one read, so parse them one
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
		if (out.empty())
			return serve();
		return writeloop(out).then([this]() {
			return serve();
		});
	}
	static
	std::string reply(std::uint64_t id, Json::Out result, bool error) {
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
	static
	Json::Out getinfo_result() {
		return Json::Out().start_object()
			.field("num_pending_channels", double(0))
			.field("num_active_channels", double(0))
		.end_object();
	}
	std::string respond(Jsmn::Object const& req) {
		auto id = std::uint64_t(double(req["id"]));
		auto method = std::string(req["method"]);
		if (method == "getinfo") {
			++getinfo_calls;
			if (hold_getinfo) {
				assert(!has_held);
				held_id = id;
				has_held = true;
				return "";
			}
			return reply(id, getinfo_result(), false);
		} else if (method == "listpeerchannels") {
			return reply(id, Json::Out().start_object()
					.start_array("channels")
					.end_array()
				.end_object(), false);
		} else if (method == "listpeers") {
			return reply(id, Json::Out().start_object()
					.start_array("peers")
					.end_array()
				.end_object(), false);
		}
		std::cerr << "Unexpected command: " << method << std::endl;
		assert(false);
		return "";
	}

public:
	/* Set to end the serve loop.  */
	bool done = false;
	/* While set, getinfo is received but not answered until
	 * release_getinfo.  */
	bool hold_getinfo = false;
	bool has_held = false;
	std::uint64_t held_id = 0;
	std::size_t getinfo_calls = 0;

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
	/* Answer the held getinfo, with a result or an error.  */
	Ev::Io<void> release_getinfo(bool error) {
		assert(has_held);
		has_held = false;
		return writeloop(reply(held_id, getinfo_result(), error));
	}
};

class MockConnector : public Net::Connector {
public:
	Net::SocketFd connect(std::string const&, int) override {
		return Net::SocketFd(nullptr);
	}
};
/* Nothing on this path uses the signer.  */
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

/* A concurrent task runs on a later pass of the event loop; this
 * many yields flush whatever the message handlers started.  */
auto const flush_yields = std::size_t(16);

/* Printed when a wait times out.  */
std::vector<std::string> const* logs_on_timeout = nullptr;

/* Yield until the condition holds.  The bound only ends a failing
 * run.  */
Ev::Io<void> wait_for(std::function<bool()> cond, double start) {
	return Ev::yield().then([cond, start]() {
		if (cond())
			return Ev::lift();
		if (Ev::now() - start >= 10.0 && logs_on_timeout)
			for (auto const& l : *logs_on_timeout)
				std::cerr << "LOG: " << l << std::endl;
		assert(Ev::now() - start < 10.0); /* Time out.  */
		return wait_for(cond, start);
	});
}
Ev::Io<void> wait_for(std::function<bool()> cond) {
	return wait_for(std::move(cond), Ev::now());
}

}

int main() {
	auto bus = S::Bus();
	auto threadpool = Ev::ThreadPool();
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
	auto imon = Boss::Mod::InternetConnectionMonitor(bus, threadpool, waiter);
	auto investigator = Boss::Mod::ChannelCandidateInvestigator::Main(bus, imon);
	auto carpenter = Boss::Mod::ChannelCreator::Carpenter(bus, waiter);
	/* Module under test.  */
	auto manager = Boss::Mod::ChannelCreator::Manager( bus
							  , investigator
							  , carpenter
							  );

	/* Every log line, in order.  */
	auto logs = std::vector<std::string>();
	logs_on_timeout = &logs;
	bus.subscribe<Boss::Msg::JsonCout
		     >([&](Boss::Msg::JsonCout const& m) {
		auto is = std::istringstream(m.obj.output());
		auto js = Jsmn::Object();
		is >> js;
		if ( !js.is_object() || !js.has("method")
		  || std::string(js["method"]) != "log"
		   )
			return Ev::lift();
		logs.push_back(std::string(js["params"]["message"]));
		return Ev::lift();
	});
	auto logged = [&](std::string const& needle) {
		auto n = std::size_t(0);
		for (auto const& l : logs)
			if (l.find(needle) != std::string::npos)
				++n;
		return n;
	};
	auto const skipped = "a cycle is running";
	auto const errored = "cycle error";
	auto const completed = "Insufficient channel candidates";

	auto request = [&]() {
		return bus.raise(Boss::Msg::RequestChannelCreation{
			Ln::Amount::sat(2000000)
		});
	};

	auto code = Ev::lift().then([&]() {
		return Ev::concurrent(server.serve());
	}).then([&]() {
		return bus.raise(Boss::Msg::AmountSettings{
			Ln::Amount::sat(500000), Ln::Amount::sat(16777215),
			Ln::Amount::sat(30000), Ln::Amount::sat(1000000),
			Ln::Amount::sat(50000)
		});
	}).then([&]() {
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Bitcoin, rpc, self_id, db,
			connector, signer, "", false
		});
	}).then([&]() {
		/* A cycle, held open at getinfo.  */
		server.hold_getinfo = true;
		return request();
	}).then([&]() {
		return wait_for([&]() { return server.has_held; });
	}).then([&]() {
		assert(server.getinfo_calls == 1);
		/* A request while it runs is skipped.  */
		return request();
	}).then([&]() {
		return Ev::yield(flush_yields);
	}).then([&]() {
		assert(server.getinfo_calls == 1);
		assert(logged(skipped) == 1);
		/* The cycle ends in an RPC error: caught and logged.  */
		server.hold_getinfo = false;
		return server.release_getinfo(true);
	}).then([&]() {
		return wait_for([&]() { return logged(errored) == 1; });
	}).then([&]() {
		/* The cycle ends a few event-loop passes after its last
		 * log line.  */
		return Ev::yield(flush_yields);
	}).then([&]() {
		/* A request after the error starts a cycle, which runs
		 * to completion with no candidates.  */
		return request();
	}).then([&]() {
		return wait_for([&]() { return logged(completed) == 1; });
	}).then([&]() {
		return Ev::yield(flush_yields);
	}).then([&]() {
		assert(server.getinfo_calls == 2);
		assert(logged(skipped) == 1);
		/* A request after the completion starts a cycle; hold
		 * it to show it is running.  */
		server.hold_getinfo = true;
		return request();
	}).then([&]() {
		return wait_for([&]() { return server.has_held; });
	}).then([&]() {
		assert(server.getinfo_calls == 3);
		return request();
	}).then([&]() {
		return Ev::yield(flush_yields);
	}).then([&]() {
		assert(server.getinfo_calls == 3);
		assert(logged(skipped) == 2);
		server.hold_getinfo = false;
		return server.release_getinfo(false);
	}).then([&]() {
		return wait_for([&]() { return logged(completed) == 2; });
	}).then([&]() {
		assert(server.getinfo_calls == 3);
		assert(logged(errored) == 1);

		/* Stop the Rpc watchers so the event loop can drain
		 * and Ev::start can return.  */
		server.done = true;
		return bus.raise(Boss::Shutdown());
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
