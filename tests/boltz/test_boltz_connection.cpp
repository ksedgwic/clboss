#undef NDEBUG
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/NormalConnection.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<arpa/inet.h>
#include<errno.h>
#include<future>
#include<netinet/in.h>
#include<string>
#include<string.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<thread>
#include<unistd.h>

namespace {

class HttpServer {
private:
	int listen_fd;
	int port;
	std::string response_body;
	std::string response_status;
	int response_code;
	std::thread thread;
	std::promise<int> port_promise;
	bool port_notified;

	static
	void write_all(int fd, std::string const& data) {
		auto written = std::size_t(0);
		while (written < data.size()) {
			auto wrote = ::write(fd, data.data() + written
				      , data.size() - written
			      );
			if (wrote < 0) {
				if (errno == EINTR)
					continue;
				throw std::runtime_error(std::string("write: ")
						      + strerror(errno));
			}
			written += std::size_t(wrote);
		}
	}

	void notify_port() {
		if (!port_notified) {
			port_notified = true;
			port_promise.set_value(port);
		}
	}
	void notify_port_error(std::exception_ptr err) {
		if (!port_notified) {
			port_notified = true;
			port_promise.set_exception(err);
		}
	}

	void run() {
		int client_fd = -1;
		try {
			listen_fd = socket(AF_INET, SOCK_STREAM, 0);
			if (listen_fd < 0)
				throw std::runtime_error(std::string("socket: ")
						      + strerror(errno));

			auto on = int(1);
			if (setsockopt( listen_fd, SOL_SOCKET, SO_REUSEADDR
				      , &on, sizeof(on)) < 0) {
				throw std::runtime_error(std::string("setsockopt: ")
						      + strerror(errno));
			}

			auto addr = sockaddr_in();
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr.sin_port = htons(0);
			if (bind(listen_fd, (sockaddr*) &addr, sizeof(addr)) < 0)
				throw std::runtime_error(std::string("bind: ")
						      + strerror(errno));
			if (listen(listen_fd, 1) < 0)
				throw std::runtime_error(std::string("listen: ")
						      + strerror(errno));

			socklen_t len = sizeof(addr);
			if (getsockname( listen_fd, (sockaddr*) &addr, &len) < 0)
				throw std::runtime_error(std::string("getsockname: ")
						      + strerror(errno));
			port = ntohs(addr.sin_port);
			notify_port();

			client_fd = accept(listen_fd, nullptr, nullptr);
			if (client_fd < 0)
				throw std::runtime_error(std::string("accept: ")
						      + strerror(errno));

			/* Consume request headers only; body is not needed for this test.  */
			auto req = std::string();
			char buf[512];
			for (;;) {
				auto readed = read(client_fd, buf, sizeof(buf));
				if (readed < 0) {
					if (errno == EINTR)
						continue;
					throw std::runtime_error(std::string("read: ")
							      + strerror(errno));
				}
				if (readed == 0)
					break;
				req.append(buf, buf + readed);
				if (req.find("\r\n\r\n") != std::string::npos
				    || req.find("\n\n") != std::string::npos)
					break;
			}

			auto header = std::string("HTTP/1.1 ")
					+ std::to_string(response_code)
					+ " "
					+ response_status
					+ "\r\n"
					+ "Content-Type: application/json\r\n"
					+ "Content-Length: "
					+ std::to_string(response_body.size())
					+ "\r\n"
					+ "Connection: close\r\n"
					+ "\r\n"
					+ response_body;
			write_all(client_fd, header);

			notify_port();
		} catch (...) {
			notify_port_error(std::current_exception());
		}
		if (client_fd >= 0)
			close(client_fd);
		if (listen_fd >= 0)
			close(listen_fd);
	}

public:
	HttpServer( std::string body
		  , int code = 200
		  , std::string status = "OK"
		  ) : listen_fd(-1)
		    , port(0)
		    , response_body(std::move(body))
		    , response_status(std::move(status))
		    , response_code(code)
		    , port_notified(false) {
		thread = std::thread([this]() { run(); });
	}
	~HttpServer() {
		if (thread.joinable())
			thread.join();
	}

	int get_port() {
		if (port == 0) {
			auto future = port_promise.get_future();
			port = future.get();
		}
		return port;
	}
};

void expect_success() {
	auto server = HttpServer(std::string("{\"ok\": true}"));
	auto port = server.get_port();

	auto tp = Ev::ThreadPool();
	auto conn = Boltz::Detail::NormalConnection(
		tp,
		std::string("http://127.0.0.1:")
		+ std::to_string(port)
		+ "/api"
	);
	auto code = conn.api("/good", Util::make_unique<Json::Out>(Json::Out::empty_object())
		).then([](Jsmn::Object result) {
			assert(result.has("ok"));
			return Ev::lift(0);
		});
	assert(Ev::start(std::move(code)) == 0);
}

void expect_invalid_json() {
	auto server = HttpServer(std::string("{'ok': 1}"));
	auto port = server.get_port();
	auto base = std::string("http://127.0.0.1:")
		     + std::to_string(port)
		     + "/api";

	auto tp = Ev::ThreadPool();
	auto conn = Boltz::Detail::NormalConnection(tp, base);
	auto caught = false;
	auto code = conn.api("/bad-json", Util::make_unique<Json::Out>(Json::Out::empty_object())
	).then([](Jsmn::Object result) {
			assert(!"should have failed");
			return Ev::lift(0);
		}).catching< Boltz::ApiError >([&](Boltz::ApiError const& e) {
			auto msg = std::string(e.what());
			assert(msg.find("Invalid JSON") != std::string::npos);
			assert(msg.find("/bad-json") != std::string::npos);
			caught = true;
			return Ev::lift(0);
		});
	assert(Ev::start(std::move(code)) == 0);
	assert(caught);
}

void expect_no_json_result() {
	auto server = HttpServer(std::string());
	auto port = server.get_port();
	auto base = std::string("http://127.0.0.1:")
		     + std::to_string(port)
		     + "/api";

	auto tp = Ev::ThreadPool();
	auto conn = Boltz::Detail::NormalConnection(tp, base);
	auto caught = false;
	auto code = conn.api("/no-result", Util::make_unique<Json::Out>(Json::Out::empty_object())
	).then([](Jsmn::Object result) {
			assert(!"should have failed");
			return Ev::lift(0);
		}).catching< Boltz::ApiError >([&](Boltz::ApiError const& e) {
			auto msg = std::string(e.what());
			assert(msg.find("No JSON result") != std::string::npos);
			assert(msg.find("/no-result") != std::string::npos);
			caught = true;
			return Ev::lift(0);
		});
	assert(Ev::start(std::move(code)) == 0);
	assert(caught);
}

}

int main() {
	expect_success();
	expect_invalid_json();
	expect_no_json_result();
	return 0;
}
