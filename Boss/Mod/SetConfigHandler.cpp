#include"Boss/Mod/SetConfigHandler.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"

namespace {

/* JSON-RPC error code for malformed parameters, matching the
 * JSONRPC2 invalid-params constant used elsewhere in clboss. */
constexpr int RPC_INVALID_PARAMS = -32602;

}

namespace Boss { namespace Mod {

void SetConfigHandler::start() {
	bus.subscribe<Boss::Msg::ManifestOption
		     >([this](Boss::Msg::ManifestOption const& o) {
		options[o.name] = o.dynamic;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::CommandRequest
		     >([this](Boss::Msg::CommandRequest const& m) {
		if (m.command != "setconfig")
			return Ev::lift();

		auto id = m.id;
		auto const& params = m.params;

		/* Extract `config` (required string). */
		if (!params.is_object() || !params.has("config")
		 || !params["config"].is_string()) {
			return bus.raise(Boss::Msg::CommandFail{
				id, RPC_INVALID_PARAMS,
				"setconfig: missing or non-string 'config' "
				"parameter",
				Json::Out::empty_object()
			});
		}
		auto name = std::string(params["config"]);

		/* Verify the option is one we registered, and is
		 * declared dynamic.  Lightningd should never forward
		 * setconfig for a non-dynamic option (libplugin would
		 * refuse it on the receiving side too), but defending
		 * here keeps the error surface clear and prevents a
		 * surprise Msg::Option re-raise for an option whose
		 * handler may not expect runtime updates. */
		auto it = options.find(name);
		if (it == options.end()) {
			return bus.raise(Boss::Msg::CommandFail{
				id, RPC_INVALID_PARAMS,
				"setconfig: unknown option '" + name + "'",
				Json::Out::empty_object()
			});
		}
		if (!it->second) {
			return bus.raise(Boss::Msg::CommandFail{
				id, RPC_INVALID_PARAMS,
				"setconfig: option '" + name
				+ "' is not dynamic",
				Json::Out::empty_object()
			});
		}

		/* Forward the value as-is.  Lightningd encodes the new
		 * value as a JSON string (see plugin_set_dynamic_opt in
		 * cln/lightningd/plugin.c), so handlers will see a
		 * Jsmn::Object with is_string() == true here even for
		 * numeric option types.  The contract documented on
		 * SetConfigHandler covers this.
		 *
		 * Note: bus.raise(Msg::Option) broadcasts to every
		 * Msg::Option subscriber, not just the one that owns
		 * this option.  Subscribers must filter by name and
		 * no-op on non-matches -- see the doc comment on
		 * Boss::Msg::Option for the full contract. */
		auto value = params.has("val")
			? params["val"]
			: Jsmn::Object();
		return Boss::log( bus, Debug
				, "SetConfigHandler: dispatching setconfig "
				  "'%s'"
				, name.c_str()
				)
		     + bus.raise(Boss::Msg::Option{name, std::move(value)})
		     + bus.raise(Boss::Msg::CommandResponse{
				id, Json::Out::empty_object()
		       });
	});
}

}}
