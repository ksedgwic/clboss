#undef NDEBUG

#include"Boss/Mod/AmountSettingsHandler.hpp"
#include"Boss/Msg/AmountSettings.hpp"
#include"Boss/Msg/EndOfOptions.hpp"
#include"Boss/Msg/Option.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/Amount.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"

#include<assert.h>
#include<cstdint>
#include<memory>
#include<string>
#include<vector>

namespace {

struct Case {
	/* Option values as delivered by lightningd; nullptr = unset.  */
	char const* min_channel;
	char const* max_channel;
	/* Expected settings after validation.  */
	std::uint64_t expect_min;
	std::uint64_t expect_max;
};

auto const cases = std::vector<Case>{
	/* Defaults pass through untouched.  */
	{ nullptr , nullptr  ,  500000, 16777215},
	/* A pair satisfying max >= 3 * min + 20k passes through.  */
	{"1000000", "3020000", 1000000,  3020000},
	/* min below the absolute floor is raised.  */
	{ "400000", nullptr  ,  500000, 16777215},
	/* Conflicting pair: max kept, min lowered to the largest
	 * value satisfying min_channel + min_remaining <= max_channel.  */
	{"1000000", "2000000",  660000,  2000000},
	/* Non-divisible conflict: truncation keeps the invariant.  */
	{"1000000", "3000000",  993333,  3000000},
	/* max too low for any allowed min: max raised, min floored.  */
	{"2000000", "1000000",  500000,  1520000},
};

Boss::Msg::Option make_option(char const* name, char const* value) {
	auto json = "\"" + std::string(value) + "\"";
	return Boss::Msg::Option{
		name,
		Jsmn::Object::parse_json(json.c_str()),
		nullptr
	};
}

}

int main() {
	auto buses = std::vector<std::unique_ptr<S::Bus>>();
	auto handlers = std::vector<
		std::unique_ptr<Boss::Mod::AmountSettingsHandler>
	>();

	auto code = Ev::lift();
	for (auto const& c : cases) {
		buses.push_back(Util::make_unique<S::Bus>());
		auto& bus = *buses.back();
		handlers.push_back(
			Util::make_unique<Boss::Mod::AmountSettingsHandler>(bus)
		);

		auto captured = std::make_shared<Boss::Msg::AmountSettings>();
		auto have = std::make_shared<bool>(false);
		bus.subscribe<Boss::Msg::AmountSettings
			     >([captured, have](Boss::Msg::AmountSettings const& m) {
			*captured = m;
			*have = true;
			return Ev::lift();
		});

		code += Ev::lift().then([&bus, c]() {
			if (!c.min_channel)
				return Ev::lift();
			return bus.raise(make_option( "clboss-min-channel"
						    , c.min_channel
						    ));
		}).then([&bus, c]() {
			if (!c.max_channel)
				return Ev::lift();
			return bus.raise(make_option( "clboss-max-channel"
						    , c.max_channel
						    ));
		}).then([&bus]() {
			return bus.raise(Boss::Msg::EndOfOptions{});
		}).then([captured, have, c]() {
			assert(*have);
			assert( captured->min_channel
			     == Ln::Amount::sat(c.expect_min)
			      );
			assert( captured->max_channel
			     == Ln::Amount::sat(c.expect_max)
			      );
			/* min_remaining derivation.  */
			assert( captured->min_remaining
			     == 2.0 * captured->min_channel
			      + Ln::Amount::sat(20000)
			      );
			/* Whatever was configured, the published
			 * settings must satisfy the Planner
			 * precondition.  */
			assert( captured->min_channel
			      + captured->min_remaining
			     <= captured->max_channel
			      );
			return Ev::lift();
		});
	}

	return Ev::start(std::move(code).then([]() {
		return Ev::lift(0);
	}));
}
