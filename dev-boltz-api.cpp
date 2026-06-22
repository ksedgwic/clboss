#include"Boltz/Connection.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/ThreadPool.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Util/make_unique.hpp"
#include<cstdlib>
#include<iostream>
#include<sstream>
#include<string>

/* argv[1] == /api
 * argv[2] == JSON
 *
 * Environment overrides:
 *   BOLTZ_API_BASE  URL prefix (default: https://boltz.exchange/api)
 *   BOLTZ_PROXY     libcurl proxy (e.g. socks5h://127.0.0.1:9050)
 */
int main(int argc, char** argv) {
	if (argc < 2 || argc > 3) {
		std::cerr << "Usage: dev-boltz-api /url json" << std::endl;
		return 1;
	}
	auto api = std::string(argv[1]);
	auto json = std::unique_ptr<Json::Out>(nullptr);
	if (argc == 3) {
		auto json_arg = ([argv]() {
			auto is = std::istringstream(std::string(argv[2]));
			auto rv = Jsmn::Object();
			is >> rv;
			return rv;
		})();
		json = Util::make_unique<Json::Out>(
			Json::Out(json_arg)
		);
	}

	auto base = std::string("https://boltz.exchange/api");
	if (auto e = std::getenv("BOLTZ_API_BASE")) base = e;
	auto proxy = std::string("");
	if (auto e = std::getenv("BOLTZ_PROXY")) proxy = e;

	Ev::ThreadPool tp;
	auto cc = Boltz::Connection(tp, base, proxy);

	auto code = Ev::lift().then([&]() {
		return cc.api(api, std::move(json));
	}).then([](Jsmn::Object result) {
		std::cout << result << std::endl;
		return Ev::lift(0);
	});

	return Ev::start(code);
}
