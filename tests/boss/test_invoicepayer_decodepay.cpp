#undef NDEBUG
#include"Boss/Mod/InvoicePayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/PayInvoice.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"Net/Connector.hpp"
#include"Net/Fd.hpp"
#include"Net/SocketFd.hpp"
#include"S/Bus.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include<assert.h>
#include<ctime>
#include<deque>
#include<errno.h>
#include<fcntl.h>
#include<memory>
#include<string>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>

namespace {

/* The payment_hash and amount_msat baked into the canonical decode
 * response below.  Tests that exercise the verification logic vary
 * these values to trigger acceptance or rejection.
 */
auto const GOOD_HASH = std::string(
	"7814817188071aec26c943f4864ef150aaff45def81b36b0dd4bc6ce8f1809a3"
);
auto const GOOD_AMOUNT = std::uint64_t(1000000);

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
	get_pubkey_tweak( Secp256k1::PrivKey const& tweak
			     ) override {
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
	get_privkey_salted_hash( std::uint8_t salt[32]
			       ) override {
		/* Not expected to be used in this unit test.  */
		if (!salt)
			return Sha256::Hash();
		auto hash = Sha256::Hash();
		hash.from_buffer(salt);
		return hash;
	}
};

/* Build a decode-response JSON string with configurable payment_hash,
 * amount_msat, created_at timestamp, and expiry.  The rest of the
 * fields mirror a real CLN decode result.
 */
std::string make_decode_response( std::string const& payment_hash
				, std::uint64_t amount_msat
				, double created_at
				, double expiry
				) {
	auto os = std::ostringstream();
	os << R"({
	   "type": "bolt11 invoice",
	   "currency": "tb",
	   "created_at": )"
	   << created_at << R"(,
	   "expiry": )"
	   << expiry << R"(,
	   "payee": "0225bbc2a7341993cd592d7b0c185bb8c6359cc1dd1337975c6d41354e4703bf64",
	   "amount_msat": )"
	   << amount_msat << R"(,
	   "description": "decode testing",
	   "min_final_cltv_expiry": 10,
	   "payment_secret": "d8577cf3c01f0b9b124adee87f552c2b3195db83f4dea30874d5b27d26201e85",
	   "features": "02024100",
	   "routes": [],
	   "payment_hash": ")"
	   << payment_hash << R"(",
	   "signature": "3045022100e745b9b7fe8133c7385e40561217e4717f7a2868c60d794b160047512c8d3a79022074619d6d2ee5c07b3099ca3684f896886aab04854bfade8f5a0f9014d5418ab6",
	   "valid": true
	})";
	return os.str();
}

class MockRpcServer {
private:
	Net::Fd socket;
	Jsmn::Parser parser;
	std::deque<Jsmn::Object> requests;
	std::shared_ptr<bool> pay_replied;
	std::string decode_response;
	bool expect_pay;

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

	static std::uint64_t assert_method(Jsmn::Object const& req
		                              , char const* method
		                      ) {
		assert(req.is_object());
		assert(req.has("id"));
		assert(req["id"].is_number());
		assert(req.has("method"));
		assert(req["method"].is_string());
		assert(std::string(req["method"]) == method);
		return std::uint64_t(double(req["id"]));
	}

	Ev::Io<void> reply_result(std::uint64_t id, std::string const& result) {
		auto response = std::string();
		response = Json::Out()
			.start_object()
				.field("jsonrpc", std::string("2.0"))
				.field("id", double(id))
				.field("result", Jsmn::Object::parse_json(result.c_str()))
			.end_object()
			.output();
		return write_all(std::move(response));
	}

public:
	MockRpcServer( Net::Fd socket_
		     , std::shared_ptr<bool> pay_replied_
		     , std::string decode_response_
		     , bool expect_pay_
		     ) : socket(std::move(socket_))
		       , parser()
		       , requests()
		       , pay_replied(std::move(pay_replied_))
		       , decode_response(std::move(decode_response_))
		       , expect_pay(expect_pay_)
	{
		auto flags = fcntl(socket.get(), F_GETFL);
		assert(flags >= 0);
		flags |= O_NONBLOCK;
		auto fcntl_result = fcntl(socket.get(), F_SETFL, flags);
		assert(fcntl_result == 0);
	}

	Ev::Io<void> run(std::string const& invoice) {
		return read_request().then([this, invoice](Jsmn::Object req) {
			auto id = assert_method(req, "decode");
			auto params = req["params"];
			assert(params.is_object());
			assert(params.has("string"));
			assert(std::string(params["string"]) == invoice);
			return reply_result(id, decode_response);
		}).then([this, invoice]() {
			if (!expect_pay)
				/* Payer should reject the invoice; no pay
				 * request will arrive.  */
				return Ev::lift();
			return read_request().then([this, invoice](Jsmn::Object req) {
				auto id = assert_method(req, "pay");
				auto params = req["params"];
				assert(params.is_object());
				assert(params.has("bolt11"));
				assert(std::string(params["bolt11"]) == invoice);
				assert(params.has("retry_for"));
				assert(params["retry_for"].is_number());
				assert(double(params["retry_for"]) == 1000.0);
				assert(params.has("maxfeepercent"));
				assert(params["maxfeepercent"].is_number());
				assert(double(params["maxfeepercent"]) == 5.0);
				return reply_result(id, "{}");
			}).then([this]() {
				*pay_replied = true;
				return Ev::lift();
			});
		});
	}
};

Ev::Io<void> wait_for(std::shared_ptr<bool> flag,
		      std::size_t retries = 0) {
	return Ev::lift().then([flag, retries]() {
		if (*flag)
			return Ev::lift();
		assert(retries < 100000);
		return Ev::yield().then([flag, retries]() {
			return wait_for(flag, retries + 1);
		});
	});
}

/* Yield a bounded number of times to let async tasks settle, without
 * requiring a specific flag.  Used for rejection scenarios where the
 * payer throws internally and no pay_replied flag is ever set.
 */
Ev::Io<void> settle(std::size_t remaining = 200) {
	return Ev::yield().then([remaining]() {
		if (remaining == 0)
			return Ev::lift();
		return settle(remaining - 1);
	});
}

struct Scenario {
	char const* label;
	std::string expected_hash;       /* PayInvoice.expected_payment_hash  */
	std::uint64_t expected_amount;   /* PayInvoice.expected_amount_msat   */
	std::string decode_hash;         /* decode response payment_hash      */
	std::uint64_t decode_amount;     /* decode response amount_msat       */
	double decode_created_at;
	double decode_expiry;
	bool expect_pay;
};

int run_scenario(Scenario const& sc) {
	auto bus = S::Bus();
	auto payer = Boss::Mod::InvoicePayer(bus);

	auto const invoice = std::string("lnbc1qtestinvoice");
	auto connector = DummyConnector();
	auto signer = DummySigner();
	auto db = Sqlite3::Db(":memory:");
	auto pay_replied = std::make_shared<bool>(false);

	int sockets[2];
	auto sockres = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	assert(sockres >= 0);
	auto server_socket = Net::Fd(sockets[0]);
	auto client_socket = Net::Fd(sockets[1]);

	auto decode_response = make_decode_response(
		sc.decode_hash, sc.decode_amount,
		sc.decode_created_at, sc.decode_expiry
	);
	auto server = MockRpcServer( std::move(server_socket)
				   , pay_replied
				   , decode_response
				   , sc.expect_pay
				   );
	auto rpc = Boss::Mod::Rpc(bus, std::move(client_socket));

	auto client_code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Regtest,
			rpc,
			Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000"),
			db,
			connector,
			signer,
			std::string(),
			false
		});
	}).then([&]() {
		return bus.raise(Boss::Msg::PayInvoice{
			invoice, sc.expected_hash, sc.expected_amount
		});
	});

	auto code = Ev::lift().then([&]() {
		return Ev::concurrent(server.run(invoice));
	}).then([&]() {
		return Ev::concurrent(client_code);
	}).then([&]() {
		if (sc.expect_pay)
			return wait_for(pay_replied);
		return settle();
	}).then([&]() {
		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);

	if (sc.expect_pay) {
		assert(*pay_replied);
	} else {
		assert(!*pay_replied);
	}
	return 0;
}

} // namespace

int main() {
	auto const now = double(std::time(nullptr));

	/* 1. No expected hash set — backward-compat happy path.  */
	run_scenario({ "no-expected-hash (backward compat)"
		      , ""              /* expected_hash   */
		      , 0               /* expected_amount */
		      , GOOD_HASH       /* decode_hash     */
		      , GOOD_AMOUNT     /* decode_amount   */
		      , now             /* created_at      */
		      , 604800          /* expiry          */
		      , true            /* expect_pay      */
	});

	/* 2. Matching payment_hash — pay succeeds.  */
	run_scenario({ "matching payment_hash"
		      , GOOD_HASH
		      , 0
		      , GOOD_HASH
		      , GOOD_AMOUNT
		      , now
		      , 604800
		      , true
	});

	/* 3. Mismatched payment_hash — pay refused.  */
	run_scenario({ "mismatched payment_hash"
		      , std::string(64, '0')   /* wrong hash */
		      , 0
		      , GOOD_HASH
		      , GOOD_AMOUNT
		      , now
		      , 604800
		      , false                   /* expect_pay */
	});

	/* 4. Matching amount — pay succeeds.  */
	run_scenario({ "matching amount_msat"
		      , GOOD_HASH
		      , GOOD_AMOUNT
		      , GOOD_HASH
		      , GOOD_AMOUNT
		      , now
		      , 604800
		      , true
	});

	/* 5. Mismatched amount — pay refused.  */
	run_scenario({ "mismatched amount_msat"
		      , ""
		      , GOOD_AMOUNT
		      , GOOD_HASH
		      , GOOD_AMOUNT + 1     /* different amount */
		      , now
		      , 604800
		      , false
	});

	/* 6. Expired invoice — pay refused.
	 * created_at + expiry far in the past.
	 */
	run_scenario({ "expired invoice"
		      , GOOD_HASH
		      , 0
		      , GOOD_HASH
		      , GOOD_AMOUNT
		      , 1.0             /* created_at = epoch         */
		      , 1.0             /* expiry = 1 second          */
		      , false
	});

	/* 7. Non-expired invoice with expected_hash — pay succeeds.  */
	run_scenario({ "non-expired invoice"
		      , GOOD_HASH
		      , 0
		      , GOOD_HASH
		      , GOOD_AMOUNT
		      , now
		      , 604800
		      , true
	});

	return 0;
}
