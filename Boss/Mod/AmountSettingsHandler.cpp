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

Ln::Amount parse_sats(Jsmn::Object value) {
	auto is = std::istringstream(std::string(value));
	auto sats = std::uint64_t();
	is >> sats;
	return Ln::Amount::sat(sats);
}

}

namespace Boss { namespace Mod {

class AmountSettingsHandler::Impl {
private:
	S::Bus& bus;
	std::unique_ptr<Msg::AmountSettings> settings;

	void start() {
		settings->min_channel = default_min_channel;
		settings->max_channel = default_max_channel;
		settings->reserve = default_reserve;

		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			assert(settings);
			return bus.raise(Msg::ManifestOption{
				"clboss-min-onchain",
				Msg::OptionType_String,
				Json::Out::direct(default_reserve.to_sat()),
				"Target to leave this number of satoshis "
				"onchain, putting the rest into channels."
			}) + bus.raise(Msg::ManifestOption{
				"clboss-min-channel",
				Msg::OptionType_String,
				Json::Out::direct(default_min_channel.to_sat()),
				"Minimum size of channels to make."
			}) + bus.raise(Msg::ManifestOption{
				"clboss-max-channel",
				Msg::OptionType_String,
				Json::Out::direct(default_max_channel.to_sat()),
				"Maximum size of channels to make."
			});
		});

		bus.subscribe<Msg::Option
			     >([this](Msg::Option o) {
			assert(settings);
			auto const& name = o.name;
			if (name == "clboss-min-onchain") {
				settings->reserve = parse_sats(o.value);
				if (settings->reserve == default_reserve)
					return Ev::lift();
				return Boss::log( bus, Info
						, "AmountSettingsHandler: "
						  "Onchain reserve set by "
						  "--clboss-min-onchain to "
						  "%s satoshis."
						, std::string(o.value).c_str()
						);
			} else if (name == "clboss-min-channel") {
				settings->min_channel = parse_sats(o.value);
				if (settings->min_channel == default_min_channel)
					return Ev::lift();
				return Boss::log( bus, Info
						, "AmountSettingsHandler: "
						  "Minimum channel size set by "
						  "--clboss-min-channel to "
						  "%s satoshis."
						, std::string(o.value).c_str()
						);
			} else if (name == "clboss-max-channel") {
				settings->max_channel = parse_sats(o.value);
				if (settings->max_channel == default_max_channel)
					return Ev::lift();
				return Boss::log( bus, Info
						, "AmountSettingsHandler: "
						  "Maximum channel size set by "
						  "--clboss-max-channel to "
						  "%s satoshis."
						, std::string(o.value).c_str()
						);
			}
			return Ev::lift();
		});

		bus.subscribe<Msg::EndOfOptions
			     >([this](Msg::EndOfOptions const& _) {
			assert(settings);

			auto act = Ev::lift();

			/* Validate settings.  */
			if (settings->min_channel < min_min_channel) {
				settings->min_channel = min_min_channel;
				act += Boss::log( bus, Info
						, "AmountSettingsHandler: "
						  "--clboss-min-channel too "
						  "low, forced to %u."
						, (unsigned int)
						  min_min_channel.to_sat()
						);
			}
			if (settings->max_channel < min_usable_max_channel) {
				act += Boss::log( bus, Warn
						, "AmountSettingsHandler: "
						  "--clboss-max-channel %u too "
						  "low for any allowed "
						  "--clboss-min-channel, "
						  "forced to %u."
						, (unsigned int)
						  settings->max_channel.to_sat()
						, (unsigned int)
						  min_usable_max_channel.to_sat()
						);
				settings->max_channel = min_usable_max_channel;
			}

			/* Compute the rest.  */
			settings->min_amount = trigger_factor
					     * settings->min_channel
					     ;
			settings->min_remaining = settings->min_amount
						+ additional_remaining
						;

			/* The ChannelCreator Planner asserts
			 *   min_channel + min_remaining <= max_channel
			 * at construction, so a violating config would
			 * abort on the first channel-creation run.
			 * max_channel is the knob that sets typical
			 * channel size: keep it, and lower min_channel
			 * to the largest value that fits.  */
			if ( settings->min_channel + settings->min_remaining
			   > settings->max_channel
			   ) {
				auto lowered = Ln::Amount::sat(
					( settings->max_channel
					- additional_remaining
					).to_sat()
					/ (std::uint64_t)(1.0 + trigger_factor)
				);
				act += Boss::log( bus, Warn
						, "AmountSettingsHandler: "
						  "--clboss-min-channel %u "
						  "and --clboss-max-channel %u "
						  "conflict (max must be at "
						  "least %u), "
						  "--clboss-min-channel "
						  "forced to %u."
						, (unsigned int)
						  settings->min_channel.to_sat()
						, (unsigned int)
						  settings->max_channel.to_sat()
						, (unsigned int)
						  ( settings->min_channel
						  + settings->min_remaining
						  ).to_sat()
						, (unsigned int)
						  lowered.to_sat()
						);
				settings->min_channel = lowered;
				settings->min_amount = trigger_factor
						     * settings->min_channel
						     ;
				settings->min_remaining = settings->min_amount
							+ additional_remaining
							;
			}

			/* Grab the settings and send it.  */
			auto msg = std::move(settings);
			return act + bus.raise(std::move(*msg));
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl( S::Bus& bus_
	    ) : bus(bus_)
	      , settings(Util::make_unique<Msg::AmountSettings>())
	      { start(); }
};

AmountSettingsHandler::~AmountSettingsHandler() =default;
AmountSettingsHandler::AmountSettingsHandler(AmountSettingsHandler&&) =default;

AmountSettingsHandler::AmountSettingsHandler(S::Bus& bus) : pimpl(Util::make_unique<Impl>(bus)) {}

}}
