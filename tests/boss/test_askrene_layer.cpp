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
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
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

/* Test 1: inform_channel_constrained sends the expected JSON-RPC
 * call shape and completes when the server replies success.
 *
 * Pattern (from test_rpc.cpp): server runs as Ev::concurrent in the
 * background; the helper runs in the main chain.  When the helper
 * completes, the chain ends -- no flag polling required.
 */
Ev::Io<void>
test_inform_channel_constrained( MockRpcServer& server
			       , Boss::Mod::Rpc& rpc
			       ) {
	auto asserter = [](Jsmn::Object const& req) {
		auto id = assert_method(req, "askrene-inform-channel");
		assert(req.has("params"));
		auto params = req["params"];
		assert(params.is_object());
		assert(params.has("layer"));
		assert(std::string(params["layer"]) == "test-layer");
		assert(params.has("short_channel_id_dir"));
		assert(std::string(params["short_channel_id_dir"]) == "100x1x0/1");
		assert(params.has("amount_msat"));
		assert(double(params["amount_msat"]) == 500000.0);
		assert(params.has("inform"));
		assert(std::string(params["inform"]) == "constrained");
		return id;
	};

	return Ev::lift().then([&server, asserter]() {
		return Ev::concurrent(server.serve_ok(std::move(asserter)));
	}).then([&rpc]() {
		return Boss::Mod::AskreneLayer::inform_channel_constrained(
			rpc, std::string("test-layer"),
			Ln::Scid("100x1x0"), std::uint32_t(1),
			Ln::Amount::msat(500000)
		);
	});
}

/* Test 2: inform_channel_unconstrained sends the same shape but
 * inform="unconstrained".  This is askrene's mode for "channel
 * proved capacity >= amount; raise the lower bound."  Askrene
 * also has an inform="succeeded" mode but that one is currently
 * a no-op stub upstream, so we use "unconstrained" for the
 * lower-bound-raise semantic (same as xpay does).
 */
Ev::Io<void>
test_inform_channel_unconstrained( MockRpcServer& server
				 , Boss::Mod::Rpc& rpc
				 ) {
	auto asserter = [](Jsmn::Object const& req) {
		auto id = assert_method(req, "askrene-inform-channel");
		auto params = req["params"];
		assert(std::string(params["short_channel_id_dir"]) == "200x2x0/0");
		assert(double(params["amount_msat"]) == 750000.0);
		assert(std::string(params["inform"]) == "unconstrained");
		return id;
	};

	return Ev::lift().then([&server, asserter]() {
		return Ev::concurrent(server.serve_ok(std::move(asserter)));
	}).then([&rpc]() {
		return Boss::Mod::AskreneLayer::inform_channel_unconstrained(
			rpc, std::string("test-layer"),
			Ln::Scid("200x2x0"), std::uint32_t(0),
			Ln::Amount::msat(750000)
		);
	});
}

/* Test 3: disable_node sends askrene-disable-node with the right
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

/* Test 4: helper silently swallows RpcError -- if the layer is
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
		return assert_method(req, "askrene-inform-channel");
	};

	return Ev::lift().then([&server, asserter]() {
		return Ev::concurrent(server.serve_error(std::move(asserter)));
	}).then([&rpc]() {
		return Boss::Mod::AskreneLayer::inform_channel_constrained(
			rpc, std::string("test-layer"),
			Ln::Scid("300x3x0"), std::uint32_t(0),
			Ln::Amount::msat(100)
		);
	});
}

/* Test 5 (pure): the write-coalescing decision (inform_coalesce_emit).
 * No RPC -- exercises the dominance + bucket rule directly. */
void test_coalesce_decision() {
	using Boss::Mod::AskreneLayer::InformObs;
	using Boss::Mod::AskreneLayer::inform_coalesce_emit;

	/* No prior: always emit, either kind. */
	assert(inform_coalesce_emit(nullptr, 100, 500, true));
	assert(inform_coalesce_emit(nullptr, 100, 500, false));

	auto const prior = InformObs{ 100, 500 };

	/* New bucket: emit even when not tighter (keep-alive vs aging). */
	assert(inform_coalesce_emit(&prior, 101, 500, true));
	assert(inform_coalesce_emit(&prior, 101, 10, true));

	/* Same bucket, lower bound (min): emit iff strictly higher. */
	assert( inform_coalesce_emit(&prior, 100, 600, true));
	assert(!inform_coalesce_emit(&prior, 100, 500, true));
	assert(!inform_coalesce_emit(&prior, 100, 400, true));

	/* Same bucket, upper bound (max): emit iff strictly lower. */
	assert( inform_coalesce_emit(&prior, 100, 400, false));
	assert(!inform_coalesce_emit(&prior, 100, 500, false));
	assert(!inform_coalesce_emit(&prior, 100, 600, false));

	/* Oscillation within a bucket cannot defeat dominance: with 600 the
	 * running min, nothing at/below it re-emits, whatever the order. */
	auto const osc = InformObs{ 100, 600 };
	assert(!inform_coalesce_emit(&osc, 100, 500, true));
	assert(!inform_coalesce_emit(&osc, 100, 400, true));
	assert(!inform_coalesce_emit(&osc, 100, 600, true));
}

/* Test 6 (behavioural): inform_channel coalesces.  A second, dominated
 * write to the same dir within the same time bucket must NOT hit the
 * RPC.  Proven via a sentinel inform to a different dir: if the
 * dominated write leaked through, the second server read would see it
 * (and its dir assertion would fire) instead of the sentinel. */
Ev::Io<void>
test_coalesce_drops_dominated( MockRpcServer& server
			     , Boss::Mod::Rpc& rpc
			     ) {
	auto const layer = std::string("coalesce-layer");

	/* Pin the coalescing bucket astronomically large so both writes
	 * below always land in the same Ev::now()/window bucket regardless
	 * of when the test runs; otherwise a run straddling a bucket
	 * boundary re-emits the dominated write as a keep-alive and
	 * assert_sentinel fires spuriously.  This is the final test before
	 * Shutdown, so the global window needs no restore. */
	Boss::Mod::AskreneLayer::set_aging_window_secs(
		std::uint64_t(1000) * 365 * 24 * 60 * 60
	);

	auto assert_first = [](Jsmn::Object const& req) {
		auto id = assert_method(req, "askrene-inform-channel");
		auto params = req["params"];
		assert(std::string(params["short_channel_id_dir"]) == "400x4x0/0");
		assert(double(params["amount_msat"]) == 750000.0);
		return id;
	};
	auto assert_sentinel = [](Jsmn::Object const& req) {
		auto id = assert_method(req, "askrene-inform-channel");
		auto params = req["params"];
		/* A leaked dominated 400x4x0/0 write would show up here
		 * instead of the sentinel -> this assertion fires. */
		assert(std::string(params["short_channel_id_dir"]) == "500x5x0/0");
		assert(double(params["amount_msat"]) == 999.0);
		return id;
	};

	return Ev::lift().then([&server, assert_first]() {
		return Ev::concurrent(server.serve_ok(assert_first));
	}).then([&rpc, layer]() {
		/* First write: new dir+bucket -> emits, served by assert_first. */
		return Boss::Mod::AskreneLayer::inform_channel_unconstrained(
			rpc, layer, Ln::Scid("400x4x0"), std::uint32_t(0),
			Ln::Amount::msat(750000)
		);
	}).then([&server, assert_sentinel]() {
		/* Spawn the sentinel server BEFORE the dominated write, so a
		 * leaked write is caught by assert_sentinel rather than
		 * hanging the test. */
		return Ev::concurrent(server.serve_ok(assert_sentinel));
	}).then([&rpc, layer]() {
		/* Dominated (lower min, same bucket) -> must DROP, no RPC. */
		return Boss::Mod::AskreneLayer::inform_channel_unconstrained(
			rpc, layer, Ln::Scid("400x4x0"), std::uint32_t(0),
			Ln::Amount::msat(500000)
		);
	}).then([&rpc, layer]() {
		/* Sentinel: different dir -> emits, served by assert_sentinel. */
		return Boss::Mod::AskreneLayer::inform_channel_unconstrained(
			rpc, layer, Ln::Scid("500x5x0"), std::uint32_t(0),
			Ln::Amount::msat(999)
		);
	});
}

} // namespace

int main() {
	/* Pure-logic coalescing test first -- needs no Ev/RPC machinery. */
	test_coalesce_decision();

	auto bus = S::Bus();

	int sockets[2];
	auto sockres = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	assert(sockres >= 0);
	auto server_socket = Net::Fd(sockets[0]);
	auto client_socket = Net::Fd(sockets[1]);

	auto server = MockRpcServer(std::move(server_socket));
	auto rpc = Boss::Mod::Rpc(bus, std::move(client_socket));

	auto code = Ev::lift().then([&]() {
		return test_inform_channel_constrained(server, rpc);
	}).then([&]() {
		return test_inform_channel_unconstrained(server, rpc);
	}).then([&]() {
		return test_disable_node(server, rpc);
	}).then([&]() {
		return test_silent_rpc_error(server, rpc);
	}).then([&]() {
		return test_coalesce_drops_dominated(server, rpc);
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
