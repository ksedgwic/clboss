#include"Boss/Mod/SelfUptimeMonitor.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/InternetOnline.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RequestSelfUptime.hpp"
#include"Boss/Msg/ResponseSelfUptime.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Boss/concurrent.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"

namespace {

/* Number of 10-minute timers expected per day,  */
auto constexpr timers_per_day = std::size_t(144);
/* Number of seconds per day.  */
auto constexpr seconds_per_day = double(24 * 60 * 60);

/* Number of days.  */
auto constexpr days_per_day3 = std::size_t(3);
auto constexpr days_per_weeks2 = std::size_t(14);
auto constexpr days_per_month1 = std::size_t(30);

/* Number of seconds.  */
auto constexpr seconds_per_day3 = days_per_day3 * seconds_per_day;
auto constexpr seconds_per_weeks2 = days_per_weeks2 * seconds_per_day;
auto constexpr seconds_per_month1 = days_per_month1 * seconds_per_day;

/* Number of 10-minute timers.  */
auto constexpr timers_per_day3 = days_per_day3 * timers_per_day;
auto constexpr timers_per_weeks2 = days_per_weeks2 * timers_per_day;
auto constexpr timers_per_month1 = days_per_month1 * timers_per_day;

}

namespace Boss { namespace Mod {

class SelfUptimeMonitor::Impl {
private:
	S::Bus& bus;

	Sqlite3::Db db;

	bool online;
	/* We have channeled peers and not one of them is connected:
	 * our own outage (lightningd --offline, a Tor or firewall
	 * failure) even when the internet probe reads online.  Such
	 * time does not count as uptime (#346).  */
	bool all_peers_disconnected;

	void start() {
		online = false;
		all_peers_disconnected = false;
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& m) {
			db = m.db;
			return db.transact().then([](Sqlite3::Tx tx) {
				tx.query_execute(R"QRY(
				CREATE TABLE IF NOT EXISTS "SelfUptimeMonitor"
				     ( time REAL PRIMARY KEY
				     );
				)QRY");

				tx.commit();
				return Ev::lift();
			});
		});
		bus.subscribe<Msg::InternetOnline
			     >([this](Msg::InternetOnline const& m) {
			online = m.online;
			return Ev::lift();
		});
		bus.subscribe<Msg::ListpeersAnalyzedResult
			     >([this](Msg::ListpeersAnalyzedResult const& r) {
			/* At init the peers have not reconnected yet;
			 * keep the previous state.  */
			if (!r.initial)
				all_peers_disconnected =
					r.connected_channeled.empty()
				     && !r.disconnected_channeled.empty();
			return Ev::lift();
		});
		bus.subscribe<Msg::Timer10Minutes
			     >([this](Msg::Timer10Minutes const& _) {
			return Boss::concurrent(timer10minutes());
		});
		bus.subscribe<Msg::RequestSelfUptime
			     >([this](Msg::RequestSelfUptime const& req) {
			auto requester = req.requester;
			return Ev::lift().then([this, requester]() {
				return get_uptime(requester);
			}).then([this](Msg::ResponseSelfUptime rsp) {
				return bus.raise(std::move(rsp));
			});
		});
		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const& m) {
			return Ev::lift().then([this]() {
				return get_uptime(nullptr);
			}).then([this](Msg::ResponseSelfUptime rsp) {
				auto out = Json::Out()
					.start_object()
						.field("past_3_days", rsp.day3)
						.field("past_2_weeks", rsp.weeks2)
						.field("past_1_month", rsp.month1)
					.end_object()
					;
				return bus.raise(Msg::ProvideStatus{
					"my_uptime",
					std::move(out)
				});
			});
		});
	}

	Ev::Io<void> timer10minutes() {
		return Ev::lift().then([this]() {
			return db.transact();
		}).then([this](Sqlite3::Tx tx) {
			auto now = Ev::now();
			if (online && !all_peers_disconnected) {
				tx.query(R"QRY(
				INSERT OR IGNORE INTO "SelfUptimeMonitor"
				VALUES(:time);
				)QRY")
					.bind(":time", now)
					.execute()
					;
			}

			/* Clean up very old entries.  */
			auto limit = now - 2 * seconds_per_month1;
			tx.query(R"QRY(
			DELETE FROM "SelfUptimeMonitor"
			 WHERE time < :mintime;
			)QRY")
				.bind(":mintime", limit)
				.execute()
				;

			tx.commit();
			return Ev::lift();
		});
	}

	/* Calculates the uptime of a particular timeframe.  */
	static
	double calc_uptime( Sqlite3::Tx& tx
			  , double seconds_per_timeframe
			  , std::size_t timers_per_timeframe
			  ) {
		auto count = std::size_t(0);

		auto fetch = tx.query(R"QRY(
		SELECT COUNT(*) FROM "SelfUptimeMonitor"
		 WHERE time >= :mintime;
		)QRY")
			.bind(":mintime", Ev::now() - seconds_per_timeframe)
			.execute()
			;
		for (auto& r : fetch) {
			count = r.get<std::size_t>(0);
			break;
		}

		if (count > timers_per_timeframe)
			count = timers_per_timeframe;

		return double(count) / double(timers_per_timeframe);
	}
	/* Gathers the uptime data.  */
	Ev::Io<Msg::ResponseSelfUptime>
	get_uptime(void* requester) {
		return db.transact().then([requester](Sqlite3::Tx tx) {
			auto msg = Msg::ResponseSelfUptime();
			msg.requester = requester;
			msg.day3 = calc_uptime(tx, seconds_per_day3, timers_per_day3);
			msg.weeks2 = calc_uptime(tx, seconds_per_weeks2, timers_per_weeks2);
			msg.month1 = calc_uptime(tx, seconds_per_month1, timers_per_month1);

			tx.commit();

			return Ev::lift(std::move(msg));
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	Impl( S::Bus& bus_
	    ) : bus(bus_)
	      { start(); }
};

SelfUptimeMonitor::SelfUptimeMonitor(SelfUptimeMonitor&&) =default;
SelfUptimeMonitor::~SelfUptimeMonitor() =default;

SelfUptimeMonitor::SelfUptimeMonitor(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
