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
/* How much larger the max_channel should be over the min_channel.  */
auto const max_channel_factor = double(2.0);
/* How much larger the channel-creation trigger should be over
 * the min_channel.  */
auto const trigger_factor = double(2.0);
/* How much to add to the channel-creation trigger above, to get
 * the amount to leave after creation.  */
auto const additional_remaining = Ln::Amount::sat(20000);

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
			/* Msg::Option was originally a startup-only signal
			 * (delivered once per registered option between
			 * Manifestation and EndOfOptions), and EndOfOptions
			 * moves `settings` away on line below.  Now that
			 * SetConfigHandler can re-raise Msg::Option at
			 * runtime to deliver setconfig updates, this handler
			 * may receive events for unrelated names long after
			 * `settings` has been moved -- e.g. a runtime
			 * `setconfig` on any dynamic option triggers a
			 * Msg::Option that hits every subscriber.
			 *
			 * Drop those silently: none of the options this
			 * handler owns is registered dynamic, so by the time
			 * we see a post-EndOfOptions event it cannot
			 * legitimately apply.  This also covers a latent
			 * assertion crash that pre-existed dynamic options
			 * (any post-EOO Msg::Option for any name would have
			 * tripped the old assert(settings) here). */
			if (!settings)
				return Ev::lift();
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
			if (settings->max_channel < ( max_channel_factor
						    * settings->min_channel
						    )) {
				settings->max_channel = ( max_channel_factor
							* settings->min_channel
							);
				act += Boss::log( bus, Info
						, "AmountSettingsHandler: "
						  "--clboss-max-channel too "
						  "low, forced to %u."
						, (unsigned int)
						  settings->max_channel.to_sat()
						);
			}

			/* Compute the rest.  */
			settings->min_amount = trigger_factor
					     * settings->min_channel
					     ;
			settings->min_remaining = settings->min_amount
						+ additional_remaining
						;

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
