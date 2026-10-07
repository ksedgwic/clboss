#undef NDEBUG
#include"Boss/Mod/ChannelFeeManager.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/Option.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<memory>
#include<sstream>
#include<string>
#include<vector>

/* clboss-zerobasefee accepts require, allow and disallow (and the
 * -d spellings) at startup and from setconfig, and refuses any
 * other word, keeping the current setting.  The setting is
 * observed through the log line each change writes.  */

namespace {

Ev::Io<std::string> setconfig(S::Bus& bus, char const* value) {
	auto reason = std::make_shared<std::string>();
	auto json = "\"" + std::string(value) + "\"";
	return bus.raise(Boss::Msg::Option{
		"clboss-zerobasefee",
		Jsmn::Object::parse_json(json.c_str()),
		reason
	}).then([reason]() {
		return Ev::lift(*reason);
	});
}

}

int main() {
	auto bus = S::Bus();
	auto mut = Boss::Mod::ChannelFeeManager(bus);

	auto logs = std::vector<std::string>();
	bus.subscribe<Boss::Msg::JsonCout
		     >([&](Boss::Msg::JsonCout const& m) {
		auto is = std::istringstream(m.obj.output());
		auto js = Jsmn::Object();
		is >> js;
		if ( js.is_object() && js.has("method")
		  && std::string(js["method"]) == "log"
		   )
			logs.push_back(std::string(js["params"]["message"]));
		return Ev::lift();
	});
	auto last_has = [&](char const* needle) {
		return !logs.empty()
		    && logs.back().find(needle) != std::string::npos;
	};

	auto code = Ev::lift().then([&]() {
		/* Startup delivery: a plain string, no back-channel.  */
		return bus.raise(Boss::Msg::Option{
			"clboss-zerobasefee",
			Jsmn::Object::parse_json("\"disallow\""),
			nullptr
		});
	}).then([&]() {
		assert(last_has("zerobasefee: disallow"));
		return setconfig(bus, "bogus");
	}).then([&](std::string reason) {
		assert(!reason.empty());
		assert(last_has("keeping disallow"));
		return setconfig(bus, "require");
	}).then([&](std::string reason) {
		assert(reason.empty());
		assert(last_has("zerobasefee: require"));
		return setconfig(bus, "allowed");
	}).then([&](std::string reason) {
		assert(reason.empty());
		assert(last_has("zerobasefee: allow"));
		return setconfig(bus, "disallowed");
	}).then([&](std::string reason) {
		assert(reason.empty());
		assert(last_has("zerobasefee: disallow"));
		return Ev::lift(0);
	});

	return Ev::start(code);
}
