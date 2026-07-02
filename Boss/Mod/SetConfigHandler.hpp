#ifndef BOSS_MOD_SETCONFIGHANDLER_HPP
#define BOSS_MOD_SETCONFIGHANDLER_HPP

#include<map>
#include<string>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::SetConfigHandler
 *
 * @brief Dispatches `setconfig` JSON-RPC calls from lightningd
 * for options that were registered with `dynamic = true` on their
 * Msg::ManifestOption.
 *
 * Lightningd routes `setconfig <name> <val>` to the plugin that
 * owns the option, as a JSON-RPC method call.  We turn that into
 * a fresh Msg::Option on the bus, so existing option handlers
 * re-apply the new value without a plugin restart.
 *
 * Contract for module authors who mark an option `dynamic = true`:
 * at startup lightningd delivers Int / Bool / Flag option values
 * as JSON primitives, but at setconfig time lightningd encodes the
 * value as a JSON string.  Any module that opts in to dynamic
 * updates MUST tolerate both shapes in its Msg::Option handler --
 * inspect `o.value.is_string()` and parse from the string form
 * when appropriate.
 *
 * A handler that REJECTS a dynamic value keeps its current setting
 * and MUST also report the rejection via Msg::Option::reject(), so
 * the setconfig command fails and lightningd does not persist the
 * value.  A silently-acked rejected value lands in config.setconfig
 * and diverges from the running configuration; a non-numeric value
 * persisted for an int-typed option even fails lightningd's own
 * option parse on the next start.
 */
class SetConfigHandler {
private:
	S::Bus& bus;
	/* Name -> dynamic flag, populated from Msg::ManifestOption
	 * events during the Manifestation phase.  Non-dynamic
	 * options are recorded too so we can return a clearer error
	 * than "unknown option" if lightningd ever forwards a
	 * setconfig for a non-dynamic name (which it should not). */
	std::map<std::string, bool> options;

	void start();

public:
	SetConfigHandler() =delete;
	SetConfigHandler(SetConfigHandler&&) =delete;
	SetConfigHandler(SetConfigHandler const&) =delete;

	explicit
	SetConfigHandler(S::Bus& bus_) : bus(bus_) { start(); }
};

}}

#endif /* !defined(BOSS_MOD_SETCONFIGHANDLER_HPP) */
