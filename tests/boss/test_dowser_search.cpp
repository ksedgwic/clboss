#undef NDEBUG
#include"Boss/Mod/Dowser.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/Network.hpp"
#include"Boss/Msg/RequestDowser.hpp"
#include"Boss/Msg/ResponseDowser.hpp"
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
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include<assert.h>
#include<cstdint>
#include<fcntl.h>
#include<sstream>
#include<stdexcept>
#include<string>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>
#include<utility>
#include<vector>

/* Drives the Dowser binary search over a real Boss::Mod::Rpc on a
 * socketpair against a scripted getroutes server, covering the paths
 * the unit tests on floor_probe_amount cannot:
 *
 *  - mid-range capacity: the probe at the ceiling fails, the floor
 *    probe succeeds, and refine() converges on the expected amount;
 *  - full-capacity fast path: a single probe answers;
 *  - genuine no flow: ceiling and floor both fail, amount 0;
 *  - unexpected RpcError: the narrowed catch aborts the search (no
 *    floor probe after a failing ceiling) and the dowse reports 0
 *    after logging the failure -- instead of misreading the error
 *    as "no flow at this amount".
 */

namespace {

auto const self_id = std::string("02")
	+ "0000000000000000000000000000000000000000000000000000000000000001";
auto const from_id = std::string("02")
	+ "0000000000000000000000000000000000000000000000000000000000000002";
auto const to_id = std::string("02")
	+ "0000000000000000000000000000000000000000000000000000000000000003";

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

/* A scripted JSON-RPC server for the socketpair's other end.
 * Requests are newline-delimited (Boss::Mod::Rpc appends "\n\n").
 * getroutes is answered from the configured scenario:
 *  - capacity mode: amount > capacity -> error -205 (the askrene
 *    "no flow at this amount" answer); otherwise a single route
 *    delivering the full requested amount;
 *  - error mode: every getroutes fails with the given code.  */
class GetroutesServer {
private:
	Net::Fd socket;
	std::string buffer;

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

	Ev::Io<void> send_result(std::uint64_t id, Json::Out result) {
		return writeloop(Json::Out()
			.start_object()
				.field("jsonrpc", std::string("2.0"))
				.field("id", std::uint64_t(id))
				.field("result", std::move(result))
			.end_object()
			.output() + "\n"
		);
	}

	Ev::Io<void> send_error( std::uint64_t id
			       , std::int64_t code
			       , std::string message
			       ) {
		return writeloop(Json::Out()
			.start_object()
				.field("jsonrpc", std::string("2.0"))
				.field("id", std::uint64_t(id))
				.start_object("error")
					.field("code", code)
					.field("message", std::move(message))
				.end_object()
			.end_object()
			.output() + "\n"
		);
	}

	/* Build the response for one parsed request.  */
	Ev::Io<void> answer( std::string const& method
			   , std::uint64_t id
			   , Jsmn::Object const& params
			   ) {
		if (method == "askrene-create-layer"
		 || method == "askrene-disable-node") {
			return send_result(id, Json::Out::empty_object());
		}
		if (method == "askrene-listlayers") {
			return send_result(id, Json::Out()
				.start_object()
					.start_array("layers")
						.entry(Json::Out()
							.start_object()
							.end_object())
					.end_array()
				.end_object());
		}
		assert(method == "getroutes");
		++getroutes_calls;
		auto amount_msat = std::uint64_t(0);
		if (params.is_object() && params.has("amount_msat")
		 && params["amount_msat"].is_number())
			amount_msat = std::uint64_t(
				double(params["amount_msat"])
			);
		getroutes_amounts.push_back(amount_msat);
		if (error_code != 0)
			return send_error( id, error_code
					  , "Unexpected error for the test"
					  );
		if (amount_msat > capacity_msat)
			return send_error( id, -205
					  , "Unable to find a route"
					  );
		/* Partial-delivery mode: a successful probe delivers only
		 * a fraction of the requested amount (askrene under an
		 * effective maxparts/liquidity limit behaves this way).  */
		auto delivered_msat = amount_msat * delivery_num
				    / delivery_den
				    ;
		return send_result(id, Json::Out()
			.start_object()
				.start_array("routes")
					.start_object()
						.field( "amount_msat"
						      , delivered_msat
						      )
					.end_object()
				.end_array()
			.end_object());
	}

public:
	std::uint64_t capacity_msat;
	/* Nonzero: every getroutes fails with this code instead of
	 * the capacity rule.  */
	std::int64_t error_code;
	/* Successful probes deliver amount * delivery_num /
	 * delivery_den (10/10 = full delivery).  */
	std::uint64_t delivery_num;
	std::uint64_t delivery_den;
	std::size_t getroutes_calls;
	std::vector<std::uint64_t> getroutes_amounts;

	explicit
	GetroutesServer(Net::Fd socket_)
		: socket(std::move(socket_))
		, capacity_msat(0)
		, error_code(0)
		, delivery_num(10)
		, delivery_den(10)
		, getroutes_calls(0)
		, getroutes_amounts()
	{
		auto flags = fcntl(socket.get(), F_GETFL);
		flags |= O_NONBLOCK;
		fcntl(socket.get(), F_SETFL, flags);
	}
	GetroutesServer(GetroutesServer&&) =delete;

	/* One pump pass: yield, slurp whatever arrived, answer every
	 * complete request line.  Requests issued by the Dowser are
	 * strictly sequential, so nothing is left unanswered between
	 * passes.  */
	Ev::Io<void> pump() {
		return Ev::yield().then([this]() {
			char buf[4096];
			for (;;) {
				auto res = read( socket.get()
					       , buf, sizeof(buf)
					       );
				if (res < 0 && ( errno == EWOULDBLOCK
					      || errno == EAGAIN
					       ))
					break;
				assert(res >= 0);
				buffer.append(buf, size_t(res));
			}
			auto act = Ev::lift();
			for (;;) {
				auto pos = buffer.find('\n');
				if (pos == std::string::npos)
					break;
				auto line = buffer.substr(0, pos);
				buffer.erase(0, pos + 1);
				if (line.empty())
					continue;
				auto jreq = Jsmn::Object();
				auto sj = std::istringstream(line);
				sj >> jreq;
				assert(jreq.is_object());
				assert(jreq.has("id"));
				assert(jreq.has("method"));
				auto id = std::uint64_t(
					double(jreq["id"])
				);
				auto method = std::string(jreq["method"]);
				auto params = jreq.has("params")
					    ? jreq["params"]
					    : Jsmn::Object()
					    ;
				act += answer(method, id, params);
			}
			return act;
		});
	}

	Ev::Io<void> serve(std::size_t passes) {
		if (passes == 0)
			return Ev::lift();
		return pump().then([this, passes]() {
			return serve(passes - 1);
		});
	}
};

}

int main() {
	using La = Ln::Amount;

	S::Bus bus;
	auto dowser = Boss::Mod::Dowser(bus);
	Boss::ModG::ReqResp< Boss::Msg::RequestDowser
			   , Boss::Msg::ResponseDowser
			   > dowser_rr(bus);

	/* Boss::log raises JsonCout notifications; capture them so the
	 * narrowed-Error vs legacy-no-flow log discrimination can be
	 * asserted.  */
	auto log_lines = std::make_shared<std::vector<std::string>>();
	bus.subscribe<Boss::Msg::JsonCout
		     >([log_lines](Boss::Msg::JsonCout const& m) {
		log_lines->push_back(m.obj.output());
		return Ev::lift();
	});

	int sockets[2];
	auto res = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	assert(res >= 0);
	auto server_socket = Net::Fd(sockets[0]);
	auto client_socket = Net::Fd(sockets[1]);
	auto server = GetroutesServer(std::move(server_socket));
	auto rpc = Boss::Mod::Rpc(bus, std::move(client_socket));

	auto connector = DummyConnector();
	auto signer = DummySigner();
	auto db = Sqlite3::Db(":memory:");

	/* Mirror of the Dowser's probe sizing, so the test can
	 * compute the probe amounts it expects to see.  */
	auto probe_amount_for = [](Ln::Amount probe_target) {
		return probe_target * (1.0 / 0.985) + La::sat(1);
	};

	/* Mirror of the search: the same bisection refine() runs,
	 * against the same capacity rule the fake server applies,
	 * computing the delivered amount the Dowser must report
	 * (before the reserve haircut).  delivery_num/delivery_den
	 * mirror the server's partial-delivery fraction.  */
	auto expected_delivered = []( La probe_amount
				     , La capacity
				     , std::uint64_t dnum
				     , std::uint64_t dden
				     ) {
		auto resolution = La::msat(
			probe_amount.to_msat() / 32
		);
		auto lo = Boss::Mod::Dowser::floor_probe_amount(
			probe_amount, La::sat(0)
		);
		auto hi = probe_amount;
		assert(lo <= capacity);
		auto delivered = La::msat(
			lo.to_msat() * dnum / dden
		);
		while (hi > lo && (hi - lo) > resolution) {
			auto mid = La::msat(
				(lo.to_msat() + hi.to_msat()) / 2
			);
			if (mid <= capacity) {
				lo = mid;
				delivered = La::msat(
					mid.to_msat() * dnum / dden
				);
			} else
				hi = mid;
		}
		return delivered;
	};

	auto dowse = [&](Ln::Amount probe_target) {
		return dowser_rr.execute(Boss::Msg::RequestDowser{
			nullptr,
			Ln::NodeId(from_id),
			Ln::NodeId(to_id),
			probe_target,
			La::sat(0)
		});
	};

	/* The server pumps in the background while each dowse
	 * executes; the Dowser issues requests strictly
	 * sequentially.  */
	auto serve_bg = [&]() {
		return Ev::concurrent(server.serve(5000));
	};

	auto probe_target = La::sat(16000000);
	auto probe_amount = probe_amount_for(probe_target);

	auto test = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Regtest,
			rpc,
			Ln::NodeId(self_id),
			db,
			connector,
			signer,
			std::string(),
			false,
			false
		});
	}).then([&]() {
		return serve_bg();
	}).then([&]() {
		/* Mid-range capacity: the ceiling probe exceeds it,
		 * the floor does not, and the search converges.  */
		server.capacity_msat = 600000000;
		server.error_code = 0;
		return dowse(probe_target);
	}).then([&](Boss::Msg::ResponseDowser r) {
		/* The probe at the ceiling failed first...  */
		assert(server.getroutes_calls >= 2);
		assert(server.getroutes_amounts[0] == probe_amount.to_msat());
		/* ...the floor probe succeeded...  */
		auto floor_amt = Boss::Mod::Dowser::floor_probe_amount(
			probe_amount, La::sat(0)
		);
		assert(server.getroutes_amounts[1] == floor_amt.to_msat());
		/* ...and refine() probes followed: the binary-search
		 * path really ran.  */
		assert(server.getroutes_calls > 2);
		/* Convergence: the reported amount is the delivered
		 * flow at the last successful bracket, less the
		 * reserve haircut.  */
		auto expected = expected_delivered(
			probe_amount, La::msat(600000000), 10, 10
		) * 0.985;
		assert(r.amount == expected);

		/* Full-capacity fast path: a single probe answers.  */
		server.capacity_msat = probe_amount.to_msat();
		server.getroutes_calls = 0;
		server.getroutes_amounts.clear();
		return dowse(probe_target);
	}).then([&](Boss::Msg::ResponseDowser r) {
		assert(server.getroutes_calls == 1);
		assert(r.amount == probe_amount * 0.985);

		/* Genuine no flow: ceiling and floor both fail, no
		 * refine probes.  */
		server.capacity_msat = 0;
		server.getroutes_calls = 0;
		server.getroutes_amounts.clear();
		return dowse(probe_target);
	}).then([&](Boss::Msg::ResponseDowser r) {
		assert(server.getroutes_calls == 2);
		assert(r.amount == La::sat(0));

		/* Unexpected RpcError: the narrowed catch aborts the
		 * search -- the ceiling probe fails with a non-
		 * no-flow code and NO floor probe is issued -- and
		 * the dowse reports 0 (after logging the failure)
		 * rather than fabricating a no-flow answer inside
		 * the search.  */
		server.error_code = -1;
		server.getroutes_calls = 0;
		server.getroutes_amounts.clear();
		return dowse(probe_target);
	}).then([&](Boss::Msg::ResponseDowser r) {
		assert(server.getroutes_calls == 1);
		assert(r.amount == La::sat(0));

		/* Partial delivery: a successful probe delivers only a
		 * fraction of the requested amount; the search still
		 * converges, and the report carries the fraction.  */
		server.error_code = 0;
		server.capacity_msat = 600000000;
		server.delivery_num = 6;
		server.delivery_den = 10;
		server.getroutes_calls = 0;
		server.getroutes_amounts.clear();
		return dowse(probe_target);
	}).then([&](Boss::Msg::ResponseDowser r) {
		assert(server.getroutes_calls > 2);
		auto expected = expected_delivered(
			probe_amount, La::msat(600000000), 6, 10
		) * 0.985;
		assert(r.amount == expected);

		/* -206 is the other expected no-flow code: same
		 * treatment as -205 (floor probe follows, then the
		 * no-flow report).  */
		server.error_code = -206;
		server.capacity_msat = 0;
		server.delivery_num = 10;
		server.delivery_den = 10;
		server.getroutes_calls = 0;
		server.getroutes_amounts.clear();
		return dowse(probe_target);
	}).then([&](Boss::Msg::ResponseDowser r) {
		assert(server.getroutes_calls == 2);
		assert(r.amount == La::sat(0));

		/* Log discrimination: exactly one narrowed Error line
		 * (the -1 abort in the earlier scenario) and at least
		 * two legacy no-flow Debug lines (the -205 and -206
		 * cells) crossed the bus.  */
		auto narrowed = size_t(0);
		auto noflow = size_t(0);
		for (auto const& line : *log_lines) {
			if (line.find("reporting 0 for this dowse")
			     != std::string::npos
			 && line.find("\"level\": \"error\"")
			     != std::string::npos)
				++narrowed;
			if (line.find("found no flow")
			     != std::string::npos)
				++noflow;
		}
		assert(narrowed == 1);
		assert(noflow >= 2);

		/* Stop the Rpc module's socket watcher (and unblock
		 * anything still pending) so the event loop can
		 * exit.  */
		return bus.raise(Boss::Shutdown()).then([] {
			return Ev::lift(0);
		});
	});

	return Ev::start(test);
}
