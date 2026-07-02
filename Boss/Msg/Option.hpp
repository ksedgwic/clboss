#ifndef BOSS_MSG_OPTION_HPP
#define BOSS_MSG_OPTION_HPP

#include"Jsmn/Object.hpp"
#include<memory>
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
	/* Rejection back-channel, non-null only on the setconfig path
	 * (Boss::Mod::SetConfigHandler allocates it; init-time Option
	 * raises leave it null).  The subscriber that owns `name` and
	 * rejects `value` reports the reason via reject();
	 * SetConfigHandler then fails the setconfig command.  Failing
	 * matters beyond cosmetics: lightningd persists a setconfig
	 * value (configvar_save) only on a success response, so
	 * acking a rejected value records it in config.setconfig and
	 * listconfigs forever -- and a non-numeric value persisted
	 * for an int-typed option even fails lightningd's own option
	 * parse on the NEXT start.  Owners that accept the value
	 * leave it untouched.
	 */
	std::shared_ptr<std::string> reject_reason;

	/* For the owning subscriber: report rejection of a dynamic
	 * update.  No-op at init time (null reject_reason), where
	 * quietly keeping the default is the correct behaviour.
	 */
	void reject(std::string reason) const {
		if (reject_reason)
			*reject_reason = std::move(reason);
	}
};

}}

#endif /* !defined(BOSS_MSG_OPTION_HPP) */
