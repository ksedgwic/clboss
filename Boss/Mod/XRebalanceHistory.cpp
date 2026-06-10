#include"Boss/Mod/XRebalanceHistory.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/Msg/XRebalanceObservation.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include<cinttypes>
#include<cstdint>
#include<string>

namespace {

/* Default retention: one week of observations.  */
auto constexpr default_history_age_secs = std::uint64_t(604800);

char const*
kind_cstr(Boss::Msg::XRebalanceObservationKind kind) {
	switch (kind) {
	case Boss::Msg::XRebalanceObservationKind::Success:
		return "success";
	case Boss::Msg::XRebalanceObservationKind::LiquidityFail:
		return "liquidity_fail";
	case Boss::Msg::XRebalanceObservationKind::PolicyFail:
		return "policy_fail";
	case Boss::Msg::XRebalanceObservationKind::NodeFail:
		return "node_fail";
	}
	return "unknown";
}

}

namespace Boss { namespace Mod {

class XRebalanceHistory::Impl {
private:
	S::Bus& bus;
	std::function<double()> get_now;
	Sqlite3::Db db;

	std::uint64_t history_age_secs;

	void start() {
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& r) {
			db = r.db;
			return init();
		});
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const&) {
			return bus.raise(Msg::ManifestCommand{
				"clboss-xrebalance-history",
				"[scid] [hours]",
				"Show recorded xrebalance liquidity "
				"observations, oldest first per channel "
				"direction.  Optional {scid} restricts to "
				"one channel (both directions); optional "
				"{hours} restricts to the most recent "
				"{hours} hours.",
				false
			}) + bus.raise(Msg::ManifestOption{
				"clboss-xrebalance-history-age-secs",
				Msg::OptionType_Int,
				Json::Out::direct(default_history_age_secs),
				"Retention (seconds) for xrebalance "
				"liquidity observations; rows older than "
				"this are trimmed once per "
				"TimerRandomHourly tick.  This history is "
				"the evidence base for the persistence "
				"forecaster, so it is normally much longer "
				"than clboss-xrebalance-age-secs.  Dynamic: "
				"settable at runtime via `lightning-cli "
				"setconfig`.  Default 604800 (1 week).",
				/* dynamic = */ true
			});
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != "clboss-xrebalance-history-age-secs")
				return Ev::lift();
			/* At startup lightningd sends Int options as a
			 * JSON number primitive; at runtime the setconfig
			 * path encodes the value as a JSON string.
			 * Tolerate both (same as
			 * clboss-xrebalance-age-secs).  */
			auto secs = std::uint64_t(0);
			try {
				if (o.value.is_number()) {
					secs = std::uint64_t(double(o.value));
				} else if (o.value.is_string()) {
					secs = std::stoull(
					    std::string(o.value));
				} else {
					return Boss::log( bus, Warn
							, "XRebalanceHistory: "
							  "clboss-xrebalance-"
							  "history-age-secs: "
							  "unsupported value "
							  "type; keeping "
							  "%" PRIu64 "."
							, history_age_secs
							);
				}
			} catch (std::exception const& e) {
				return Boss::log( bus, Warn
						, "XRebalanceHistory: clboss-"
						  "xrebalance-history-age-secs: "
						  "parse error '%s'; "
						  "keeping %" PRIu64 "."
						, e.what()
						, history_age_secs
						);
			}
			if (secs == 0) {
				return Boss::log( bus, Warn
						, "XRebalanceHistory: clboss-"
						  "xrebalance-history-age-secs: "
						  "must be > 0; keeping "
						  "%" PRIu64 "."
						, history_age_secs
						);
			}
			history_age_secs = secs;
			return Boss::log( bus, Info
					, "XRebalanceHistory: retention = "
					  "%" PRIu64 " seconds"
					, history_age_secs
					);
		});
		bus.subscribe<Msg::XRebalanceObservation
			     >([this](Msg::XRebalanceObservation const& o) {
			if (!db)
				return Ev::lift();
			return record(o);
		});
		bus.subscribe<Msg::TimerRandomHourly
			     >([this](Msg::TimerRandomHourly const&) {
			if (!db)
				return Ev::lift();
			return trim();
		});
		bus.subscribe<Msg::CommandRequest
			     >([this](Msg::CommandRequest const& req) {
			if (req.command != "clboss-xrebalance-history")
				return Ev::lift();
			return report(req);
		});
		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const&) {
			if (!db)
				return Ev::lift();
			return status();
		});
	}

	Ev::Io<void> init() {
		return db.transact().then([](Sqlite3::Tx tx) {
			tx.query_execute(R"QRY(
			CREATE TABLE IF NOT EXISTS "XRebalanceHistory"
			     ( time INTEGER NOT NULL -- unix seconds
			     , scid TEXT NOT NULL
			     , dir INTEGER NOT NULL -- askrene direction 0/1
			     , kind TEXT NOT NULL
			       -- success | liquidity_fail
			       -- | policy_fail | node_fail
			     , amount_msat INTEGER NOT NULL
			     , failcode INTEGER -- NULL for success
			     , erring_node TEXT -- NULL for success
			     );
			CREATE INDEX IF NOT EXISTS
			    idx_xrebalancehistory_scid_dir_time
			    ON "XRebalanceHistory" (scid, dir, time);
			CREATE INDEX IF NOT EXISTS
			    idx_xrebalancehistory_time
			    ON "XRebalanceHistory" (time);
			)QRY");
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> record(Msg::XRebalanceObservation const& o) {
		return db.transact().then([o](Sqlite3::Tx tx) {
			auto q = tx.query(R"QRY(
			INSERT INTO "XRebalanceHistory"
			VALUES( :time, :scid, :dir, :kind
			      , :amount, :failcode, :erring_node);
			)QRY");
			q
				.bind(":time", o.time)
				.bind(":scid", std::string(o.scid))
				.bind(":dir", o.dir)
				.bind(":kind", kind_cstr(o.kind))
				.bind(":amount", o.amount.to_msat())
				;
			if (o.kind == Msg::XRebalanceObservationKind
					::Success)
				q.bind(":failcode", nullptr);
			else
				q.bind(":failcode", int(o.failcode));
			if (o.erring_node)
				q.bind( ":erring_node"
				      , std::string(o.erring_node));
			else
				q.bind(":erring_node", nullptr);
			q.execute();
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> trim() {
		auto cutoff = std::uint64_t(get_now())
			    - history_age_secs;
		return db.transact().then([this, cutoff](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			DELETE FROM "XRebalanceHistory"
			 WHERE time < :cutoff;
			)QRY")
				.bind(":cutoff", cutoff)
				.execute()
				;
			auto removed = std::int64_t(0);
			auto fetch = tx.query(
				"SELECT changes() AS n;"
			).execute();
			for (auto& r : fetch)
				removed = r.get<std::int64_t>(0);
			tx.commit();
			if (removed == 0)
				return Ev::lift();
			return Boss::log( bus, Debug
					, "XRebalanceHistory: trimmed "
					  "%" PRId64 " observation(s) older "
					  "than %" PRIu64 "."
					, removed
					, cutoff
					);
		});
	}

	Ev::Io<void> report(Msg::CommandRequest const& req) {
		auto id = req.id;
		auto paramfail = [this, id]() {
			return bus.raise(Msg::CommandFail{
				id, -32602,
				"Parameter failure",
				Json::Out::empty_object()
			});
		};

		/* Optional filters: a string parameter is the scid, a
		 * number parameter is the hours window.  Accept either
		 * keyword or positional form.  */
		auto scid = std::string("");
		auto hours = double(0.0);
		auto scid_j = Jsmn::Object();
		auto hours_j = Jsmn::Object();
		auto params = req.params;
		if (params.is_object()) {
			if (params.size() > 2)
				return paramfail();
			for (auto key : {"scid", "hours"}) {
				if (params.has(key)) {
					if (std::string(key) == "scid")
						scid_j = params[key];
					else
						hours_j = params[key];
				}
			}
			/* Reject unknown keys.  */
			auto known = std::size_t(0);
			if (!scid_j.is_null()) ++known;
			if (!hours_j.is_null()) ++known;
			if (params.size() != known)
				return paramfail();
		} else if (params.is_array()) {
			if (params.size() > 2)
				return paramfail();
			for (auto p : params) {
				if (p.is_string() && scid_j.is_null())
					scid_j = p;
				else if (p.is_number() && hours_j.is_null())
					hours_j = p;
				else
					return paramfail();
			}
		}
		if (!scid_j.is_null()) {
			if (!scid_j.is_string())
				return paramfail();
			scid = std::string(scid_j);
			if (!Ln::Scid::valid_string(scid))
				return paramfail();
		}
		if (!hours_j.is_null()) {
			if (!hours_j.is_number())
				return paramfail();
			hours = double(hours_j);
			if (hours <= 0)
				return paramfail();
		}

		auto cutoff = std::uint64_t(0);
		if (hours > 0) {
			auto window = std::uint64_t(hours * 3600.0);
			auto now = std::uint64_t(get_now());
			cutoff = (window < now) ? (now - window)
						: std::uint64_t(0);
		}

		return db.transact().then([id, scid, cutoff, this
					  ](Sqlite3::Tx tx) {
			auto sql = std::string(R"QRY(
			SELECT time, scid, dir, kind, amount_msat
			     , COALESCE(failcode, -1)
			     , COALESCE(erring_node, '')
			  FROM "XRebalanceHistory"
			 WHERE time >= :cutoff
			)QRY");
			if (!scid.empty())
				sql += " AND scid = :scid";
			sql += " ORDER BY scid, dir, time;";
			auto q = tx.query(sql.c_str());
			q.bind(":cutoff", cutoff);
			if (!scid.empty())
				q.bind(":scid", scid);
			auto fetch = q.execute();

			auto out = Json::Out();
			auto obj = out.start_object();
			auto arr = obj.start_array("observations");
			for (auto& r : fetch) {
				auto ndx = std::size_t(0);
				auto time = r.get<std::uint64_t>(ndx++);
				auto row_scid = r.get<std::string>(ndx++);
				auto dir = r.get<std::uint32_t>(ndx++);
				auto kind = r.get<std::string>(ndx++);
				auto amount = r.get<std::uint64_t>(ndx++);
				auto failcode = r.get<std::int64_t>(ndx++);
				auto enode = r.get<std::string>(ndx++);
				auto row = arr.start_object();
				row
					.field("time", time)
					.field("scid", row_scid)
					.field("dir", dir)
					.field("kind", kind)
					.field("amount_msat", amount)
					;
				if (failcode >= 0)
					row.field( "failcode"
						 , std::uint64_t(failcode));
				if (!enode.empty())
					row.field("erring_node", enode);
				row.end_object();
			}
			arr.end_array();
			obj.end_object();
			tx.commit();

			return bus.raise(Msg::CommandResponse{
				id, std::move(out)
			});
		});
	}

	Ev::Io<void> status() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
			SELECT COUNT(*)
			     , COUNT(DISTINCT scid || '/' || dir)
			     , COALESCE(MIN(time), 0)
			     , COALESCE(MAX(time), 0)
			  FROM "XRebalanceHistory";
			)QRY").execute();

			auto out = Json::Out();
			auto obj = out.start_object();
			for (auto& r : fetch) {
				auto count = r.get<std::uint64_t>(0);
				auto chandirs = r.get<std::uint64_t>(1);
				auto oldest = r.get<std::uint64_t>(2);
				auto newest = r.get<std::uint64_t>(3);
				obj
					.field("observations", count)
					.field("channel_directions", chandirs)
					.field("oldest_time", oldest)
					.field("newest_time", newest)
					;
			}
			obj.end_object();
			tx.commit();

			return bus.raise(Msg::ProvideStatus{
				"xrebalance_history",
				std::move(out)
			});
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl(S::Bus& bus_, std::function<double()> get_now_)
		: bus(bus_)
		, get_now(std::move(get_now_))
		, history_age_secs(default_history_age_secs) { start(); }
};

XRebalanceHistory::XRebalanceHistory(XRebalanceHistory&&) =default;
XRebalanceHistory::~XRebalanceHistory() =default;

XRebalanceHistory::XRebalanceHistory( S::Bus& bus
				    , std::function<double()> get_now_
				    )
	: pimpl(Util::make_unique<Impl>(bus, get_now_)) { }

}}
