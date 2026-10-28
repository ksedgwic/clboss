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
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Net/Connector.hpp"
#include"Net/Fd.hpp"
#include"Net/SocketFd.hpp"
#include"S/Bus.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include<assert.h>
#include<deque>
#include<errno.h>
#include<fcntl.h>
#include<memory>
#include<stdexcept>
#include<string>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>

namespace {

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
	Secp256k1::KeyPair
	get_keypair_tweak(Secp256k1::PrivKey const&) override {
		throw std::logic_error("Not expected to use.");
	}
};

class MockRpcServer {
private:
	Net::Fd socket;
	Jsmn::Parser parser;
	std::deque<Jsmn::Object> requests;
	std::shared_ptr<bool> pay_replied;

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
	MockRpcServer(Net::Fd socket_, std::shared_ptr<bool> pay_replied_) : socket(std::move(socket_)), parser(), requests(), pay_replied(std::move(pay_replied_)) {
		auto flags = fcntl(socket.get(), F_GETFL);
		assert(flags >= 0);
		flags |= O_NONBLOCK;
		auto fcntl_result = fcntl(socket.get(), F_SETFL, flags);
		assert(fcntl_result == 0);
	}

	/* Asserts that no request arrives for `rounds` yields.  */
	Ev::Io<void> assert_quiet(std::size_t rounds) {
		return Ev::yield().then([this, rounds]() {
			assert(requests.empty());
			char buf[512];
			auto rd = ssize_t();
			do {
				rd = read(socket.get(), buf, sizeof(buf));
			} while (rd < 0 && errno == EINTR);
			assert(rd < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
			if (rounds == 0)
				return Ev::lift();
			return assert_quiet(rounds - 1);
		});
	}

	Ev::Io<void> expect_decode(std::string const& invoice) {
		return read_request().then([this, invoice](Jsmn::Object req) {
			auto id = assert_method(req, "decode");
			auto params = req["params"];
			assert(params.is_object());
			assert(params.has("string"));
			assert(std::string(params["string"]) == invoice);
			/* The same decode result for every invoice:
			 * payment_hash 7814...09a3, 1,000,000 msat.  */
			return reply_result(id, R"({
			   "type": "bolt11 invoice",
			   "currency": "tb",
			   "created_at": 1771010577,
			   "expiry": 604800,
			   "payee": "0225bbc2a7341993cd592d7b0c185bb8c6359cc1dd1337975c6d41354e4703bf64",
			   "amount_msat": 1000000,
			   "description": "decode testing",
			   "min_final_cltv_expiry": 10,
			   "payment_secret": "d8577cf3c01f0b9b124adee87f552c2b3195db83f4dea30874d5b27d26201e85",
			   "features": "02024100",
			   "routes": [
			      [
			         {
			            "pubkey": "031c64a68e6d1b9e50711336d92b434c584ce668b2fae59ee688bd73713fee1569",
			            "short_channel_id": "4659673x21x0",
			            "fee_base_msat": 2000,
			            "fee_proportional_millionths": 2,
			            "cltv_expiry_delta": 80
			         }
			      ]
			   ],
			   "payment_hash": "7814817188071aec26c943f4864ef150aaff45def81b36b0dd4bc6ce8f1809a3",
			   "signature": "3045022100e745b9b7fe8133c7385e40561217e4717f7a2868c60d794b160047512c8d3a79022074619d6d2ee5c07b3099ca3684f896886aab04854bfade8f5a0f9014d5418ab6",
			   "valid": true
			})");
		});
	}

	Ev::Io<void> expect_xpay(std::string const& invoice) {
		return read_request().then([this, invoice](Jsmn::Object req) {
			auto id = assert_method(req, "xpay");
			auto params = req["params"];
			assert(params.is_object());
			assert(params.has("invstring"));
			assert(std::string(params["invstring"]) == invoice);
			assert(params.has("retry_for"));
			assert(params["retry_for"].is_number());
			assert(double(params["retry_for"]) == 1000.0);
			assert(params.has("maxfee"));
			assert(params["maxfee"].is_number());
			/* 1% of the 1,000,000 msat invoice above;
			 * the 5000 msat floor does not bind here.  */
			assert(double(params["maxfee"]) == 10000.0);
			return reply_result(id, "{}");
		});
	}

	/* The invoices are raised in this order.  The two whose
	 * expectations do not match the decode result get no
	 * `xpay`; the matching one is paid (#329).  */
	Ev::Io<void> run( std::string const& wrong_hash
			, std::string const& wrong_amount
			, std::string const& good
			) {
		return expect_decode(wrong_hash).then([this, wrong_amount]() {
			return expect_decode(wrong_amount);
		}).then([this, good]() {
			return expect_decode(good);
		}).then([this, good]() {
			return expect_xpay(good);
		}).then([this]() {
			/* Nor does an `xpay` for a rejected invoice
			 * arrive late.  */
			return assert_quiet(200);
		}).then([this]() {
			*pay_replied = true;
			return Ev::lift();
		});
	}
};

Ev::Io<void> wait_for_pay_reply(std::shared_ptr<bool> pay_replied,
				std::size_t retries = 0) {
	return Ev::lift().then([pay_replied, retries]() {
		if (*pay_replied)
			return Ev::lift();
		assert(retries < 100000);
		return Ev::yield().then([pay_replied, retries]() {
			return wait_for_pay_reply(pay_replied, retries + 1);
		});
	});
}

Ev::Io<void> yield_n(std::size_t n) {
	if (n == 0)
		return Ev::lift();
	return Ev::yield().then([n]() {
		return yield_n(n - 1);
	});
}

} // namespace

int main() {
	auto bus = S::Bus();
	auto payer = Boss::Mod::InvoicePayer(bus);

	/* What the mock's decode result carries.  */
	auto const decoded_hash = Sha256::Hash(
		"7814817188071aec26c943f4864ef150aaff45def81b36b0dd4bc6ce8f1809a3"
	);
	auto const decoded_amount = Ln::Amount::msat(1000000);
	auto const other_hash = Sha256::Hash(
		"0000000000000000000000000000000000000000000000000000000000000001"
	);
	auto const wrong_hash_invoice = std::string("lnbc1wronghash");
	auto const wrong_amount_invoice = std::string("lnbc1wrongamount");
	auto const good_invoice = std::string("lnbc1qtestinvoice");

	auto connector = DummyConnector();
	auto signer = DummySigner();
	auto db = Sqlite3::Db(":memory:");
	auto pay_replied = std::make_shared<bool>(false);

	int sockets[2];
	auto sockres = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	assert(sockres >= 0);
	auto server_socket = Net::Fd(sockets[0]);
	auto client_socket = Net::Fd(sockets[1]);

	auto server = MockRpcServer(std::move(server_socket), pay_replied);
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
			wrong_hash_invoice, other_hash, decoded_amount
		});
	}).then([&]() {
		return yield_n(100);
	}).then([&]() {
		return bus.raise(Boss::Msg::PayInvoice{
			wrong_amount_invoice, decoded_hash,
			Ln::Amount::msat(999000)
		});
	}).then([&]() {
		return yield_n(100);
	}).then([&]() {
		return bus.raise(Boss::Msg::PayInvoice{
			good_invoice, decoded_hash, decoded_amount
		});
	});

	auto code = Ev::lift().then([&]() {
		return Ev::concurrent(server.run( wrong_hash_invoice
						, wrong_amount_invoice
						, good_invoice
						));
	}).then([&]() {
		return Ev::concurrent(client_code);
	}).then([&]() {
		return wait_for_pay_reply(pay_replied);
	}).then([&]() {
		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(*pay_replied);
	assert(ec == 0);
	return 0;
}
