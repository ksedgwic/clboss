#include"Boss/Mod/PeerTrackRecord.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/RequestPeerTrackRecord.hpp"
#include"Boss/Msg/ResponsePeerTrackRecord.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/coroutine.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"

#include<cinttypes>
#include<map>
#include<stdexcept>

namespace {

auto const default_window_days = std::int64_t(180);
auto const default_keeper_tral_bps = std::int64_t(50);
auto const default_min_record_days = std::int64_t(7);

/* FeeMonitor writes one record per fee-set cycle, roughly one per
 * hour per peer while the channel exists.  */
auto constexpr records_per_day = double(24.0);

}

namespace Boss { namespace Mod {

PeerTrackRecord::PeerTrackRecord( S::Bus& bus_
				, std::function<double()> get_now_
				) : bus(bus_)
				  , get_now(std::move(get_now_))
				  , window_days(default_window_days)
				  , keeper_tral_bps(default_keeper_tral_bps)
				  , min_record_days(default_min_record_days)
				  { start(); }

void PeerTrackRecord::start() {
	bus.subscribe<Msg::DbResource
		     >([this](Msg::DbResource const& r) {
		db = r.db;
		return Ev::lift();
	});
	bus.subscribe<Msg::Manifestation
		     >([this](Msg::Manifestation const& _) {
		return bus.raise(Msg::ManifestOption{
			"clboss-candidate-record-window-days",
			Msg::OptionType_Int,
			Json::Out::direct(default_window_days),
			"How many days of earnings history to consider "
			"when judging the track record of channel-open "
			"candidates.  Dynamic: settable at runtime via "
			"`lightning-cli setconfig`.  Default 180.",
			/* dynamic = */ true
		}) + bus.raise(Msg::ManifestOption{
			"clboss-candidate-keeper-tral-bps",
			Msg::OptionType_Int,
			Json::Out::direct(default_keeper_tral_bps),
			"Annualized net return on liquidity (basis "
			"points, the TRAL column of "
			"clboss-forwarding-stats) at or above which a "
			"channel-open candidate's track record marks it "
			"a keeper, preferred over candidates without "
			"history.  Candidates with a record below this "
			"are used only when no other candidate can "
			"absorb the funds.  Dynamic: settable at runtime "
			"via `lightning-cli setconfig`.  Default 50 "
			"(= 0.5%/year).",
			/* dynamic = */ true
		}) + bus.raise(Msg::ManifestOption{
			"clboss-candidate-min-record-days",
			Msg::OptionType_Int,
			Json::Out::direct(default_min_record_days),
			"Minimum observed operational days within the "
			"record window before a candidate's track record "
			"is trusted; candidates with less history are "
			"treated as having no record.  Dynamic: settable "
			"at runtime via `lightning-cli setconfig`.  "
			"Default 7.",
			/* dynamic = */ true
		}) + bus.raise(Msg::ManifestCommand{
			"clboss-track-record", "nodeid",
			"Show the earnings track record and verdict for "
			"{nodeid}, as used to rank channel-open "
			"candidates.",
			false
		});
	});
	bus.subscribe<Msg::Option
		     >([this](Msg::Option const& o) {
		return on_option(o);
	});
	bus.subscribe<Msg::RequestPeerTrackRecord
		     >([this](Msg::RequestPeerTrackRecord const& m) {
		return Boss::concurrent(on_request(m.requester, m.nodes));
	});
	bus.subscribe<Msg::CommandRequest
		     >([this](Msg::CommandRequest const& req) {
		if (req.command != "clboss-track-record")
			return Ev::lift();
		auto id = req.id;

		/* Accept `clboss-track-record <nodeid>` in both the
		 * positional and keyword forms.  */
		auto nodeid_j = Jsmn::Object();
		auto params = req.params;
		if (params.is_object() && params.size() == 1
		 && params.has("nodeid"))
			nodeid_j = params["nodeid"];
		else if (params.is_array() && params.size() == 1)
			nodeid_j = params[0];
		if (!nodeid_j.is_string())
			return bus.raise(Msg::CommandFail{
				id, -32602,
				"Parameter failure: need nodeid",
				Json::Out::empty_object()
			});

		return Boss::concurrent(on_command(
			id, std::string(nodeid_j)
		));
	});
}

Ev::Io<void> PeerTrackRecord::on_option(Msg::Option const& o) {
	if (o.name == "clboss-candidate-record-window-days")
		return handle_int_option(o, window_days, o.name.c_str(), 1);
	else if (o.name == "clboss-candidate-keeper-tral-bps")
		return handle_int_option( o, keeper_tral_bps, o.name.c_str()
					, INT64_MIN
					);
	else if (o.name == "clboss-candidate-min-record-days")
		return handle_int_option(o, min_record_days, o.name.c_str(), 0);
	return Ev::lift();
}

/* Parse and apply one integer option, tolerating both the
 * number-at-startup and string-via-setconfig encodings; same
 * pattern as the AskreneUpdates option handlers.  */
Ev::Io<void> PeerTrackRecord::handle_int_option( Msg::Option const& o
					       , std::int64_t& target
					       , char const* name
					       , std::int64_t minimum
					       ) {
	auto value = std::int64_t(0);
	try {
		if (o.value.is_number()) {
			value = std::int64_t(double(o.value));
		} else if (o.value.is_string()) {
			value = std::stoll(std::string(o.value));
		} else {
			o.reject( std::string(name)
				+ ": unsupported value type");
			return Boss::log( bus, Warn
					, "PeerTrackRecord: %s: "
					  "unsupported value type; "
					  "keeping %" PRId64 "."
					, name, target
					);
		}
	} catch (std::exception const& e) {
		o.reject(std::string(name) + ": not a valid number");
		return Boss::log( bus, Warn
				, "PeerTrackRecord: %s: parse error "
				  "'%s'; keeping %" PRId64 "."
				, name, e.what(), target
				);
	}
	if (value < minimum) {
		o.reject( std::string(name) + ": must be >= "
			+ std::to_string(minimum)
			);
		return Boss::log( bus, Warn
				, "PeerTrackRecord: %s: must be >= %" PRId64
				  "; keeping %" PRId64 "."
				, name, minimum, target
				);
	}
	target = value;
	return Boss::log( bus, Info
			, "PeerTrackRecord: %s = %" PRId64 "."
			, name, target
			);
}

Ev::Io<Sqlite3::Tx> PeerTrackRecord::db_transact() {
	while (!db) {
		co_await Ev::yield();
	}
	co_return co_await db.transact();
}

/* The data this module judges is owned by other modules
 * (EarningsTracker, FeeMonitor); if a table has not been created
 * yet, report "no record" rather than erroring.  */
bool PeerTrackRecord::have_tables(Sqlite3::Tx& tx) {
	auto fetch = tx.query(R"QRY(
	SELECT COUNT(*)
	  FROM sqlite_master
	 WHERE type = 'table'
	   AND name IN ( 'EarningsTracker'
		       , 'feemon_change_events'
		       , 'feemon_peers'
		       );
	)QRY").execute();
	for (auto& r : fetch)
		return r.get<std::uint64_t>(0) == 3;
	return false;
}

Msg::TrackRecord PeerTrackRecord::compute( Sqlite3::Tx& tx
					 , std::string const& node
					 , double now
					 ) {
	auto rec = Msg::TrackRecord{
		Msg::TrackRecordVerdict::NoRecord, 0.0, 0.0, 0
	};
	auto cutoff = now - double(window_days) * 24 * 60 * 60;

	/* Net earnings (both directions, net of rebalance
	 * expenditures) within the window.  */
	auto net_fetch = tx.query(R"QRY(
	SELECT COALESCE( SUM(in_earnings + out_earnings)
		       - SUM(in_expenditures + out_expenditures)
		       , 0)
	  FROM "EarningsTracker"
	 WHERE node = :node
	   AND time_bucket >= :cutoff;
	)QRY")
		.bind(":node", node)
		.bind(":cutoff", cutoff)
		.execute()
		;
	for (auto& r : net_fetch)
		rec.net_msat = r.get<std::int64_t>(0);

	/* Operational coverage and average balance within the
	 * window.  Averaging per-record (instead of sample-and-hold
	 * over wall-clock time) weights only hours the channel
	 * actually operated, so a channel that closed mid-window is
	 * not diluted by the gap after its close.  */
	auto cover_fetch = tx.query(R"QRY(
	SELECT COUNT(e.balance_our_msat)
	     , COALESCE(AVG(e.balance_our_msat), 0.0)
	  FROM feemon_change_events e
	  JOIN feemon_peers p ON e.peer_id = p.id
	 WHERE p.node_id = :node
	   AND e.ts >= :cutoff;
	)QRY")
		.bind(":node", node)
		.bind(":cutoff", cutoff)
		.execute()
		;
	auto count = std::uint64_t(0);
	auto avg_msat = double(0.0);
	for (auto& r : cover_fetch) {
		count = r.get<std::uint64_t>(0);
		avg_msat = r.get<double>(1);
	}

	rec.op_days = double(count) / records_per_day;
	if ( count == 0
	  || rec.op_days < double(min_record_days)
	  || avg_msat <= 0.0
	   )
		return rec;

	rec.tral_bps = (double(rec.net_msat) / avg_msat)
		     / rec.op_days * 365.0 * 10000.0;
	rec.verdict = (rec.tral_bps >= double(keeper_tral_bps))
		    ? Msg::TrackRecordVerdict::Keeper
		    : Msg::TrackRecordVerdict::Underperformer
		    ;
	return rec;
}

Ev::Io<void> PeerTrackRecord::on_request( void* requester
					, std::vector<Ln::NodeId> nodes
					) {
	auto tx = co_await db_transact();
	auto records = std::map<Ln::NodeId, Msg::TrackRecord>();
	if (have_tables(tx)) {
		auto now = get_now();
		for (auto const& n : nodes)
			records[n] = compute(tx, std::string(n), now);
	} else {
		for (auto const& n : nodes)
			records[n] = Msg::TrackRecord{
				Msg::TrackRecordVerdict::NoRecord,
				0.0, 0.0, 0
			};
	}
	tx.commit();

	auto msg = Msg::ResponsePeerTrackRecord{
		requester, std::move(records)
	};
	co_await bus.raise(std::move(msg));
	co_return;
}

Ev::Io<void> PeerTrackRecord::on_command( Ln::CommandId id
					, std::string nodeid
					) {
	auto tx = co_await db_transact();
	auto rec = Msg::TrackRecord{
		Msg::TrackRecordVerdict::NoRecord, 0.0, 0.0, 0
	};
	if (have_tables(tx))
		rec = compute(tx, nodeid, get_now());
	tx.commit();

	auto out = Json::Out();
	out.start_object()
		.field("node", nodeid)
		.field( "verdict"
		      , std::string(Msg::track_record_verdict_name(
				rec.verdict
			))
		      )
		.field("tral_bps", rec.tral_bps)
		.field("op_days", rec.op_days)
		.field("net_msat", rec.net_msat)
		.field("window_days", window_days)
		.field("keeper_tral_bps", keeper_tral_bps)
		.field("min_record_days", min_record_days)
	.end_object();

	auto msg = Msg::CommandResponse{id, std::move(out)};
	co_await bus.raise(std::move(msg));
	co_return;
}

}}
