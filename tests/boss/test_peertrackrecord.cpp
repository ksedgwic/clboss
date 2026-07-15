#undef NDEBUG

#include"Boss/Mod/EarningsTracker.hpp"
#include"Boss/Mod/FeeMonitor.hpp"
#include"Boss/Mod/PeerTrackRecord.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/RequestPeerTrackRecord.hpp"
#include"Boss/Msg/ResponsePeerTrackRecord.hpp"
#include"Boss/Msg/TrackRecord.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"

#include<assert.h>
#include<cstdint>
#include<string>
#include<vector>

namespace {

/* Keeper: 60 days of coverage, strong earnings.  */
auto const K = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
/* Underperformer: 60 days of coverage, negligible earnings.  */
auto const U = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
/* Insufficient: only 3 days of coverage.  */
auto const I = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");
/* No data at all.  */
auto const N = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000004");
/* Partial overlap: 10 days of coverage; earnings sized so the
 * verdict is Keeper only if annualization uses the 10 observed
 * operational days and not the full 180-day window.  */
auto const P = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000005");

/* A daily-bucket boundary (2024-08-06 00:00:00 UTC).  */
auto const mock_now = double(1722902400);
double mock_get_now() { return mock_now; }

auto const balance_msat = double(10000000000.0); /* 1e10 msat */

void insert_feemon_rows( Sqlite3::Tx& tx
		       , std::uint64_t peer_id
		       , Ln::NodeId const& node
		       , unsigned int hours
		       ) {
	tx.query(R"QRY(
	INSERT INTO feemon_peers VALUES(:id, :node);
	)QRY")
		.bind(":id", peer_id)
		.bind(":node", std::string(node))
		.execute();
	tx.query(R"QRY(
	WITH RECURSIVE h(i) AS (
		SELECT 0
		UNION ALL
		SELECT i + 1 FROM h WHERE i + 1 < :hours
	)
	INSERT INTO feemon_change_events (ts, peer_id, balance_our_msat)
	SELECT :now - (i * 3600.0), :peer_id, :balance FROM h;
	)QRY")
		.bind(":hours", hours)
		.bind(":now", mock_now)
		.bind(":peer_id", peer_id)
		.bind(":balance", balance_msat)
		.execute();
}

void insert_earnings( Sqlite3::Tx& tx
		    , Ln::NodeId const& node
		    , double bucket
		    , std::uint64_t in_earnings_msat
		    ) {
	tx.query(R"QRY(
	INSERT INTO EarningsTracker
	VALUES(:node, :bucket, :in_earnings, 0, 0, 0, 0, 0, 0, 0);
	)QRY")
		.bind(":node", std::string(node))
		.bind(":bucket", bucket)
		.bind(":in_earnings", in_earnings_msat)
		.execute();
}

}

int main() {
	auto bus = S::Bus();

	/* Own the schemas the module under test reads.  */
	Boss::Mod::EarningsTracker earnings(bus, &mock_get_now);
	Boss::Mod::FeeMonitor feemon(bus);
	/* Module under test.  */
	Boss::Mod::PeerTrackRecord mut(bus, &mock_get_now);

	auto db = Sqlite3::Db(":memory:");

	auto have_response = false;
	Boss::Msg::ResponsePeerTrackRecord last;
	bus.subscribe< Boss::Msg::ResponsePeerTrackRecord
		     >([&](Boss::Msg::ResponsePeerTrackRecord const& r) {
		last = r;
		have_response = true;
		return Ev::lift();
	});

	/* Boss::concurrent schedules the request handler; yield a
	 * bunch to let it run to completion.  */
	auto flush = []() {
		auto act = Ev::lift();
		for (auto i = 0; i < 32; ++i)
			act = std::move(act).then([]() {
				return Ev::yield();
			});
		return act;
	};
	auto verdict_of = [&](Ln::NodeId const& n) {
		auto it = last.records.find(n);
		assert(it != last.records.end());
		return it->second.verdict;
	};

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::DbResource{ db });
	}).then([&]() {
		return flush();
	}).then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto day = double(24 * 60 * 60);
		/* K: 60 days coverage; net 5e7 msat.
		 * TRAL = (5e7 / 1e10) / 60 * 365e4 ~ +304 bps.  */
		insert_feemon_rows(tx, 1, K, 60 * 24);
		insert_earnings(tx, K, mock_now - day, 50000000);
		/* U: 60 days coverage; net 1e6 msat -> ~ +6 bps.  */
		insert_feemon_rows(tx, 2, U, 60 * 24);
		insert_earnings(tx, U, mock_now - day, 1000000);
		/* I: 3 days coverage (below min-record-days 7),
		 * earnings do not matter.  */
		insert_feemon_rows(tx, 3, I, 3 * 24);
		insert_earnings(tx, I, mock_now - day, 50000000);
		/* P: 10 days coverage; net 1e7 msat.
		 * Annualized over the 10 observed days:
		 * (1e7 / 1e10) / 10 * 365e4 ~ +365 bps -> Keeper.
		 * If wrongly annualized over the whole 180-day
		 * window: ~ +20 bps -> Underperformer.  */
		insert_feemon_rows(tx, 4, P, 10 * 24);
		insert_earnings(tx, P, mock_now - day, 10000000);
		tx.commit();
		return Ev::lift();
	}).then([&]() {
		auto msg = Boss::Msg::RequestPeerTrackRecord{
			nullptr, std::vector<Ln::NodeId>{K, U, I, N, P}
		};
		return bus.raise(std::move(msg));
	}).then([&]() {
		return flush();
	}).then([&]() {
		assert(have_response);
		assert(last.records.size() == 5);
		assert(verdict_of(K) == Boss::Msg::TrackRecordVerdict::Keeper);
		assert(verdict_of(U) == Boss::Msg::TrackRecordVerdict::Underperformer);
		assert(verdict_of(I) == Boss::Msg::TrackRecordVerdict::NoRecord);
		assert(verdict_of(N) == Boss::Msg::TrackRecordVerdict::NoRecord);
		assert(verdict_of(P) == Boss::Msg::TrackRecordVerdict::Keeper);

		/* Sanity on the computed numbers.  */
		auto const& k = last.records.find(K)->second;
		assert(k.net_msat == 50000000);
		assert(k.op_days > 59.9 && k.op_days < 60.1);
		assert(k.tral_bps > 290.0 && k.tral_bps < 320.0);
		return Ev::lift();
	}).then([&]() {
		/* Raise the keeper threshold to 350 bps, using the
		 * string encoding that the runtime setconfig path
		 * delivers.  K (~304) drops to Underperformer while
		 * P (~365) stays Keeper.  */
		auto msg = Boss::Msg::Option{
			"clboss-candidate-keeper-tral-bps",
			Jsmn::Object::parse_json("\"350\""),
			nullptr
		};
		return bus.raise(std::move(msg));
	}).then([&]() {
		have_response = false;
		auto msg = Boss::Msg::RequestPeerTrackRecord{
			nullptr, std::vector<Ln::NodeId>{K, P}
		};
		return bus.raise(std::move(msg));
	}).then([&]() {
		return flush();
	}).then([&]() {
		assert(have_response);
		assert(verdict_of(K) == Boss::Msg::TrackRecordVerdict::Underperformer);
		assert(verdict_of(P) == Boss::Msg::TrackRecordVerdict::Keeper);
		return Ev::lift(0);
	});

	return Ev::start(std::move(code));
}
