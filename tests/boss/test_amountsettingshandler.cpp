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
	char const* min_onchain;
	/* Expected settings after validation.  */
	std::uint64_t expect_min;
	std::uint64_t expect_max;
	std::uint64_t expect_reserve;
};

auto const cases = std::vector<Case>{
	/* Defaults pass through untouched.  */
	{ nullptr , nullptr  , nullptr   ,  500000, 16777215,   30000},
	/* A pair satisfying max >= 3 * min + 20k passes through.  */
	{"1000000", "3020000", nullptr  , 1000000,  3020000,   30000},
	/* min below the absolute floor is raised.  */
	{ "400000", nullptr  , nullptr  ,  500000, 16777215,   30000},
	/* Conflicting pair: max kept, min lowered to the largest
	 * value satisfying min_channel + min_remaining <= max_channel.  */
	{"1000000", "2000000", nullptr  ,  660000,  2000000,   30000},
	/* Non-divisible conflict: truncation keeps the invariant.  */
	{"1000000", "3000000", nullptr  ,  993333,  3000000,   30000},
	/* max too low for any allowed min: max raised, min floored.  */
	{"2000000", "1000000", nullptr  ,  500000, 1520000,   30000},
	/* Reserve below CLN min-emergency-msat (25000 sat
	 * default) + funding fees makes every multifundchannel
	 * fail 313 forever; forced up to the usable floor.  */
	{ nullptr , nullptr  ,  "10000" ,  500000, 16777215,   30000},
	{ nullptr , nullptr  ,  "29999" ,  500000, 16777215,   30000},
	/* Exactly at the floor stays.  */
	{ nullptr , nullptr  ,  "30000" ,  500000, 16777215,   30000},
	/* Higher reserves pass through untouched.  */
	{ nullptr , nullptr  ,  "500000",  500000, 16777215,  500000},
	/* A leading '-' parses as 0 and is floored: "-1" would
	 * otherwise wrap to a huge value through the unsigned
	 * parse and bypass the floor.  Whitespace before the
	 * minus is skipped the same way the stream skips it,
	 * so " -1" wraps the same way and is rejected too.  */
	{ nullptr , nullptr  ,     "-1" ,  500000, 16777215,   30000},
	{ nullptr , nullptr  ,  " -1" ,  500000, 16777215,   30000},
	/* Any other spelling keeps the stream parse, so a
	 * leading '+' still parses; above the floor, to
	 * distinguish acceptance from floored rejection.  */
	{ nullptr , nullptr  , "+500000",  500000, 16777215,  500000},
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
		}).then([&bus, c]() {
			if (!c.min_onchain)
				return Ev::lift();
			return bus.raise(make_option( "clboss-min-onchain"
						    , c.min_onchain
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
			assert( captured->reserve
			     == Ln::Amount::sat(c.expect_reserve)
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

	/* The setconfig path: after EndOfOptions a clean value that
	 * passes validation is applied and the settings are published
	 * again; a value the validation would alter, or that is not
	 * an amount, is refused and nothing is published.  */
	buses.push_back(Util::make_unique<S::Bus>());
	auto& bus = *buses.back();
	handlers.push_back(
		Util::make_unique<Boss::Mod::AmountSettingsHandler>(bus)
	);
	auto published = std::make_shared<std::vector<Boss::Msg::AmountSettings>>();
	bus.subscribe<Boss::Msg::AmountSettings
		     >([published](Boss::Msg::AmountSettings const& m) {
		published->push_back(m);
		return Ev::lift();
	});
	/* A setconfig delivery: the value as a JSON string, with the
	 * rejection back-channel allocated.  Returns the reason, empty
	 * when accepted.  */
	auto setconfig = [&bus](char const* name, char const* value) {
		auto reason = std::make_shared<std::string>();
		auto json = "\"" + std::string(value) + "\"";
		return bus.raise(Boss::Msg::Option{
			name,
			Jsmn::Object::parse_json(json.c_str()),
			reason
		}).then([reason]() {
			return Ev::lift(*reason);
		});
	};
	auto last = [published]() -> Boss::Msg::AmountSettings const& {
		return published->back();
	};

	code += Ev::lift().then([&bus]() {
		return bus.raise(make_option("clboss-min-channel", "1000000"));
	}).then([&bus]() {
		return bus.raise(make_option("clboss-max-channel", "3020000"));
	}).then([&bus]() {
		return bus.raise(Boss::Msg::EndOfOptions{});
	}).then([published, last]() {
		assert(published->size() == 1);
		assert(last().reserve == Ln::Amount::sat(30000));
		return Ev::lift();
	}).then([setconfig]() {
		/* Accepted: published again with the new reserve.  */
		return setconfig("clboss-min-onchain", "40000");
	}).then([published, last](std::string reason) {
		assert(reason.empty());
		assert(published->size() == 2);
		assert(last().reserve == Ln::Amount::sat(40000));
		assert(last().min_channel == Ln::Amount::sat(1000000));
		return Ev::lift();
	}).then([setconfig]() {
		/* Not an amount.  */
		return setconfig("clboss-min-onchain", "abc");
	}).then([published, setconfig](std::string reason) {
		assert(!reason.empty());
		assert(published->size() == 2);
		/* Trailing text.  */
		return setconfig("clboss-min-onchain", "50000 sat");
	}).then([published, setconfig](std::string reason) {
		assert(!reason.empty());
		assert(published->size() == 2);
		/* A leading minus.  */
		return setconfig("clboss-min-onchain", " -1");
	}).then([published, setconfig](std::string reason) {
		assert(!reason.empty());
		assert(published->size() == 2);
		/* Below the reserve floor: the startup path would force
		 * it up; setconfig refuses it.  */
		return setconfig("clboss-min-onchain", "10000");
	}).then([published, last, setconfig](std::string reason) {
		assert(!reason.empty());
		assert(published->size() == 2);
		assert(last().reserve == Ln::Amount::sat(40000));
		/* A max below 3 * min + 20000 would lower min at
		 * startup; refused here, min and max unchanged.  */
		return setconfig("clboss-max-channel", "2000000");
	}).then([published, last, setconfig](std::string reason) {
		assert(!reason.empty());
		assert(published->size() == 2);
		assert(last().max_channel == Ln::Amount::sat(3020000));
		/* Raising max is fine.  */
		return setconfig("clboss-max-channel", "5000000");
	}).then([published, last, setconfig](std::string reason) {
		assert(reason.empty());
		assert(published->size() == 3);
		assert(last().max_channel == Ln::Amount::sat(5000000));
		/* A min that now fits under the raised max.  */
		return setconfig("clboss-min-channel", "1500000");
	}).then([published, last, setconfig](std::string reason) {
		assert(reason.empty());
		assert(published->size() == 4);
		assert(last().min_channel == Ln::Amount::sat(1500000));
		assert( last().min_remaining
		     == 2.0 * last().min_channel + Ln::Amount::sat(20000)
		      );
		/* Below the absolute min floor.  */
		return setconfig("clboss-min-channel", "400000");
	}).then([published, last](std::string reason) {
		assert(!reason.empty());
		assert(published->size() == 4);
		assert(last().min_channel == Ln::Amount::sat(1500000));
		return Ev::lift();
	});

	return Ev::start(std::move(code).then([]() {
		return Ev::lift(0);
	}));
}
