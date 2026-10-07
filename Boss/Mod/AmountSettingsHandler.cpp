#include"Boss/Mod/AmountSettingsHandler.hpp"
#include"Boss/Msg/AmountSettings.hpp"
#include"Boss/Msg/EndOfOptions.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/log.hpp"
#include"Jsmn/Object.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<sstream>
#include<string>
#include<vector>

namespace {

/* Default channel boundaries.  */
auto const default_min_channel = Ln::Amount::sat(  500000);
auto const default_max_channel = Ln::Amount::sat(16777215);

/* Default amount to always leave onchain for future
 * channel management actions.  */
auto const default_reserve =     Ln::Amount::sat(   30000);

/* The absolute lowest min_channel setting.  */
auto const min_min_channel =     Ln::Amount::sat(  500000);
/* How much larger the channel-creation trigger should be over
 * the min_channel.  */
auto const trigger_factor = double(2.0);
/* How much to add to the channel-creation trigger above, to get
 * the amount to leave after creation.  */
auto const additional_remaining = Ln::Amount::sat(20000);

/* The ChannelCreator Planner requires
 *   min_channel + min_remaining <= max_channel
 * where min_remaining = trigger_factor * min_channel
 * + additional_remaining.  This is the lowest max_channel that
 * satisfies it at the lowest allowed min_channel; below this no
 * valid min_channel exists.  */
auto const min_usable_max_channel =
	(1.0 + trigger_factor) * min_min_channel + additional_remaining;

/* CLBOSS computes the channel-creation budget as
 *   onchain_spendable - reserve
 * and spends down to that level, but CLN refuses any funding
 * transaction that would leave less than its `min-emergency-msat`
 * (25,000 sat default) plus the funding fee.  A reserve below
 * that floor makes every multifundchannel fail with error 313
 * forever while churning the candidate table, so floor it.  */
auto const min_usable_reserve = Ln::Amount::sat(30000);

/* Read an amount in satoshis from the option value.
 *
 * `operator>>` into an unsigned skips leading whitespace and
 * then accepts a leading '-', so even " -1" wraps to a huge
 * value and bypasses the floors below.  Skip the same whitespace
 * the stream would, then read nothing for a minus there (the
 * amount stays 0); everything else keeps the stream parse, so
 * spellings that parsed before, including a leading '+', still
 * parse.
 *
 * Returns whether the text was a clean amount: digits, with an
 * optional leading '+' and surrounding whitespace, and nothing
 * else.  The startup path keeps the lenient value either way, as
 * it always has; the setconfig path refuses anything unclean.  */
bool parse_sats(Jsmn::Object const& value, Ln::Amount& out) {
	auto str = std::string(value);
	auto is = std::istringstream(str);
	auto sats = std::uint64_t(0);
	auto clean = true;
	is >> std::ws;
	if (is.peek() == '-')
		clean = false;
	else {
		is >> sats;
		if (is.fail()) {
			sats = 0;
			clean = false;
		} else {
			is >> std::ws;
			if (!is.eof())
				clean = false;
		}
	}
	out = Ln::Amount::sat(sats);
	return clean;
}

unsigned int sat(Ln::Amount const& a) {
	return (unsigned int) a.to_sat();
}

/* One adjustment the validation made: the problem, and what was
 * done about it.  The startup path logs both; the setconfig path
 * refuses the value with the problem alone.  */
struct Note {
	Boss::LogLevel level;
	std::string problem;
	std::string fix;
};

/* Apply the floors and the planner precondition to the settings
 * and derive min_amount and min_remaining, noting each forced
 * change.  */
std::vector<Note> validate(Boss::Msg::AmountSettings& s) {
	auto notes = std::vector<Note>();

	if (s.min_channel < min_min_channel) {
		auto os = std::ostringstream();
		os << "--clboss-min-channel " << sat(s.min_channel)
		   << " is below the floor " << sat(min_min_channel)
		    ;
		auto fix = std::ostringstream();
		fix << "forced to " << sat(min_min_channel);
		notes.push_back(Note{Boss::Info, os.str(), fix.str()});
		s.min_channel = min_min_channel;
	}
	if (s.max_channel < min_usable_max_channel) {
		auto os = std::ostringstream();
		os << "--clboss-max-channel " << sat(s.max_channel)
		   << " is too low for any allowed --clboss-min-channel "
		      "(at least " << sat(min_usable_max_channel) << ")"
		    ;
		auto fix = std::ostringstream();
		fix << "forced to " << sat(min_usable_max_channel);
		notes.push_back(Note{Boss::Warn, os.str(), fix.str()});
		s.max_channel = min_usable_max_channel;
	}
	if (s.reserve < min_usable_reserve) {
		auto os = std::ostringstream();
		os << "clboss-min-onchain " << sat(s.reserve)
		   << " is below " << sat(min_usable_reserve)
		   << " sat, CLN's default min-emergency-msat plus "
		      "funding fees; every channel open would fail"
		    ;
		auto fix = std::ostringstream();
		fix << "using " << sat(min_usable_reserve);
		notes.push_back(Note{Boss::Warn, os.str(), fix.str()});
		s.reserve = min_usable_reserve;
	}

	/* Compute the rest.  */
	s.min_amount = trigger_factor * s.min_channel;
	s.min_remaining = s.min_amount + additional_remaining;

	/* The ChannelCreator Planner asserts
	 *   min_channel + min_remaining <= max_channel
	 * at construction, so a violating config would abort on the
	 * first channel-creation run.  max_channel is the knob that
	 * sets typical channel size: keep it, and lower min_channel
	 * to the largest value that fits.  */
	if (s.min_channel + s.min_remaining > s.max_channel) {
		auto lowered = Ln::Amount::sat(
			(s.max_channel - additional_remaining).to_sat()
			/ (std::uint64_t)(1.0 + trigger_factor)
		);
		auto os = std::ostringstream();
		os << "--clboss-min-channel " << sat(s.min_channel)
		   << " and --clboss-max-channel " << sat(s.max_channel)
		   << " conflict (max must be at least "
		   << sat(s.min_channel + s.min_remaining) << ")"
		    ;
		auto fix = std::ostringstream();
		fix << "--clboss-min-channel forced to " << sat(lowered);
		notes.push_back(Note{Boss::Warn, os.str(), fix.str()});
		s.min_channel = lowered;
		s.min_amount = trigger_factor * s.min_channel;
		s.min_remaining = s.min_amount + additional_remaining;
	}

	return notes;
}

}

namespace Boss { namespace Mod {

class AmountSettingsHandler::Impl {
private:
	S::Bus& bus;
	/* The settings in effect: as configured until EndOfOptions,
	 * validated and published from then on.  */
	Msg::AmountSettings settings;
	bool options_ended;

	static
	Ln::Amount* field_of(Msg::AmountSettings& s, std::string const& name) {
		if (name == "clboss-min-onchain")
			return &s.reserve;
		if (name == "clboss-min-channel")
			return &s.min_channel;
		if (name == "clboss-max-channel")
			return &s.max_channel;
		return nullptr;
	}

	/* Startup: take the configured value; the validation runs at
	 * EndOfOptions over all three together.  */
	Ev::Io<void> on_startup_option( Msg::Option const& o
				      , Ln::Amount& field
				      , Ln::Amount const& default_value
				      , char const* what
				      ) {
		parse_sats(o.value, field);
		if (field == default_value)
			return Ev::lift();
		return Boss::log( bus, Info
				, "AmountSettingsHandler: "
				  "%s set by --%s to %s satoshis."
				, what
				, o.name.c_str()
				, std::string(o.value).c_str()
				);
	}

	/* setconfig: validate a copy with the new value in place, and
	 * refuse a value the validation would alter, so the value
	 * lightningd persists is always the one in effect.  */
	Ev::Io<void> on_dynamic_option(Msg::Option const& o) {
		auto amount = Ln::Amount();
		if (!parse_sats(o.value, amount)) {
			o.reject( o.name + ": not a valid amount in satoshis");
			return Boss::log( bus, Warn
					, "AmountSettingsHandler: %s: '%s' is "
					  "not a valid amount in satoshis; "
					  "keeping %u."
					, o.name.c_str()
					, std::string(o.value).c_str()
					, sat(*field_of(settings, o.name))
					);
		}
		auto candidate = settings;
		*field_of(candidate, o.name) = amount;
		auto notes = validate(candidate);
		if (!notes.empty()) {
			o.reject(notes[0].problem);
			return Boss::log( bus, Warn
					, "AmountSettingsHandler: %s %u "
					  "refused: %s; keeping %u."
					, o.name.c_str()
					, sat(amount)
					, notes[0].problem.c_str()
					, sat(*field_of(settings, o.name))
					);
		}
		settings = candidate;
		return Boss::log( bus, Info
				, "AmountSettingsHandler: %s set to %u "
				  "satoshis."
				, o.name.c_str()
				, sat(amount)
				).then([this]() {
			return bus.raise(Msg::AmountSettings(settings));
		});
	}

	void start() {
		settings.min_channel = default_min_channel;
		settings.max_channel = default_max_channel;
		settings.reserve = default_reserve;
		options_ended = false;

		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return bus.raise(Msg::ManifestOption{
				"clboss-min-onchain",
				Msg::OptionType_String,
				Json::Out::direct(default_reserve.to_sat()),
				"Target to leave this number of satoshis "
				"onchain, putting the rest into channels.  "
				"Dynamic: settable at runtime via "
				"`lightning-cli setconfig`.",
				/* dynamic = */ true
			}) + bus.raise(Msg::ManifestOption{
				"clboss-min-channel",
				Msg::OptionType_String,
				Json::Out::direct(default_min_channel.to_sat()),
				"Minimum size of channels to make.  "
				"Dynamic: settable at runtime via "
				"`lightning-cli setconfig`.",
				/* dynamic = */ true
			}) + bus.raise(Msg::ManifestOption{
				"clboss-max-channel",
				Msg::OptionType_String,
				Json::Out::direct(default_max_channel.to_sat()),
				"Maximum size of channels to make.  "
				"Dynamic: settable at runtime via "
				"`lightning-cli setconfig`.",
				/* dynamic = */ true
			});
		});

		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			/* Every Msg::Option reaches every subscriber;
			 * only the three amounts are ours.  */
			if (!field_of(settings, o.name))
				return Ev::lift();
			if (options_ended)
				return on_dynamic_option(o);
			if (o.name == "clboss-min-onchain")
				return on_startup_option( o, settings.reserve
							, default_reserve
							, "Onchain reserve"
							);
			if (o.name == "clboss-min-channel")
				return on_startup_option( o, settings.min_channel
							, default_min_channel
							, "Minimum channel size"
							);
			return on_startup_option( o, settings.max_channel
						, default_max_channel
						, "Maximum channel size"
						);
		});

		bus.subscribe<Msg::EndOfOptions
			     >([this](Msg::EndOfOptions const& _) {
			assert(!options_ended);
			options_ended = true;

			auto act = Ev::lift();
			for (auto const& n : validate(settings))
				act += Boss::log( bus, n.level
						, "AmountSettingsHandler: "
						  "%s, %s."
						, n.problem.c_str()
						, n.fix.c_str()
						);

			return act + bus.raise(Msg::AmountSettings(settings));
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl( S::Bus& bus_
	    ) : bus(bus_)
	      , settings()
	      , options_ended(false)
	      { start(); }
};

AmountSettingsHandler::~AmountSettingsHandler() =default;
AmountSettingsHandler::AmountSettingsHandler(AmountSettingsHandler&&) =default;

AmountSettingsHandler::AmountSettingsHandler(S::Bus& bus) : pimpl(Util::make_unique<Impl>(bus)) {}

}}
