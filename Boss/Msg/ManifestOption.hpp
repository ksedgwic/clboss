#ifndef BOSS_MSG_MANIFESTOPTION_HPP
#define BOSS_MSG_MANIFESTOPTION_HPP

#include"Boss/Msg/OptionType.hpp"
#include"Json/Out.hpp"
#include<string>

namespace Boss { namespace Msg {

/** struct Boss::Msg::ManifestOption
 *
 * @brief emit in response to `Boss::Msg::Manifestation` to
 * register an option.
 */
struct ManifestOption {
	std::string name;
	OptionType type;
	Json::Out default_value;
	std::string description;
	/* If true, lightningd will accept `setconfig <name> <val>` at
	 * runtime for this option and forward the new value to clboss
	 * via the `setconfig` JSON-RPC method.  SetConfigHandler turns
	 * that into a fresh Msg::Option on the bus, so existing option
	 * handlers re-apply the new value without a plugin restart.
	 * Default false preserves the original startup-only contract.
	 */
	bool dynamic = false;
};

}}

#endif /* !defined(BOSS_MSG_MANIFESTOPTION_HPP) */
