#undef NDEBUG
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/NormalConnection.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include<arpa/inet.h>
#include<assert.h>
#include<chrono>
#include<cstdint>
#include<memory>
#include<netinet/in.h>
#include<string>
#include<sys/socket.h>
#include<thread>
#include<unistd.h>
#include<vector>

/* A provider that accepts the connection and never answers: the
 * request fails with ApiError once the request timeout passes,
 * instead of holding the thread forever.  */

namespace {

/* Listens on a loopback port, accepts every connection and keeps
 * it open without sending anything, until destroyed.  */
class SilentServer {
private:
	int listen_fd;
	std::uint16_t port_;
	std::thread thread;
	std::vector<int> accepted;

public:
	SilentServer() {
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
		rc = listen(listen_fd, 8);
		assert(rc == 0);
		thread = std::thread([this]() {
			for (;;) {
				auto fd = accept(listen_fd, nullptr, nullptr);
				if (fd < 0)
					break;
				accepted.push_back(fd);
			}
		});
	}
	~SilentServer() {
		/* Wakes the accept in the thread.  */
		shutdown(listen_fd, SHUT_RDWR);
		thread.join();
		close(listen_fd);
		for (auto fd : accepted)
			close(fd);
	}

	std::uint16_t port() const { return port_; }
};

}

int main() {
	SilentServer server;
	Ev::ThreadPool threadpool;
	auto conn = Boltz::Detail::NormalConnection(
		threadpool
	      , "http://127.0.0.1:" + std::to_string(server.port())
	      , ""
	      , 1 /* second */
	);

	auto failed = false;
	auto start = std::chrono::steady_clock::now();
	auto code = conn.api("/v2/swap/reverse", nullptr
	).then([](Jsmn::Object _) {
		/* The server never answers, so a result is wrong.  */
		assert(false);
		return Ev::lift();
	}).catching<Boltz::ApiError>([&](Boltz::ApiError const& _) {
		failed = true;
		return Ev::lift();
	}).then([&]() {
		auto elapsed = std::chrono::steady_clock::now() - start;
		assert(failed);
		/* Far above the one second set, far below forever;
		 * room for a loaded host.  */
		assert(elapsed < std::chrono::seconds(30));
		return Ev::lift(0);
	});

	return Ev::start(code);
}
