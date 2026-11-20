#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/NormalConnection.hpp"
#include"Boltz/Detail/node_auth_message.hpp"
#include"Boltz/EnvIF.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Util/make_unique.hpp"
#include<arpa/inet.h>
#include<assert.h>
#include<cstdint>
#include<map>
#include<memory>
#include<netinet/in.h>
#include<string>
#include<sys/socket.h>
#include<thread>
#include<unistd.h>

/* With a signer, every request carries our node ID, the timestamp
 * and the signature of exactly the method, path and body sent;
 * without one, none of those headers.  */

namespace {

struct Request {
	std::string method;
	std::string path;
	std::map<std::string, std::string> headers;
	std::string body;
};

/* Answers one HTTP/1.1 request on a loopback port with "{}" and
 * records it.  */
class RecordingServer {
private:
	int listen_fd;
	std::uint16_t port_;
	std::thread thread;

public:
	Request request;

	RecordingServer() {
		listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		assert(listen_fd >= 0);
		auto addr = sockaddr_in();
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		auto rc = bind(listen_fd, (sockaddr const*) &addr, sizeof(addr));
		assert(rc == 0);
		auto len = socklen_t(sizeof(addr));
		rc = getsockname(listen_fd, (sockaddr*) &addr, &len);
		assert(rc == 0);
		port_ = ntohs(addr.sin_port);
		rc = listen(listen_fd, 1);
		assert(rc == 0);
		thread = std::thread([this]() { serve(); });
	}
	~RecordingServer() {
		thread.join();
		close(listen_fd);
	}

	std::uint16_t port() const { return port_; }

private:
	void serve() {
		auto fd = accept(listen_fd, nullptr, nullptr);
		assert(fd >= 0);

		auto data = std::string();
		auto buf = std::string(4096, '\0');
		auto header_end = std::string::npos;
		while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
			auto n = read(fd, &buf[0], buf.size());
			assert(n > 0);
			data.append(buf, 0, n);
		}

		auto lines = data.substr(0, header_end);
		auto start = std::size_t(0);
		auto first = true;
		for (;;) {
			auto end = lines.find("\r\n", start);
			auto line = lines.substr(start, end - start);
			if (first) {
				auto sp1 = line.find(' ');
				auto sp2 = line.find(' ', sp1 + 1);
				request.method = line.substr(0, sp1);
				request.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
				first = false;
			} else {
				auto colon = line.find(':');
				auto name = line.substr(0, colon);
				for (auto& c : name)
					c = std::tolower(c);
				auto value = line.substr(colon + 1);
				while (!value.empty() && value[0] == ' ')
					value.erase(0, 1);
				request.headers[name] = value;
			}
			if (end == std::string::npos)
				break;
			start = end + 2;
		}

		auto length = std::size_t(0);
		if (request.headers.count("content-length"))
			length = std::stoul(request.headers["content-length"]);
		request.body = data.substr(header_end + 4);
		while (request.body.size() < length) {
			auto n = read(fd, &buf[0], buf.size());
			assert(n > 0);
			request.body.append(buf, 0, n);
		}

		auto response = std::string("HTTP/1.1 200 OK\r\n"
					    "Content-Type: application/json\r\n"
					    "Content-Length: 2\r\n"
					    "Connection: close\r\n"
					    "\r\n"
					    "{}");
		auto written = write(fd, response.data(), response.size());
		assert(written == ssize_t(response.size()));
		close(fd);
	}
};

class SigningEnv : public Boltz::EnvIF {
public:
	std::string signed_message;

	Ev::Io<Boltz::NodeSignature>
	sign_message(std::string message) override {
		signed_message = message;
		return Ev::lift(Boltz::NodeSignature{"02abcd", "zbase-signature"});
	}

	Ev::Io<std::uint32_t> get_feerate() override {
		return Ev::lift(std::uint32_t(1000));
	}
	Ev::Io<std::string> get_block(std::uint32_t) override {
		return Ev::lift(std::string());
	}
	Ev::Io<bool> has_confirmed_output( Bitcoin::TxId
					 , std::uint32_t
					 ) override {
		return Ev::lift(false);
	}
	Ev::Io<bool> broadcast_tx(Bitcoin::Tx) override {
		return Ev::lift(true);
	}
	Ev::Io<void> logd(std::string) override { return Ev::lift(); }
	Ev::Io<void> logi(std::string) override { return Ev::lift(); }
	Ev::Io<void> loge(std::string) override { return Ev::lift(); }
};

Ev::Io<Request> send( Ev::ThreadPool& threadpool
		    , Boltz::EnvIF* signer
		    , std::unique_ptr<Json::Out> params
		    ) {
	auto server = std::make_shared<RecordingServer>();
	auto conn = std::make_shared<Boltz::Detail::NormalConnection>(
		threadpool
	      , "http://127.0.0.1:" + std::to_string(server->port())
	      , ""
	      , 30
	      , signer
	);
	return conn->api("/v2/swap/reverse", std::move(params)
	).then([server, conn](Jsmn::Object) {
		return Ev::lift(server->request);
	});
}

}

int main() {
	Ev::ThreadPool threadpool;
	SigningEnv env;

	auto code = Ev::lift().then([&]() {
		auto params = Util::make_unique<Json::Out>(
			Json::Out()
				.start_object()
					.field("invoiceAmount", 100000)
				.end_object()
		);
		return send(threadpool, &env, std::move(params));
	}).then([&](Request req) {
		assert(req.method == "POST");
		assert(req.path == "/v2/swap/reverse");
		assert(req.headers["x-node-id"] == "02abcd");
		assert(req.headers["x-node-signature"] == "zbase-signature");
		auto ts = std::stoull(req.headers["x-node-timestamp"]);
		/* What was signed is what was sent.  */
		assert( env.signed_message
		     == Boltz::Detail::node_auth_message( ts, "POST"
							, "/v2/swap/reverse"
							, req.body
							)
		      );
		return send(threadpool, &env, nullptr);
	}).then([&](Request req) {
		assert(req.method == "GET");
		assert(req.headers.count("x-node-id") == 1);
		auto ts = std::stoull(req.headers["x-node-timestamp"]);
		assert( env.signed_message
		     == Boltz::Detail::node_auth_message( ts, "GET"
							, "/v2/swap/reverse"
							, ""
							)
		      );
		/* Without a signer nothing identifies our node.  */
		return send(threadpool, nullptr, nullptr);
	}).then([&](Request req) {
		assert(req.headers.count("x-node-id") == 0);
		assert(req.headers.count("x-node-timestamp") == 0);
		assert(req.headers.count("x-node-signature") == 0);
		return Ev::lift(0);
	});

	return Ev::start(code);
}
