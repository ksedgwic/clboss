#include"Boss/Mod/XRebalanceHistory.hpp"
#include"Boss/Mod/XRebalancePredict.hpp"
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
#include<map>
#include<string>
#include<utility>
#include<vector>

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
				"{hours} hours.  With {scid}, also reports "
				"what the persistence forecaster would "
				"assert right now for each direction "
				"(predictions block); keyword parameters "
				"{horizon_frac} {horizon_max_secs} "
				"{min_samples} {wall_margin} {floor_factor} "
				"override the prediction defaults for "
				"spot-checking.",
				false
			}) + bus.raise(Msg::ManifestCommand{
				"clboss-xrebalance-predictions",
				"[kind]",
				"Report what the persistence forecaster "
				"would assert right now for EVERY "
				"channel direction in the observation "
				"store.  {kind} selects: walls (wall "
				"side asserts), floors (floor side "
				"asserts), asserting (either side; "
				"default), all (every direction, "
				"including declines with reasons).  The "
				"same keyword parameters as "
				"clboss-xrebalance-history override the "
				"prediction defaults per query.  "
				"Read-only; nothing is written to any "
				"layer.",
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
			if (req.command == "clboss-xrebalance-history")
				return report(req);
			if (req.command == "clboss-xrebalance-predictions")
				return predictions_report(req);
			return Ev::lift();
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

	/* Validates and applies the per-query prediction overrides
	 * shared by the history and predictions commands.  Returns
	 * false on a malformed value.  */
	static bool apply_predict_overrides
		( Jsmn::Object const& frac_j
		, Jsmn::Object const& horizon_max_j
		, Jsmn::Object const& min_samples_j
		, Jsmn::Object const& wall_margin_j
		, Jsmn::Object const& floor_factor_j
		, XRebalancePredict::Params& pparams
		) {
		if (!frac_j.is_null()) {
			if (!frac_j.is_number()
			 || double(frac_j) <= 0)
				return false;
			pparams.horizon_frac = double(frac_j);
		}
		if (!horizon_max_j.is_null()) {
			if (!horizon_max_j.is_number()
			 || double(horizon_max_j) < 0)
				return false;
			pparams.horizon_max_secs =
				std::uint64_t(double(horizon_max_j));
		}
		if (!min_samples_j.is_null()) {
			if (!min_samples_j.is_number()
			 || double(min_samples_j) < 1)
				return false;
			pparams.min_samples =
				std::size_t(double(min_samples_j));
		}
		if (!wall_margin_j.is_null()) {
			if (!wall_margin_j.is_number()
			 || double(wall_margin_j) <= 0)
				return false;
			pparams.wall_margin = double(wall_margin_j);
		}
		if (!floor_factor_j.is_null()) {
			if (!floor_factor_j.is_number())
				return false;
			pparams.floor_factor = double(floor_factor_j);
		}
		return true;
	}

	template<typename Obj>
	static void json_add_prediction_params
		( Obj& obj
		, XRebalancePredict::Params const& pparams
		) {
		auto pp = obj.start_object("prediction_params");
		pp
			.field("horizon_frac", pparams.horizon_frac)
			.field( "horizon_max_secs"
			      , pparams.horizon_max_secs)
			.field( "min_samples"
			      , std::uint64_t(pparams.min_samples))
			.field("wall_margin", pparams.wall_margin)
			.field("floor_factor", pparams.floor_factor)
			;
		pp.end_object();
	}

	/* Emit one per-direction prediction object (shared between
	 * the history command's per-scid block and the fleet-wide
	 * predictions command).  */
	template<typename Arr>
	static void emit_prediction( Arr& arr
				   , std::string const& scid
				   , std::uint32_t dir
				   , XRebalancePredict::Result const& res
				   ) {
		auto po = arr.start_object();
		po
			.field("scid", scid)
			.field("dir", dir)
			.field( "regime_records"
			      , std::uint64_t(res.regime_records))
			.field("regime_span_secs", res.regime_span_secs)
			.field( "regime_start_time"
			      , res.regime_start_time)
			.field("data_age_secs", res.data_age_secs)
			.field("horizon_secs", res.horizon_secs)
			.field("truncated", res.truncated)
			;
		auto put_side = [&po]( char const* name
				     , XRebalancePredict::Side const& s) {
			auto so = po.start_object(name);
			so
				.field("would_assert", s.would_assert)
				.field( "samples"
				      , std::uint64_t(s.samples))
				;
			if (s.has_amount)
				so.field("amount_msat", s.amount_msat);
			if (!s.would_assert)
				so.field( "decline_reason"
					, s.decline_reason);
			so.end_object();
		};
		put_side("wall", res.wall);
		put_side("floor", res.floor);
		po.end_object();
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
		 * keyword or positional form.  Keyword-only extras
		 * override the prediction defaults so a spot-check can
		 * play with the constants per-query.  */
		auto scid = std::string("");
		auto hours = double(0.0);
		auto pparams = XRebalancePredict::default_params;
		auto scid_j = Jsmn::Object();
		auto hours_j = Jsmn::Object();
		auto frac_j = Jsmn::Object();
		auto horizon_max_j = Jsmn::Object();
		auto min_samples_j = Jsmn::Object();
		auto wall_margin_j = Jsmn::Object();
		auto floor_factor_j = Jsmn::Object();
		auto params = req.params;
		if (params.is_object()) {
			auto known = std::size_t(0);
			auto get_key = [&](char const* key) {
				auto r = Jsmn::Object();
				if (params.has(key)) {
					r = params[key];
					++known;
				}
				return r;
			};
			scid_j = get_key("scid");
			hours_j = get_key("hours");
			frac_j = get_key("horizon_frac");
			horizon_max_j = get_key("horizon_max_secs");
			min_samples_j = get_key("min_samples");
			wall_margin_j = get_key("wall_margin");
			floor_factor_j = get_key("floor_factor");
			/* Reject unknown keys.  */
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
		if (!apply_predict_overrides( frac_j, horizon_max_j
					    , min_samples_j, wall_margin_j
					    , floor_factor_j, pparams))
			return paramfail();

		auto cutoff = std::uint64_t(0);
		if (hours > 0) {
			auto window = std::uint64_t(hours * 3600.0);
			auto now = std::uint64_t(get_now());
			cutoff = (window < now) ? (now - window)
						: std::uint64_t(0);
		}

		/* The predictions block is per-channel spot-check
		 * output; emit it only for the single-scid form.  */
		auto want_predictions = !scid.empty();
		auto now = std::uint64_t(get_now());

		return db.transact().then([id, scid, cutoff, pparams,
					   want_predictions, now, this
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

			/* Bounds-relevant rows grouped per direction,
			 * for the predictions block.  node_fail is a
			 * whole-node verdict, not a per-channel
			 * liquidity bound, so it never becomes a
			 * Bound.  */
			auto bounds_by_dir = std::map<
			    std::uint32_t,
			    std::vector<XRebalancePredict::Bound>>();

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

				if (want_predictions) {
					auto is_fail = false;
					if (XRebalancePredict::kind_is_bound(
						kind, is_fail))
						bounds_by_dir[dir].push_back(
						    {time, is_fail, amount});
				}
			}
			arr.end_array();

			if (want_predictions) {
				json_add_prediction_params(obj, pparams);
				auto parr = obj.start_array("predictions");
				for (auto const& e : bounds_by_dir) {
					auto res = XRebalancePredict::predict(
					    e.second, now, pparams);
					emit_prediction( parr, scid
						       , e.first, res);
				}
				parr.end_array();
			}
			obj.end_object();
			tx.commit();

			return bus.raise(Msg::CommandResponse{
				id, std::move(out)
			});
		});
	}

	Ev::Io<void> predictions_report(Msg::CommandRequest const& req) {
		auto id = req.id;
		auto paramfail = [this, id]() {
			return bus.raise(Msg::CommandFail{
				id, -32602,
				"Parameter failure",
				Json::Out::empty_object()
			});
		};

		auto selector = std::string("asserting");
		auto pparams = XRebalancePredict::default_params;
		auto selector_j = Jsmn::Object();
		auto frac_j = Jsmn::Object();
		auto horizon_max_j = Jsmn::Object();
		auto min_samples_j = Jsmn::Object();
		auto wall_margin_j = Jsmn::Object();
		auto floor_factor_j = Jsmn::Object();
		auto params = req.params;
		if (params.is_object()) {
			auto known = std::size_t(0);
			auto get_key = [&](char const* key) {
				auto r = Jsmn::Object();
				if (params.has(key)) {
					r = params[key];
					++known;
				}
				return r;
			};
			selector_j = get_key("kind");
			frac_j = get_key("horizon_frac");
			horizon_max_j = get_key("horizon_max_secs");
			min_samples_j = get_key("min_samples");
			wall_margin_j = get_key("wall_margin");
			floor_factor_j = get_key("floor_factor");
			/* Reject unknown keys.  */
			if (params.size() != known)
				return paramfail();
		} else if (params.is_array()) {
			if (params.size() > 1)
				return paramfail();
			for (auto p : params)
				selector_j = p;
		}
		if (!selector_j.is_null()) {
			if (!selector_j.is_string())
				return paramfail();
			selector = std::string(selector_j);
			if ( selector != "walls"
			  && selector != "floors"
			  && selector != "asserting"
			  && selector != "all")
				return paramfail();
		}
		if (!apply_predict_overrides( frac_j, horizon_max_j
					    , min_samples_j, wall_margin_j
					    , floor_factor_j, pparams))
			return paramfail();

		auto now = std::uint64_t(get_now());

		return db.transact().then([id, selector, pparams, now, this
					  ](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
			SELECT time, scid, dir, kind, amount_msat
			  FROM "XRebalanceHistory"
			 ORDER BY scid, dir, time;
			)QRY").execute();

			auto groups = std::map<
			    std::pair<std::string, std::uint32_t>,
			    std::vector<XRebalancePredict::Bound>>();
			for (auto& r : fetch) {
				auto ndx = std::size_t(0);
				auto time = r.get<std::uint64_t>(ndx++);
				auto row_scid = r.get<std::string>(ndx++);
				auto dir = r.get<std::uint32_t>(ndx++);
				auto kind = r.get<std::string>(ndx++);
				auto amount = r.get<std::uint64_t>(ndx++);
				/* Touch the group even for node_fail
				 * rows so `all` reports the direction
				 * (with decline reasons).  */
				auto& bounds = groups[{row_scid, dir}];
				auto is_fail = false;
				if (XRebalancePredict::kind_is_bound(
					kind, is_fail))
					bounds.push_back(
					    {time, is_fail, amount});
			}

			auto out = Json::Out();
			auto obj = out.start_object();
			json_add_prediction_params(obj, pparams);
			auto parr = obj.start_array("predictions");
			for (auto const& e : groups) {
				auto res = XRebalancePredict::predict(
				    e.second, now, pparams);
				auto keep = false;
				if (selector == "walls")
					keep = res.wall.would_assert;
				else if (selector == "floors")
					keep = res.floor.would_assert;
				else if (selector == "asserting")
					keep = res.wall.would_assert
					    || res.floor.would_assert;
				else
					keep = true;
				if (!keep)
					continue;
				emit_prediction( parr, e.first.first
					       , e.first.second, res);
			}
			parr.end_array();
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
