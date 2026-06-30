#include"Boss/Mod/AskreneVolatileLayer.hpp"
#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<cinttypes>

namespace {

/* Default seconds between wholesale wipes of the clboss-volatile layer.  */
auto constexpr default_wipe_secs = std::uint64_t(10800); /* 3h */

}

namespace Boss { namespace Mod {

class AskreneVolatileLayer::Impl {
private:
	S::Bus& bus;
	Boss::Mod::Rpc* rpc;

	/* Seconds between wipes; dynamic via clboss-volatile-layer-wipe-secs.  */
	std::uint64_t wipe_secs;
	/* Ev::now() at the last (re)create.  The first wipe happens one
	 * interval after startup, so the freshly-created layer is given time
	 * to accumulate before being wiped.  */
	double last_wipe;

	void start() {
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			rpc = &init.rpc;
			last_wipe = Ev::now();
			return Boss::concurrent(create_layer());
		});

		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const&) {
			return bus.raise(Msg::ManifestOption{
				"clboss-volatile-layer-wipe-secs",
				Msg::OptionType_Int,
				Json::Out::direct(wipe_secs),
				"Seconds between wholesale wipes of the shared "
				"clboss-volatile askrene layer (the never-aged "
				"node disables and channel-update overrides that "
				"the rebalancers write).  Once the interval "
				"elapses the layer is removed and recreated on a "
				"TimerRandomHourly tick, so blocks re-accumulate "
				"from fresh failures and a recovered node or "
				"channel becomes routable again within one "
				"interval.  Dynamic: settable at runtime via "
				"`lightning-cli setconfig "
				"clboss-volatile-layer-wipe-secs <secs>`.  "
				"Default 10800 (3h).",
				/* dynamic = */ true
			});
		});
		bus.subscribe<Msg::Option>([this](Msg::Option const& o) {
			if (o.name != "clboss-volatile-layer-wipe-secs")
				return Ev::lift();
			/* Number at startup, string via setconfig -- the same
			 * dual encoding the other dynamic options handle.
			 * Signed so a negative value is rejected below rather
			 * than wrapping to a huge unsigned. */
			long long secs = 0;
			try {
				if (o.value.is_number()) {
					secs = static_cast<long long>(double(o.value));
				} else if (o.value.is_string()) {
					secs = std::stoll(std::string(o.value));
				} else {
					return Boss::log( bus, Warn
							, "AskreneVolatileLayer: "
							  "clboss-volatile-layer-wipe-"
							  "secs: unsupported value "
							  "type; keeping %" PRIu64 "."
							, wipe_secs
							);
				}
			} catch (std::exception const& e) {
				return Boss::log( bus, Warn
						, "AskreneVolatileLayer: "
						  "clboss-volatile-layer-wipe-secs: "
						  "parse error '%s'; keeping "
						  "%" PRIu64 "."
						, e.what()
						, wipe_secs
						);
			}
			if (secs <= 0) {
				return Boss::log( bus, Warn
						, "AskreneVolatileLayer: "
						  "clboss-volatile-layer-wipe-secs: "
						  "must be > 0; keeping %" PRIu64 "."
						, wipe_secs
						);
			}
			wipe_secs = std::uint64_t(secs);
			return Boss::log( bus, Info
					, "AskreneVolatileLayer: wipe interval "
					  "set to %" PRIu64 "s."
					, wipe_secs
					);
		});

		bus.subscribe<Msg::TimerRandomHourly
			     >([this](Msg::TimerRandomHourly const&) {
			if (!rpc)
				return Ev::lift();
			auto now = Ev::now();
			if (now - last_wipe < double(wipe_secs))
				return Ev::lift();
			last_wipe = now;
			return Boss::concurrent(wipe_layer());
		});
	}

	/* (Re)create the non-persistent volatile layer.  Non-fatal on
	 * RpcError: on CLN < v24.11 (no askrene) the layer simply does not
	 * exist and the rebalancers' block-writes silently no-op.  */
	Ev::Io<void> create_layer() {
		auto parms = Json::Out()
			.start_object()
				.field( "layer"
				      , Boss::Mod::AskreneLayer::clboss_volatile_layer_name
				      )
				.field("persistent", false)
			.end_object()
			;
		return rpc->command( "askrene-create-layer"
				   , std::move(parms)
				   ).then([](Jsmn::Object) {
			return Ev::lift();
		}).catching<RpcError>([this](RpcError const& e) {
			auto code = int(0);
			if (e.error.has("code") && e.error["code"].is_number())
				code = int(double(e.error["code"]));
			auto is_method_missing = (code == -32601);
			return Boss::log( bus
					, is_method_missing ? Debug : Warn
					, "AskreneVolatileLayer: "
					  "askrene-create-layer (%s) failed: "
					  "%s%s"
					, Boss::Mod::AskreneLayer::clboss_volatile_layer_name
						.c_str()
					, Util::stringify(e.error).c_str()
					, is_method_missing
						? " (RPC missing; volatile layer "
						  "unavailable on this CLN)."
						: " (unexpected)."
					);
		});
	}

	/* Wipe = remove + recreate.  Single fixed-name layer, so a getroutes
	 * landing in the brief gap simply misses the blocks for that one call
	 * -- benign and self-correcting (the next failure re-records them).
	 * The remove is allowed to fail (the layer may not exist yet).  */
	Ev::Io<void> wipe_layer() {
		auto parms = Json::Out()
			.start_object()
				.field( "layer"
				      , Boss::Mod::AskreneLayer::clboss_volatile_layer_name
				      )
			.end_object()
			;
		return rpc->command( "askrene-remove-layer"
				   , std::move(parms)
				   ).then([](Jsmn::Object) {
			return Ev::lift();
		}).catching<RpcError>([](RpcError const&) {
			/* Layer absent (first wipe, or a prior create failed);
			 * the recreate below establishes it regardless.  */
			return Ev::lift();
		}).then([this]() {
			return create_layer();
		}).then([this]() {
			return Boss::log( bus, Debug
					, "AskreneVolatileLayer: wiped %s "
					  "(blocks re-accumulate from fresh "
					  "failures)."
					, Boss::Mod::AskreneLayer::clboss_volatile_layer_name
						.c_str()
					);
		});
	}

public:
	explicit
	Impl(S::Bus& bus_
	    ) : bus(bus_)
	      , rpc(nullptr)
	      , wipe_secs(default_wipe_secs)
	      , last_wipe(0.0)
	      { start(); }
};

AskreneVolatileLayer::AskreneVolatileLayer(AskreneVolatileLayer&&) =default;
AskreneVolatileLayer::~AskreneVolatileLayer() =default;

AskreneVolatileLayer::AskreneVolatileLayer(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
