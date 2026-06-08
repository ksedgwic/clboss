#include"Boss/Mod/Manifester.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/ManifestHook.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Ev/Io.hpp"
#include"S/Bus.hpp"
#include<stdlib.h>

namespace {

std::string option_type_name(Boss::Msg::OptionType t) {
	switch (t) {
	case Boss::Msg::OptionType_String: return "string";
	case Boss::Msg::OptionType_Bool: return "bool";
	case Boss::Msg::OptionType_Int: return "int";
	case Boss::Msg::OptionType_Flag: return "flag";
	}
	abort();
}

}

namespace Boss { namespace Mod {

void Manifester::start() {
	bus.subscribe<Boss::Msg::CommandRequest>([this](Boss::Msg::CommandRequest const& req) {
		if (req.command != "getmanifest")
			return Ev::lift();

		auto id = req.id;
		return Ev::lift().then([this]() {
			return bus.raise(Boss::Msg::Manifestation());
		}).then([this, id]() {
			/* At this point, other modules should have
			 * emitted the Manifest registration.  */
			auto result = Json::Out();
			auto robj = result.start_object();
			robj
				.field("dynamic", true)
				;

			/* Holy carp.
			 * The C-lightning document describes `"features"`,
			 * but the actual field is named `"featurebits"`.  */

			auto harr = robj.start_array("hooks");
			for (auto const& h : hooks)
				harr.entry(h);
			harr.end_array();

			auto narr = robj.start_array("subscriptions");
			for (auto const& n : notifications)
				narr.entry(n);
			narr.end_array();

			auto carr = robj.start_array("rpcmethods");
			for (auto const& c : commands) {
				auto const& info = c.second;
				carr.entry(
					Json::Out()
					.start_object()
						.field("name", info.name)
						.field("usage", info.usage)
						.field("description", info.description)
						.field("deprecated", info.deprecated)
					.end_object()
				);
			}
			carr.end_array();

			auto oarr = robj.start_array("options");
			for (auto const& n_o : options) {
				auto const& o = n_o.second;
				oarr.entry(
					Json::Out()
					.start_object()
						.field("name", o.name)
						.field("type"
						      , option_type_name(
								o.type
							)
						      )
						.field( "default"
						      , o.default_value
						      )
						.field( "description"
						      , o.description
						      )
						.field( "dynamic"
						      , o.dynamic
						      )
					.end_object()
				);
			}
			oarr.end_array();

			robj.end_object();

			/* Now the manifester has created the result,
			 * we can clear the objects.
			 */
			hooks.clear();
			notifications.clear();
			commands.clear();

			return bus.raise(Boss::Msg::CommandResponse{
				id, result
			});
		});
	});
	/* Registrations.  */
	bus.subscribe<Boss::Msg::ManifestCommand>([this](Boss::Msg::ManifestCommand const& c) {
		commands[c.name] = c;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::ManifestHook>([this](Boss::Msg::ManifestHook const& h) {
		hooks.insert(h.name);
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::ManifestNotification>([this](Boss::Msg::ManifestNotification const& n) {
		notifications.insert(n.name);
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::ManifestOption>([this](Boss::Msg::ManifestOption const& o) {
		options[o.name] = o;
		return Ev::lift();
	});
}

}}
