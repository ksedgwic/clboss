#undef NDEBUG
#include"Boss/Mod/AvailableRpcCommandsAnnouncer.hpp"
#include"Boss/Msg/AvailableRpcCommands.hpp"
#include"Boss/Msg/Begin.hpp"
#include"Boss/Msg/RequestRpcCommand.hpp"
#include"Boss/Msg/ResponseRpcCommand.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<sstream>

namespace {

auto const help = std::string(R"JSON(
{ "help":
  [ { "command": "clboss-status"
    , "category": "plugin"
    }
  , { "command": "clboss-ignore-onchain [hours]"
    , "category": "plugin"
    }
  , { "command": "clboss-unmanage nodeid tags"
    , "category": "plugin"
    }
  , { "command": "clboss-recent-earnings [days]"
    , "category": "plugin"
    }
  , { "command": "clboss-earnings-history [nodeid]"
    , "category": "plugin"
    }
  ]
}
)JSON");

}

int main() {
	auto bus = S::Bus();
	auto mod = Boss::Mod::AvailableRpcCommandsAnnouncer(bus);

	/* Mock the `help` command.  */
	auto help_flag = false;
	bus.subscribe<Boss::Msg::RequestRpcCommand
		     >([&](Boss::Msg::RequestRpcCommand const& m) {
		/* Make sure it only happens once.  */
		assert(m.command == "help");
		assert(!help_flag);

		help_flag = true;

		auto j_help = Jsmn::Object();
		auto s_help = std::istringstream(help);
		s_help >> j_help;

		auto response = Boss::Msg::ResponseRpcCommand();
		response.requester = m.requester;
		response.succeeded = true;
		response.result = std::move(j_help);

		return bus.raise(std::move(response));
	});

	/* Wait for the `AvailableRpcCommands`.  */
	auto available_flag = false;
	bus.subscribe<Boss::Msg::AvailableRpcCommands
		     >([&](Boss::Msg::AvailableRpcCommands const& m) {
		/* Make sure this only happens once.  */
		assert(!available_flag);
		available_flag = true;

		/* Checks.  */
		auto commands = m.commands;
		assert(commands.count("clboss-status") != 0);
		assert(commands.count("clboss-ignore-onchain") != 0);
		assert(commands.count("clboss-unmanage") != 0);
		assert(commands["clboss-status"].usage == "");
		assert(commands["clboss-ignore-onchain"].usage == "[hours]");
		assert(commands["clboss-unmanage"].usage == "nodeid tags");
		assert(commands.count("clboss-recent-earnings") != 0);
		assert(commands["clboss-recent-earnings"].usage == "[days]");
		assert(commands.count("clboss-earnings-history") != 0);
		assert(commands["clboss-earnings-history"].usage == "[nodeid]");

		return Ev::lift();
	});

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::Begin{});
	}).then([&]() {
		/* Let the module execute.  */
		return Ev::yield(25);
	}).then([&]() {
		assert(help_flag);
		assert(available_flag);
		return Ev::lift(0);
	});

	return Ev::start(code);
}
