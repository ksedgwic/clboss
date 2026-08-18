#undef NDEBUG
#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"Net/Fd.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<deque>
#include<errno.h>
#include<fcntl.h>
#include<string>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>

namespace {

/* Minimal MockRpcServer: read one JSON-RPC request from the socket,
 * stash it for the test to inspect, then reply with either a success
 * result or an error.  Modelled on test_invoicepayer_decodepay.cpp.
 */
class MockRpcServer {
private:
	Net::Fd socket;
	Jsmn::Parser parser;
	std::deque<Jsmn::Object> requests;

	Ev::Io<Jsmn::Object> read_request(std::size_t retries = 0) {
		return Ev::yield().then([this]() {
			if (requests.empty())
				return Ev::lift(Jsmn::Object());
			auto req = std::move(requests.front());
			requests.pop_front();
			return Ev::lift(std::move(req));
		}).then([this, retries](Jsmn::Object req) {
			if (!req.is_null())
				return Ev::lift(std::move(req));
			assert(retries < 100000);

			char buf[512];
			auto rd = ssize_t();
			do {
				rd = read(socket.get(), buf, sizeof(buf));
			} while (rd < 0 && errno == EINTR);
			if (rd < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
				return read_request(retries + 1);
			assert(rd > 0);

			auto parsed = parser.feed(std::string(buf, std::size_t(rd)));
			for (auto& p : parsed)
				requests.push_back(std::move(p));
			return read_request(retries + 1);
		});
	}

	Ev::Io<void> write_all(std::string data, std::size_t retries = 0) {
		return Ev::yield().then([this, data, retries]() {
			auto wr = ssize_t();
			do {
				wr = write(socket.get(), data.c_str(), data.size());
			} while (wr < 0 && errno == EINTR);
			if (wr < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
				return write_all(data, retries + 1);
			assert(wr >= 0);
			assert(retries < 100000);
			if (std::size_t(wr) < data.size())
				return write_all(data.substr(std::size_t(wr)),
						retries + 1);
			return Ev::lift();
		});
	}

	Ev::Io<void> reply_ok(std::uint64_t id) {
		auto response = Json::Out()
			.start_object()
				.field("jsonrpc", std::string("2.0"))
				.field("id", double(id))
				.start_object("result")
				.end_object()
			.end_object()
			.output();
		return write_all(std::move(response));
	}

	Ev::Io<void> reply_error(std::uint64_t id) {
		auto response = Json::Out()
			.start_object()
				.field("jsonrpc", std::string("2.0"))
				.field("id", double(id))
				.start_object("error")
					.field("code", double(-32000))
					.field("message", std::string("test-induced failure"))
				.end_object()
			.end_object()
			.output();
		return write_all(std::move(response));
	}

public:
	explicit MockRpcServer(Net::Fd socket_)
		: socket(std::move(socket_)), parser(), requests() {
		auto flags = fcntl(socket.get(), F_GETFL);
		assert(flags >= 0);
		flags |= O_NONBLOCK;
		auto fcntl_result = fcntl(socket.get(), F_SETFL, flags);
		assert(fcntl_result == 0);
	}

	/* Read the next request, hand to the asserter callback, then
	 * reply with reply_ok.  Convenience wrapper for happy-path
	 * tests.
	 */
	template <typename F>
	Ev::Io<void> serve_ok(F asserter) {
		return read_request().then([this, asserter = std::move(asserter)](Jsmn::Object req) mutable {
			auto id = asserter(req);
			return reply_ok(id);
		});
	}

	/* Read the next request, run the asserter, then reply with an
	 * RPC error.  For testing the silent-swallow path.
	 */
	template <typename F>
	Ev::Io<void> serve_error(F asserter) {
		return read_request().then([this, asserter = std::move(asserter)](Jsmn::Object req) mutable {
			auto id = asserter(req);
			return reply_error(id);
		});
	}
};

std::uint64_t assert_method(Jsmn::Object const& req, char const* method) {
	assert(req.is_object());
	assert(req.has("id"));
	assert(req["id"].is_number());
	assert(req.has("method"));
	assert(req["method"].is_string());
	assert(std::string(req["method"]) == method);
	return std::uint64_t(double(req["id"]));
}

/* Test 1: disable_node sends askrene-disable-node with the right
 * shape.
 */
Ev::Io<void>
test_disable_node( MockRpcServer& server
		 , Boss::Mod::Rpc& rpc
		 ) {
	auto const node_str = std::string(
		"020000000000000000000000000000000000000000000000000000000000000000"
	);

	auto asserter = [node_str](Jsmn::Object const& req) {
		auto id = assert_method(req, "askrene-disable-node");
		auto params = req["params"];
		assert(std::string(params["layer"]) == "test-layer");
		assert(params.has("node"));
		assert(std::string(params["node"]) == node_str);
		return id;
	};

	return Ev::lift().then([&server, asserter]() {
		return Ev::concurrent(server.serve_ok(std::move(asserter)));
	}).then([&rpc, node_str]() {
		return Boss::Mod::AskreneLayer::disable_node(
			rpc, std::string("test-layer"),
			Ln::NodeId(node_str)
		);
	});
}

/* Test 2: helper silently swallows RpcError -- if the layer is
 * missing (e.g. CLN without askrene), the helper's Ev::Io<void>
 * should still complete cleanly rather than propagating the
 * exception.  Verify by having the mock reply with an RPC error
 * and confirming the helper still completes (we reach the next
 * .then in main).
 */
Ev::Io<void>
test_silent_rpc_error( MockRpcServer& server
		     , Boss::Mod::Rpc& rpc
		     ) {
	auto asserter = [](Jsmn::Object const& req) {
		return assert_method(req, "askrene-disable-node");
	};

	return Ev::lift().then([&server, asserter]() {
		return Ev::concurrent(server.serve_error(std::move(asserter)));
	}).then([&rpc]() {
		return Boss::Mod::AskreneLayer::disable_node(
			rpc, std::string("test-layer"),
			Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000")
		);
	});
}

} // namespace

int main() {
	auto bus = S::Bus();

	int sockets[2];
	auto sockres = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	assert(sockres >= 0);
	auto server_socket = Net::Fd(sockets[0]);
	auto client_socket = Net::Fd(sockets[1]);

	auto server = MockRpcServer(std::move(server_socket));
	auto rpc = Boss::Mod::Rpc(bus, std::move(client_socket));

	auto code = Ev::lift().then([&]() {
		return test_disable_node(server, rpc);
	}).then([&]() {
		return test_silent_rpc_error(server, rpc);
	}).then([&]() {
		/* All tests passed; raise Shutdown so concurrent
		 * server tasks (if any are still alive) and the
		 * event loop exit cleanly.
		 */
		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);
	return 0;
}
