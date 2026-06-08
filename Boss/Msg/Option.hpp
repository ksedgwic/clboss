#ifndef BOSS_MSG_OPTION_HPP
#define BOSS_MSG_OPTION_HPP

#include"Jsmn/Object.hpp"
#include<string>

namespace Boss { namespace Msg {

/** struct Boss::Msg::Option
 *
 * @brief providing the value of an option we registered.
 *
 * Emitted during `init` handling (one Msg::Option per option
 * lightningd actually carried in the init request), AND re-
 * emitted by Boss::Mod::SetConfigHandler at runtime when
 * lightningd forwards a `setconfig` JSON-RPC call for an option
 * we registered as `dynamic = true`.
 *
 * Subscribers MUST filter by `name` and no-op for names they do
 * not own (the bus broadcasts to all Msg::Option subscribers, so
 * a dynamic option update for module A will be delivered to
 * module B as well).  Subscribers MUST also tolerate post-init
 * arrival -- any local invariants that were valid only "between
 * Manifestation and EndOfOptions" must be re-checked rather than
 * asserted.
 */
struct Option {
	std::string name;
	Jsmn::Object value;
};

}}

#endif /* !defined(BOSS_MSG_OPTION_HPP) */
