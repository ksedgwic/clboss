#undef NDEBUG
#include"Boss/Mod/Initiator.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Net/Fd.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<deque>
#include<errno.h>
#include<fcntl.h>
#include<limits.h>
#include<sstream>
#include<stdint.h>
#include<string>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>
#include<vector>

namespace {

std::string const self_id
= "020000000000000000000000000000000000000000000000000000000000000000";

Jsmn::Object parse_json(std::string const& text) {
	auto parser = Jsmn::Parser();
	auto objs = parser.feed(text);
	assert(objs.size() == 1);
	return std::move(objs[0]);
}

class TempDirGuard {
private:
	std::string old_cwd;
	std::string temp_dir;

public:
	TempDirGuard() {
		auto oldbuf = std::vector<char>(PATH_MAX, '\0');
		auto p = getcwd(oldbuf.data(), oldbuf.size());
		assert(p != nullptr);
		old_cwd = std::string(p);

		auto tmpl = std::string("/tmp/clboss-initiator-test-XXXXXX");
		auto modifiable = std::vector<char>(tmpl.begin(), tmpl.end());
		modifiable.push_back('\0');
		auto t = mkdtemp(modifiable.data());
		assert(t != nullptr);
		temp_dir = std::string(t);

		auto res = chdir(temp_dir.c_str());
		assert(res == 0);
	}

	~TempDirGuard() {
		auto res = chdir(old_cwd.c_str());
		assert(res == 0);

		unlink((temp_dir + "/data.clboss").c_str());
		unlink((temp_dir + "/keys.clboss").c_str());
		rmdir(temp_dir.c_str());
	}

	TempDirGuard(TempDirGuard&&) =delete;
	TempDirGuard(TempDirGuard const&) =delete;
};

class RpcServerMock {
private:
	Net::Fd fd;
	Jsmn::Parser parser;
	std::deque<Jsmn::Object> requests;

	Ev::Io<Jsmn::Object> read_request(std::size_t retries = 0) {
		return Ev::yield().then([this]() {
			if (!requests.empty()) {
				auto req = std::move(requests.front());
				requests.pop_front();
				return Ev::lift(std::move(req));
			}
			return Ev::lift(Jsmn::Object());
		}).then([this, retries](Jsmn::Object req) {
			if (!req.is_null())
				return Ev::lift(std::move(req));
			assert(retries < 100000);

			char buf[512];
			auto rd = ssize_t();
			do {
				rd = read(fd.get(), buf, sizeof(buf));
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

	Ev::Io<void> write_all(std::string data) {
		return Ev::yield().then([this, data]() {
			auto wr = ssize_t();
			do {
				wr = write(fd.get(), data.c_str(), data.size());
			} while (wr < 0 && errno == EINTR);

			if (wr < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
				return write_all(data);
			assert(wr >= 0);
			if (std::size_t(wr) < data.size())
				return write_all(data.substr(std::size_t(wr)));

			return Ev::lift();
		});
	}

	static std::string extract_id_and_check_method( Jsmn::Object const& req
						      , std::string const& method
						      ) {
		assert(req.is_object());
		assert(req.has("id"));
		assert(req["id"].is_number());
		assert(req.has("method"));
		assert(req["method"].is_string());
		assert(std::string(req["method"]) == method);
		return req["id"].direct_text();
	}

	Ev::Io<void> reply_result(std::string const& id, std::string const& result) {
		auto response = std::string();
		response += R"({"jsonrpc":"2.0","id":)";
		response += id;
		response += R"(,"result":)";
		response += result;
		response += "}\n\n";
		return write_all(std::move(response));
	}

public:
	explicit
	RpcServerMock(Net::Fd fd_) : fd(std::move(fd_)), parser(), requests() {
		auto flags = fcntl(fd.get(), F_GETFL);
		assert(flags >= 0);
		flags |= O_NONBLOCK;
		auto res = fcntl(fd.get(), F_SETFL, flags);
		assert(res == 0);
	}
	RpcServerMock(RpcServerMock&&) =delete;

	Ev::Io<void> run(std::string listconfigs_result) {
		return read_request().then([this](Jsmn::Object req) {
			auto id = extract_id_and_check_method(req, "getinfo");
			return reply_result(id, std::string(R"({"id":")") + self_id + R"("})");
		}).then([this]() {
			return read_request();
		}).then([this, listconfigs_result](Jsmn::Object req) {
			auto id = extract_id_and_check_method(req, "listconfigs");
			return reply_result(id, listconfigs_result);
		});
	}
};

struct ProxyConfig {
	std::string proxy;
	bool always_use_proxy;
};

ProxyConfig run_initiator_case(std::string listconfigs_result) {
	auto guard = TempDirGuard();

	int socks[2];
	auto res = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
	assert(res == 0);
	auto server_socket = Net::Fd(socks[0]);
	auto client_socket = Net::Fd(socks[1]);

	auto bus = S::Bus();
	auto threadpool = Ev::ThreadPool();
	auto server = RpcServerMock(std::move(server_socket));

	auto client_socket_holder = std::make_shared<Net::Fd>(std::move(client_socket));
	auto initiator = Boss::Mod::Initiator(
		bus, threadpool,
		[client_socket_holder]( std::string const& lightning_dir
				      , std::string const& rpc_file
				      ) {
			assert(lightning_dir == ".");
			assert(rpc_file == "lightning-rpc");
			auto fd = Net::Fd();
			std::swap(fd, *client_socket_holder);
			assert(fd);
			return fd;
		}
	);

	auto received_init = false;
	auto received_response = false;
	auto received_fail = false;
	auto got = ProxyConfig();

	bus.subscribe<Boss::Msg::Init>([&](Boss::Msg::Init const& m) {
		assert(!received_init);
		received_init = true;
		got.proxy = m.proxy;
		got.always_use_proxy = m.always_use_proxy;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::CommandResponse>([&](Boss::Msg::CommandResponse const& m) {
		assert(!received_response);
		m.id.cmatch([&](std::uint64_t id) {
			assert(id == 42);
		}, [&](std::string const&) {
			assert(false);
		});
		received_response = true;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::CommandFail>([&](Boss::Msg::CommandFail const& m) {
		(void) m;
		received_fail = true;
		return Ev::lift();
	});

	auto params = parse_json(R"JSON(
	{
	  "configuration": {
	    "network": "regtest",
	    "lightning-dir": ".",
	    "rpc-file": "lightning-rpc"
	  }
	}
	)JSON");

	auto req = Boss::Msg::CommandRequest{
		"init",
		std::move(params),
		Ln::CommandId::left(std::uint64_t(42))
	};

	auto server_code = server.run(std::move(listconfigs_result));
	auto client_code = Ev::lift().then([&]() {
		return bus.raise(req);
	}).then([&]() {
		assert(!received_fail);
		assert(received_response);
		assert(received_init);
		return bus.raise(Boss::Shutdown{});
	}).then([&]() {
		return Ev::lift(0);
	});

	auto code = Ev::lift().then([&]() {
		return Ev::concurrent(std::move(server_code));
	}).then([&]() {
		return client_code;
	});

	auto ec = Ev::start(code);
	assert(ec == 0);
	return got;
}

}

int main() {
	auto const legacy = std::string(R"JSON(
	{
	  "proxy": { "value_str": "127.0.0.1:9050" },
	  "always-use-proxy": { "value_bool": true }
	}
	)JSON");
	auto const modern = std::string(R"JSON(
	{
	  "configs": {
	    "proxy": { "value_str": "127.0.0.1:9050" },
	    "always-use-proxy": { "value_bool": true }
	  }
	}
	)JSON");

	auto legacy_cfg = run_initiator_case(legacy);
	auto modern_cfg = run_initiator_case(modern);

	assert(legacy_cfg.proxy == "127.0.0.1:9050");
	assert(modern_cfg.proxy == "127.0.0.1:9050");
	assert(legacy_cfg.always_use_proxy);
	assert(modern_cfg.always_use_proxy);

	assert(legacy_cfg.proxy == modern_cfg.proxy);
	assert(legacy_cfg.always_use_proxy == modern_cfg.always_use_proxy);

	return 0;
}
