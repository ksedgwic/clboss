#ifndef BOSS_MODG_TIMEDBOOLEAN_HPP
#define BOSS_MODG_TIMEDBOOLEAN_HPP

#include"Boss/ModG/TimedBoolean.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RequestGetAutoOpenFlag.hpp"
#include"Boss/Msg/ResponseGetAutoOpenFlag.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/log.hpp"
#include<cctype>
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Ev/now.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/date.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<vector>

namespace Boss { namespace ModG {

template <typename RequestMsg, typename ResponseMsg>
class TimedBoolean {
private:
	struct DbState {
		bool state = true;
		bool temp_state = true;
		double temp_until = 0;
	};

	S::Bus& bus;
	Sqlite3::Db db;

	const std::string controller_name;
	const std::string option_name;
	const std::string description_name;

	// Used to save a state set by an option on startup until we can
	// write it to the database.
	bool have_pending_db_write;
	bool pending_db_state;

	std::vector<void*> pending_requesters;

	std::string bool_str(bool b) {
		return b ? "true" : "false";
	}

	std::string enabled_str(bool b) {
		return b ? "enabled" : "disabled";
	}

	std::string not_str(bool b) {
		return b ? "" : "not ";
	}

	std::string capitalise(std::string s) {
		if (s.length() > 0)
			s[0] = static_cast<char>(std::toupper(
				static_cast<unsigned char>(s[0])));
		return s;
	}

	std::string replace(std::string s, char from, char to) {
		std::replace(s.begin(), s.end(), from, to);
		return s;
	}

public:
	TimedBoolean() =delete;
	TimedBoolean(TimedBoolean&&) =delete;
	TimedBoolean(TimedBoolean const&) =delete;

	TimedBoolean(S::Bus& bus_
		, const std::string& controller_name_
		, const std::string& option_name_
		, const std::string& description_name_)
		: bus(bus_)
		, controller_name(controller_name_)
		, option_name(option_name_)
		, description_name(description_name_)
		, have_pending_db_write(false)
		, pending_db_state(false)
		{}

	void start() {
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& m) {
			db = m.db;
			auto pending = std::move(pending_requesters);
			auto res = init_db()
			     + Ev::foreach( [this](void* requester) {
						return get_flag(requester);
					    }
					  , std::move(pending)
					  );
			if (have_pending_db_write) {
				DbState db_state;
				db_state.state = pending_db_state;
				have_pending_db_write = false;
				res += write_db(db_state);
			}
			return res;
		});
		bus.subscribe<RequestMsg
			     >([this
			       ](RequestMsg const& m) {
			if (!db) {
				pending_requesters.push_back(m.requester);
				return Ev::lift();
			}
			return get_flag(m.requester);
		});

		/* Manifestation.  */
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return bus.raise(Msg::ManifestCommand{
				"clboss-temporarily-" + option_name,
                                "[enable] [hours]",
				"Set temporary " + description_name + " "
                                "behaviour for {hours} (default 24 hours).",
				false
			}) + bus.raise(Msg::ManifestCommand{
				"clboss-" + option_name, "[enable]",
				"Set " + description_name + " behaviour, "
				"immediately overriding any temporary setting "
				"in effect from clboss-temporarily-" +
				option_name + ".",
				false
			}) + bus.raise(Msg::ManifestOption{
				"clboss-" + option_name, Msg::OptionType_String,
				Json::Out::direct("-"),
				"Whether CLBOSS should " + description_name +
				".  Set 'true' to enable, 'false' to disable."
			});
		});

		/* Option handling.  */
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != "clboss-" + option_name)
				return Ev::lift();
			std::string value = std::string(o.value);
			if (value == "-")
				return Ev::lift();
			if (value != "true" && value != "false")
				throw std::runtime_error(
					controller_name + ": Invalid parameter '"
					+ value + "' for option '" + o.name + "'.");
			pending_db_state = (value == "true");
			have_pending_db_write = true;
			return Boss::log( bus, Info, "%s: %s %s."
					, controller_name.c_str()
					, capitalise(description_name).c_str()
					, enabled_str(pending_db_state).c_str());
		});

		/* Command handling.  */
		bus.subscribe<Msg::CommandRequest
			     >([this](Msg::CommandRequest const& req) {
			auto id = req.id;
			auto succeed = [this, id]() {
				return bus.raise(Msg::CommandResponse{
					id, Json::Out::empty_object()
				});
			};

			auto act = Ev::lift();
			auto paramfail = [this, id]() {
				return bus.raise(Msg::CommandFail{
					id, -32602,
					"Parameter failure",
					Json::Out::empty_object()
				});
			};
			if (req.command == "clboss-temporarily-" + option_name) {
				auto temp_state = bool(true);
				auto hours = double(24.0);

				auto temp_state_j = Jsmn::Object();
				auto hours_j = Jsmn::Object();
				auto params = req.params;
				if (params.is_object()) {
					if (params.size() > 2)
						return paramfail();
					if (params.has("enable"))
						temp_state_j = params["temp_state"];
					if (params.has("hours"))
						hours_j = params["hours"];
				} else if (params.is_array()) {
					if (params.size() > 2)
						return paramfail();
					if (params.size() >= 1)
						temp_state_j = params[0];
					if (params.size() == 2)
						hours_j = params[1];
				} else
					return paramfail();
				if (!temp_state_j.is_null()) {
					if (!temp_state_j.is_boolean())
						return paramfail();
					temp_state = (bool) temp_state_j;
				}
				if (!hours_j.is_null()) {
					if (!hours_j.is_number())
						return paramfail();
					hours = (double) hours_j;
				}

				act += Ev::lift().then([this](){
					return read_db();
				}).then([this, temp_state, hours](DbState db_state) {
					db_state.temp_state = temp_state;
					db_state.temp_until = Ev::now() + (hours * 3600);
					return write_db(db_state);
				});
				act += Boss::log( bus, Info
						, "%s: Will %sperform %s for "
						  "%f hours starting now."
						, controller_name.c_str()
						, not_str(temp_state).c_str()
						, description_name.c_str()
						, hours);
				act += Ev::lift().then([this]() {
					return read_db();
				}).then([this](DbState db_state) {
					if (db_state.temp_state == db_state.state) {
						return Boss::log( bus, Warn
								, "%s: "
								  "Temporary setting "
								  "is same as "
								  "permanent setting."
								, controller_name.c_str()
								);
					}
					return Ev::lift();
				});
				act += succeed();

			} else if (req.command == "clboss-" + option_name) {
				auto state = bool(true);

				auto state_j = Jsmn::Object();
				auto params = req.params;
				if (params.is_object()) {
					if (params.size() > 1)
						return paramfail();
					if (params.has("enabled"))
						state_j = params["enabled"];
				} else if (params.is_array()) {
					if (params.size() > 1)
						return paramfail();
					if (params.size() >= 1)
						state_j = params[0];
				} else
					return paramfail();
				if (!state_j.is_null()) {
					if (!state_j.is_boolean())
						return paramfail();
					state = (bool) state_j;
				}

				act += Ev::lift().then([this](){
					return read_db();
				}).then([this, state](DbState db_state) {
					db_state.state = state;
					db_state.temp_until = 0;
					return write_db(db_state);
				});
				act += Boss::log( bus, Info
						, "%s: Will %sallow %s."
						, controller_name.c_str()
						, not_str(state).c_str()
						, description_name.c_str()
						);
				act += succeed();
			}
			return act;
		});

		/* clboss-status */
		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const& _) {
			return Ev::lift().then([this]() {
				return read_db();
			}).then([this](const DbState& db_state) {
				auto now = Ev::now();
				auto status = (now < db_state.temp_until)
					? db_state.temp_state : db_state.state;
				auto comment = std::string();

				auto stats = Json::Out();
				auto stats_obj = stats.start_object()
						.field( "enabled"
						      , bool_str(status)
						      );
				// There can be quite a few TimedBoolean objects
				// so we avoid spamming the status with details
				// related to temporary states unless one is in
				// effect.
				if ((now < db_state.temp_until)
				    && (db_state.temp_state != db_state.state)) {
					stats_obj
						.field( "base_state"
						      , bool_str(db_state.state)
						      )
						.field( "temporary_state"
						      , bool_str(db_state.temp_state)
						      )
						.field( "now_human"
						      , Util::date(now)
						      )
						.field( "temporary_state_until_human"
						      , Util::date(
								db_state.temp_until
							)
						      );
					comment = "`clboss-" + option_name + " " + bool_str(db_state.state) + "` to cancel temporary override.";
				} else {
					comment = "`clboss-temporarily-" + option_name + " " + bool_str(!db_state.state) + "` to temporarily " + std::string(!db_state.state ? "enable" : "disable") + " " + description_name;
				}
				stats_obj
					.field("comment", comment)
					.end_object()
					;
				return bus.raise(Msg::ProvideStatus{
					"should_" + replace(option_name, '-', '_'),
					std::move(stats)
				});
			});
		});
	};

	Ev::Io<void> init_db() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			tx.query_execute(R"QRY(
			CREATE TABLE IF NOT EXISTS ")QRY" + controller_name + R"QRY("
			     ( id INTEGER PRIMARY KEY
			     , state INTEGER NOT NULL
			     , tempstate INTEGER NOT NULL
			     , tempuntil REAL NOT NULL
			     );
			INSERT OR IGNORE INTO ")QRY" + controller_name + R"QRY("
			VALUES(1, TRUE, TRUE, 0.0);
			)QRY");
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<DbState> read_db() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
			SELECT state, tempstate, tempuntil FROM ")QRY"
			+ controller_name + R"QRY(";)QRY").execute();
			DbState db_state;
			for (auto& r : fetch) {
				db_state.state = r.get<bool>(0);
				db_state.temp_state = r.get<bool>(1);
				db_state.temp_until = r.get<double>(2);
				break;
			}
			tx.commit();

			return Ev::lift(std::move(db_state));
		});
	}

	Ev::Io<void> write_db(const DbState& db_state) {
		return db.transact().then([this, db_state](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			UPDATE ")QRY" + controller_name + R"QRY("
			   SET state = :state,
			       tempstate = :tempstate,
			       tempuntil = :tempuntil;
			)QRY")
				.bind(":state", db_state.state)
				.bind(":tempstate", db_state.temp_state)
				.bind(":tempuntil", db_state.temp_until)
				.execute()
				;
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> get_flag(void* requester) {
		return read_db().then([requester, this
					       ](const DbState& db_state) {
			auto now = Ev::now();

			auto msg = ResponseMsg();
			// We avoid generating a confusing message about a
			// temporary state that is the same as the state we'll
			// revert to later.
			if ((now < db_state.temp_until)
			    && (db_state.temp_state != db_state.state)) {
				msg.state = db_state.temp_state;
				msg.comment =
					capitalise(description_name) + " is "
					+ enabled_str(db_state.temp_state)
					+ " for "
					+ Util::stringify(static_cast<int>(
						db_state.temp_until - now))
					+ " seconds";
			} else {
				msg.state = db_state.state;
				msg.comment =
					capitalise(description_name) + " is "
					+ enabled_str(db_state.state);
			}
			msg.requester = requester;
			return bus.raise(msg);
		});
	}
};

}}

#endif
