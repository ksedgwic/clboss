#include"Boss/Mod/RebalanceModeManager.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RequestRebalanceMode.hpp"
#include"Boss/Msg/ResponseRebalanceMode.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/RebalanceMode.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"

namespace {

/* Single dynamic option: the config file sets the startup default and
 * `setconfig clboss-rebalance-mode <mode>` switches it at runtime.  */
auto const option_name = std::string("clboss-rebalance-mode");

}

namespace Boss { namespace Mod {

class RebalanceModeManager::Impl {
private:
	S::Bus& bus;

	RebalanceMode mode;

	void start() {
		mode = default_rebalance_mode;

		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return bus.raise(Msg::ManifestOption{
				option_name,
				Msg::OptionType_String,
				Json::Out::direct(std::string(
					rebalance_mode_to_string(
						default_rebalance_mode
					)
				)),
				"Rebalancer mode: \"classic\" (the original "
				"rebalancer), \"xrebalance\" (the circular askrene "
				"rebalancer, tuned by clboss-xrebalance-*), or "
				"\"off\" (disable rebalancing). "
				" Set in the config for the startup default or "
				"at runtime with "
				"`setconfig clboss-rebalance-mode <mode>`.",
				true /* dynamic: runtime-settable via setconfig */
			});
		});

		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != option_name)
				return Ev::lift();
			auto s = std::string(o.value);
			auto m = RebalanceMode();
			if (!rebalance_mode_from_string(s, m))
				return Boss::log( bus, Error
						, "RebalanceModeManager: "
						  "ignoring unrecognized "
						  "%s value \"%s\"; "
						  "keeping \"%s\"."
						, option_name.c_str()
						, s.c_str()
						, rebalance_mode_to_string(mode)
						);
			if (m == mode)
				return Ev::lift();
			mode = m;
			return Boss::log( bus, Info
					, "RebalanceModeManager: "
					  "mode set to \"%s\"."
					, rebalance_mode_to_string(mode)
					);
		});

		bus.subscribe<Msg::RequestRebalanceMode
			     >([this](Msg::RequestRebalanceMode const& m) {
			return bus.raise(Msg::ResponseRebalanceMode{
				m.requester, mode
			});
		});

		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const& _) {
			auto out = Json::Out();
			out.start_object()
				.field("mode", std::string(rebalance_mode_to_string(mode)))
				.end_object()
				;
			return bus.raise(Msg::ProvideStatus{
				"rebalance_mode", std::move(out)
			});
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl(S::Bus& bus_) : bus(bus_) {
		start();
	}
};

RebalanceModeManager::RebalanceModeManager(RebalanceModeManager&&) =default;
RebalanceModeManager::~RebalanceModeManager() =default;

RebalanceModeManager::RebalanceModeManager(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
