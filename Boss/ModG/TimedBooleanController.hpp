#ifndef BOSS_MOD_TIMEDBOOLEANCONTROLLER_HPP
#define BOSS_MOD_TIMEDBOOLEANCONTROLLER_HPP

#include"Boss/ModG/TimedBoolean.hpp"
#include<memory>

namespace S { class Bus; }

namespace Boss { namespace Mod {

template <typename RequestMsg, typename ResponseMsg,
	  const char* controller_name, const char* option_name,
	  const char* description>
class TimedBooleanController {
private:
	Boss::ModG::TimedBoolean<RequestMsg, ResponseMsg> timed_boolean;

	void start() {
		timed_boolean.start();
	}

public:
	TimedBooleanController() =delete;
	TimedBooleanController(TimedBooleanController&&) =delete;
	TimedBooleanController(TimedBooleanController const&) =delete;

	~TimedBooleanController() =default;

	explicit
	TimedBooleanController(S::Bus& bus_)
	: timed_boolean(bus_, controller_name, option_name, description)
	{ start(); }
};

}}

#endif /* !defined(BOSS_MOD_TIMEDBOOLEANCONTROLLER_HPP) */
