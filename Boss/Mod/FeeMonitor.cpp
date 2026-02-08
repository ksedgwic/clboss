#include"Boss/Mod/FeeMonitor.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/MonitorFeeByBalance.hpp"
#include"Boss/Msg/MonitorFeeByTheory.hpp"
#include"Boss/Msg/MonitorFeeSetChannel.hpp"
#include"Boss/Msg/MonitorFeeBySize.hpp"
#include"Boss/Msg/PeerMedianChannelFee.hpp"
#include"Ev/Io.hpp"
#include"Ev/coroutine.hpp"
#include"Ev/now.hpp"
#include"Ev/yield.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include<cmath>

namespace {

template<typename T>
void bind_optional(Sqlite3::Query& q, char const* name
		 , std::optional<T> const& value) {
	if (value)
		q.bind(name, *value);
	else
		q.bind(name, nullptr);
}

template<typename Obj>
void add_optional_int( Obj& obj
		     , char const* name
		     , Sqlite3::Row& r
		     , std::size_t& idx
		     ) {
	auto value = r.get<std::int64_t>(idx++);
	auto is_null = r.get<int>(idx++) != 0;
	if (is_null)
		obj.field(name, nullptr);
	else
		obj.field(name, value);
}

template<typename Obj>
void add_optional_double( Obj& obj
			, char const* name
			, Sqlite3::Row& r
			, std::size_t& idx
			) {
	auto value = r.get<double>(idx++);
	auto is_null = r.get<int>(idx++) != 0;
	if (is_null)
		obj.field(name, nullptr);
	else
		obj.field(name, value);
}

bool parse_optional_number( Jsmn::Object const& value
			  , std::optional<double>& out
			  ) {
	if (value.is_null())
		return true;
	if (!value.is_number())
		return false;
	out = (double) value;
	return true;
}

}

namespace Boss { namespace Mod {

void FeeMonitor::start() {
	bus.subscribe<Msg::DbResource
		     >([this](Msg::DbResource const& m) {
		return on_db(m);
	});
	bus.subscribe<Msg::PeerMedianChannelFee
		     >([this](Msg::PeerMedianChannelFee const& m) {
		return on_baseline(m);
	});
	bus.subscribe<Msg::ChannelDestruction
		     >([this](Msg::ChannelDestruction const& d) {
		auto it = peers.find(d.peer);
		if (it == peers.end())
			return Ev::lift();
		peers.erase(it);
		return Ev::lift();
	});
	bus.subscribe<Msg::MonitorFeeBySize
		     >([this](Msg::MonitorFeeBySize const& m) {
		return on_size(m);
	});
	bus.subscribe<Msg::MonitorFeeByBalance
		     >([this](Msg::MonitorFeeByBalance const& m) {
		return on_balance(m);
	});
	bus.subscribe<Msg::MonitorFeeByTheory
		     >([this](Msg::MonitorFeeByTheory const& m) {
		return on_price(m);
	});
	bus.subscribe<Msg::MonitorFeeSetChannel
		     >([this](Msg::MonitorFeeSetChannel const& m) {
		return on_set(m);
	});
	bus.subscribe<Msg::Manifestation
		     >([this](Msg::Manifestation const&) {
		return bus.raise(Msg::ManifestCommand{
			"clboss-feemon-history",
			"nodeid [since] [before]",
			"Show fee modifier history for nodeid between since and before.",
			false
		});
	});
	bus.subscribe<Msg::CommandRequest
		     >([this](Msg::CommandRequest const& req) {
		if (req.command != "clboss-feemon-history")
			return Ev::lift();

		auto id = req.id;
		auto paramfail = [this, id]() {
			return bus.raise(Msg::CommandFail{
				id, -32602,
				"Parameter failure",
				Json::Out::empty_object()
			});
		};

		auto nodeid_j = Jsmn::Object();
		auto since_j = Jsmn::Object();
		auto before_j = Jsmn::Object();
		auto params = req.params;
		if (params.is_object()) {
			auto has_nodeid = params.has("nodeid");
			auto has_since = params.has("since");
			auto has_before = params.has("before");
			if (!has_nodeid)
				return paramfail();
			if (params.size() != std::size_t(
				has_nodeid + has_since + has_before
			))
				return paramfail();
			nodeid_j = params["nodeid"];
			if (has_since)
				since_j = params["since"];
			if (has_before)
				before_j = params["before"];
		} else if (params.is_array()) {
			if (params.size() < 1 || params.size() > 3)
				return paramfail();
			nodeid_j = params[0];
			if (params.size() >= 2)
				since_j = params[1];
			if (params.size() >= 3)
				before_j = params[2];
		} else {
			return paramfail();
		}

		if (!nodeid_j.is_string())
			return paramfail();
		auto nodeid_s = std::string(nodeid_j);
		if (!Ln::NodeId::valid_string(nodeid_s))
			return paramfail();
		nodeid_s = std::string(Ln::NodeId(nodeid_s));

		auto since = std::optional<double>();
		auto before = std::optional<double>();
		if (!parse_optional_number(since_j, since))
			return paramfail();
		if (!parse_optional_number(before_j, before))
			return paramfail();
		if (since && before && *since > *before)
			return paramfail();

		return db_transact().then([this, id, nodeid_s, since, before](Sqlite3::Tx tx) {
			auto q = tx.query(R"QRY(
			SELECT e.id,
			       e.ts,
			       e.peer_id,
			       e.set_base,
			       e.set_base IS NULL,
			       e.set_ppm,
			       e.set_ppm IS NULL,
			       e.baseline_base,
			       e.baseline_base IS NULL,
			       e.baseline_ppm,
			       e.baseline_ppm IS NULL,
			       e.size_mult,
			       e.size_mult IS NULL,
			       e.size_total_peers,
			       e.size_total_peers IS NULL,
			       e.size_less_peers,
			       e.size_less_peers IS NULL,
			       e.balance_mult,
			       e.balance_mult IS NULL,
			       e.balance_our_msat,
			       e.balance_our_msat IS NULL,
			       e.balance_total_msat,
			       e.balance_total_msat IS NULL,
			       e.price_level,
			       e.price_level IS NULL,
			       e.price_mult,
			       e.price_mult IS NULL,
			       e.price_cards_left,
			       e.price_cards_left IS NULL,
			       e.price_center,
			       e.price_center IS NULL,
			       e.mult_product,
			       e.mult_product IS NULL,
			       e.est_base,
			       e.est_base IS NULL,
			       e.est_ppm,
			       e.est_ppm IS NULL
			  FROM feemon_change_events e
			  JOIN feemon_peers p
			    ON e.peer_id = p.id
			 WHERE p.node_id = :node_id
			   AND (:since IS NULL OR e.ts >= :since)
			   AND (:before IS NULL OR e.ts <= :before)
			 ORDER BY e.ts ASC;
			)QRY");
			q.bind(":node_id", nodeid_s);
			bind_optional(q, ":since", since);
			bind_optional(q, ":before", before);
			auto fetch = q.execute();

			auto out = Json::Out();
			auto top = out.start_object();
			top.field("nodeid", nodeid_s);
			if (since)
				top.field("since", *since);
			if (before)
				top.field("before", *before);
			auto history = top.start_array("history");
			for (auto& r : fetch) {
				auto row = history.start_object();
				auto idx = std::size_t(0);
				row.field("id", r.get<std::uint64_t>(idx++));
				row.field("ts", static_cast<std::uint64_t>(r.get<double>(idx++)));
				row.field("peer_id", r.get<std::uint64_t>(idx++));
				add_optional_int(row, "set_base", r, idx);
				add_optional_int(row, "set_ppm", r, idx);
				add_optional_int(row, "baseline_base", r, idx);
				add_optional_int(row, "baseline_ppm", r, idx);
				add_optional_double(row, "size_mult", r, idx);
				add_optional_int(row, "size_total_peers", r, idx);
				add_optional_int(row, "size_less_peers", r, idx);
				add_optional_double(row, "balance_mult", r, idx);
				add_optional_int(row, "balance_our_msat", r, idx);
				add_optional_int(row, "balance_total_msat", r, idx);
				add_optional_int(row, "price_level", r, idx);
				add_optional_double(row, "price_mult", r, idx);
				add_optional_int(row, "price_cards_left", r, idx);
				add_optional_int(row, "price_center", r, idx);
				add_optional_double(row, "mult_product", r, idx);
				add_optional_int(row, "est_base", r, idx);
				add_optional_int(row, "est_ppm", r, idx);
				row.end_object();
			}
			history.end_array();
			top.end_object();
			tx.commit();
			return bus.raise(Msg::CommandResponse{id, out});
		});
	});
}

Ev::Io<void> FeeMonitor::on_db(Msg::DbResource const& m) {
	db = m.db;
	co_await initialize_db();
	co_return;
}

Ev::Io<void> FeeMonitor::initialize_db() {
	auto tx = co_await db_transact();
	tx.query_execute("PRAGMA foreign_keys = ON;");
	tx.query_execute(R"QRY(
	CREATE TABLE IF NOT EXISTS feemon_peers (
		id INTEGER PRIMARY KEY,
		node_id TEXT NOT NULL UNIQUE
	);
	CREATE TABLE IF NOT EXISTS feemon_change_events (
		id INTEGER PRIMARY KEY,
		ts REAL NOT NULL,
		peer_id INTEGER NOT NULL,
		set_base INTEGER,
		set_ppm INTEGER,
		baseline_base INTEGER,
		baseline_ppm INTEGER,
		size_mult REAL,
		size_total_peers INTEGER,
		size_less_peers INTEGER,
		balance_mult REAL,
		balance_our_msat INTEGER,
		balance_total_msat INTEGER,
		price_level INTEGER,
		price_mult REAL,
		price_cards_left INTEGER,
		price_center INTEGER,
		mult_product REAL,
		est_base INTEGER,
		est_ppm INTEGER,
		FOREIGN KEY(peer_id) REFERENCES feemon_peers(id)
	);
	CREATE INDEX IF NOT EXISTS feemon_change_events_peer_ts_idx
	ON feemon_change_events(peer_id, ts);
	CREATE INDEX IF NOT EXISTS feemon_change_events_ts_peer_idx
	ON feemon_change_events(ts, peer_id);
	)QRY");
	tx.commit();
	co_return;
}

Ev::Io<Sqlite3::Tx> FeeMonitor::db_transact() {
	while (!db) {
		co_await Ev::yield();
	}
	co_return co_await db.transact();
}

std::uint64_t
FeeMonitor::get_peer_id(Sqlite3::Tx& tx, Ln::NodeId const& node) {
	auto fetch = tx.query(R"QRY(
	SELECT id
	  FROM feemon_peers
	 WHERE node_id = :node_id
	     ;
	)QRY")
		.bind(":node_id", std::string(node))
		.execute()
		;
	for (auto& r : fetch)
		return r.get<std::uint64_t>(0);

	tx.query(R"QRY(
	INSERT OR IGNORE INTO feemon_peers
	VALUES(NULL, :node_id);
	)QRY")
		.bind(":node_id", std::string(node))
		.execute()
		;

	return get_peer_id(tx, node);
}

Ev::Io<void>
FeeMonitor::on_baseline(Msg::PeerMedianChannelFee const& m) {
	auto& info = peers[m.node];
	info.baseline_base = m.base;
	info.baseline_ppm = m.proportional;
	co_return;
}

Ev::Io<void>
FeeMonitor::on_size(Msg::MonitorFeeBySize const& m) {
	auto& info = peers[m.node];
	info.size_mult = m.mult;
	info.size_total_peers = m.total_peers;
	info.size_less_peers = m.less_peers;
	co_return;
}

Ev::Io<void>
FeeMonitor::on_balance(Msg::MonitorFeeByBalance const& m) {
	auto& info = peers[m.node];
	info.balance_mult = m.mult;
	info.balance_our_msat = m.our_msat;
	info.balance_total_msat = m.total_msat;
	co_return;
}

Ev::Io<void>
FeeMonitor::on_price(Msg::MonitorFeeByTheory const& m) {
	auto& info = peers[m.node];
	info.price_level = m.level;
	info.price_mult = m.mult;
	info.price_cards_left = m.cards_left;
	info.price_center = m.center;
	co_return;
}

Ev::Io<void>
FeeMonitor::on_set(Msg::MonitorFeeSetChannel const& m) {
	auto snapshot = peers[m.node];
	auto ts = Ev::now();

	auto mult_product = std::optional<double>();
	auto est_base = std::optional<std::int64_t>();
	auto est_ppm = std::optional<std::int64_t>();

	if ( snapshot.baseline_base
	  && snapshot.baseline_ppm
	  && snapshot.size_mult
	  && snapshot.balance_mult
	  && snapshot.price_mult
	   ) {
		mult_product = (*snapshot.size_mult)
			     * (*snapshot.balance_mult)
			     * (*snapshot.price_mult);
		auto base = std::llround(
			double(*snapshot.baseline_base) * *mult_product
		);
		auto ppm = std::llround(
			double(*snapshot.baseline_ppm) * *mult_product
		);
		if (ppm == 0)
			ppm = 1;
		est_base = base;
		est_ppm = ppm;
	}

	auto tx = co_await db_transact();
	auto peer_id = get_peer_id(tx, m.node);
	auto q = tx.query(R"QRY(
	INSERT INTO feemon_change_events (
		ts,
		peer_id,
		set_base,
		set_ppm,
		baseline_base,
		baseline_ppm,
		size_mult,
		size_total_peers,
		size_less_peers,
		balance_mult,
		balance_our_msat,
		balance_total_msat,
		price_level,
		price_mult,
		price_cards_left,
		price_center,
		mult_product,
		est_base,
		est_ppm
	) VALUES (
		:ts,
		:peer_id,
		:set_base,
		:set_ppm,
		:baseline_base,
		:baseline_ppm,
		:size_mult,
		:size_total_peers,
		:size_less_peers,
		:balance_mult,
		:balance_our_msat,
		:balance_total_msat,
		:price_level,
		:price_mult,
		:price_cards_left,
		:price_center,
		:mult_product,
		:est_base,
		:est_ppm
	);
	)QRY");

	q.bind(":ts", ts)
	 .bind(":peer_id", peer_id)
	 .bind(":set_base", m.base)
	 .bind(":set_ppm", m.proportional);
	bind_optional(q, ":baseline_base", snapshot.baseline_base);
	bind_optional(q, ":baseline_ppm", snapshot.baseline_ppm);
	bind_optional(q, ":size_mult", snapshot.size_mult);
	bind_optional(q, ":size_total_peers", snapshot.size_total_peers);
	bind_optional(q, ":size_less_peers", snapshot.size_less_peers);
	bind_optional(q, ":balance_mult", snapshot.balance_mult);
	bind_optional(q, ":balance_our_msat", snapshot.balance_our_msat);
	bind_optional(q, ":balance_total_msat", snapshot.balance_total_msat);
	bind_optional(q, ":price_level", snapshot.price_level);
	bind_optional(q, ":price_mult", snapshot.price_mult);
	bind_optional(q, ":price_cards_left", snapshot.price_cards_left);
	bind_optional(q, ":price_center", snapshot.price_center);
	bind_optional(q, ":mult_product", mult_product);
	bind_optional(q, ":est_base", est_base);
	bind_optional(q, ":est_ppm", est_ppm);
	q.execute();
	tx.commit();
	co_return;
}

FeeMonitor::FeeMonitor(S::Bus& bus_) : bus(bus_) {
	start();
}
FeeMonitor::~FeeMonitor() =default;

}}
