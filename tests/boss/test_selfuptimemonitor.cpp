#undef NDEBUG
#include"Boss/Mod/SelfUptimeMonitor.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/InternetOnline.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/RequestSelfUptime.hpp"
#include"Boss/Msg/ResponseSelfUptime.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include<assert.h>

/* Uptime rows are written on the 10-minute timer only once the
 * 10-minute listpeers poll has reported, and not while every
 * channeled peer is disconnected with no other connection (#346).  */

namespace {

auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000");

Ev::Io<void> yieldloop(unsigned int i) {
	return Ev::yield().then([i]() {
		if (i == 0)
			return Ev::lift();
		return yieldloop(i - 1);
	});
}
Ev::Io<void> yield256() {
	return yieldloop(256);
}

}

int main() {
	auto db = Sqlite3::Db(":memory:");
	auto bus = S::Bus();
	auto monitor = Boss::Mod::SelfUptimeMonitor(bus);

	auto reqresp = Boss::ModG::ReqResp< Boss::Msg::RequestSelfUptime
					  , Boss::Msg::ResponseSelfUptime
					  >
			(bus);
	auto get_day3 = [&]() {
		return Ev::lift().then([&]() {
			return reqresp.execute(Boss::Msg::RequestSelfUptime{
				nullptr
			});
		}).then([&](Boss::Msg::ResponseSelfUptime m) {
			return Ev::lift(m.day3);
		});
	};
	/* The timer handler runs in the background; let it finish.  */
	auto tick = [&]() {
		return bus.raise(Boss::Msg::Timer10Minutes{}).then([]() {
			return yield256();
		});
	};

	auto uptime = double(0);
	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::DbResource{db});
	}).then([&]() {
		return bus.raise(Boss::Msg::InternetOnline{true});
	}).then([&]() {
		/* Online, but the poll has not reported yet: no row.  */
		return tick();
	}).then([&]() {
		return get_day3();
	}).then([&](double day3) {
		assert(day3 == 0);

		/* The initial listpeers does not count as a poll.  */
		return bus.raise(Boss::Msg::ListpeersAnalyzedResult{
			{}, {A}, {}, {}, true, true
		});
	}).then([&]() {
		return tick();
	}).then([&]() {
		return get_day3();
	}).then([&](double day3) {
		assert(day3 == 0);

		/* The poll reports a connected peer: rows are written.  */
		return bus.raise(Boss::Msg::ListpeersAnalyzedResult{
			{A}, {}, {}, {}, false, false
		});
	}).then([&]() {
		return tick();
	}).then([&]() {
		return get_day3();
	}).then([&](double day3) {
		assert(day3 > 0);
		uptime = day3;

		/* Blackout: no row.  */
		return bus.raise(Boss::Msg::ListpeersAnalyzedResult{
			{}, {A}, {}, {}, false, true
		});
	}).then([&]() {
		return tick();
	}).then([&]() {
		return get_day3();
	}).then([&](double day3) {
		assert(day3 == uptime);

		/* Peers back: rows again.  */
		return bus.raise(Boss::Msg::ListpeersAnalyzedResult{
			{A}, {}, {}, {}, false, false
		});
	}).then([&]() {
		return tick();
	}).then([&]() {
		return get_day3();
	}).then([&](double day3) {
		assert(day3 > uptime);
		uptime = day3;

		/* Internet probe offline: no row, whatever the peers.  */
		return bus.raise(Boss::Msg::InternetOnline{false});
	}).then([&]() {
		return tick();
	}).then([&]() {
		return get_day3();
	}).then([&](double day3) {
		assert(day3 == uptime);

		return Ev::lift(0);
	});
	return Ev::start(code);
}
