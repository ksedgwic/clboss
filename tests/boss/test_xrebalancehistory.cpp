#undef NDEBUG

#include"Boss/Mod/XRebalanceHistory.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/Msg/XRebalanceObservation.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/CommandId.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"

#include<cassert>
#include<string>

namespace {
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
auto const C = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");
}

double mock_now = 0.0;
double mock_get_now() {
	return mock_now;
}

int main() {
	using Boss::Msg::XRebalanceObservation;
	using Boss::Msg::XRebalanceObservationKind;

	auto bus = S::Bus();

	/* Module under test */
	Boss::Mod::XRebalanceHistory mut(bus, &mock_get_now);

	auto db = Sqlite3::Db(":memory:");

	auto rsp = false;
	Boss::Msg::CommandResponse lastRsp;
	bus.subscribe<Boss::Msg::CommandResponse
		     >([&](Boss::Msg::CommandResponse const& r) {
		rsp = true;
		lastRsp = r;
		return Ev::lift();
	});
	Boss::Msg::ProvideStatus lastStatus;
	bus.subscribe<Boss::Msg::ProvideStatus
		     >([&](Boss::Msg::ProvideStatus const& st) {
		lastStatus = st;
		return Ev::lift();
	});

	auto req_id = std::uint64_t(0);
	auto history_cmd = [&](char const* params_json) {
		++req_id;
		rsp = false;
		return bus.raise(Boss::Msg::CommandRequest{
			"clboss-xrebalance-history",
			Jsmn::Object::parse_json(params_json),
			Ln::CommandId::left(req_id)
		});
	};
	auto observations = [&]() {
		assert(rsp);
		assert(lastRsp.id == Ln::CommandId::left(req_id));
		auto result = Jsmn::Object::parse_json(
			lastRsp.response.output().c_str());
		return result["observations"];
	};

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::DbResource{ db });
	}).then([&]() {
		auto o = XRebalanceObservation{
			1000, Ln::Scid("100x1x0"), 0,
			XRebalanceObservationKind::Success,
			Ln::Amount::msat(5000), 0, Ln::NodeId()};
		return bus.raise(std::move(o));
	}).then([&]() {
		auto o = XRebalanceObservation{
			2000, Ln::Scid("100x1x0"), 0,
			XRebalanceObservationKind::LiquidityFail,
			Ln::Amount::msat(7000), 0x1007, B};
		return bus.raise(std::move(o));
	}).then([&]() {
		auto o = XRebalanceObservation{
			3000, Ln::Scid("200x2x1"), 1,
			XRebalanceObservationKind::PolicyFail,
			Ln::Amount::msat(1), 0x100c, C};
		return bus.raise(std::move(o));
	}).then([&]() {
		/* Full report, ordered (scid, dir, time).  */
		return history_cmd("{}");
	}).then([&]() {
		auto obs = observations();
		assert(obs.size() == 3);
		assert(obs[0] == Jsmn::Object::parse_json(R"JSON(
		{ "time": 1000
		, "scid": "100x1x0"
		, "dir": 0
		, "kind": "success"
		, "amount_msat": 5000
		}
		)JSON"));
		assert(obs[1] == Jsmn::Object::parse_json(R"JSON(
		{ "time": 2000
		, "scid": "100x1x0"
		, "dir": 0
		, "kind": "liquidity_fail"
		, "amount_msat": 7000
		, "failcode": 4103
		, "erring_node": "020000000000000000000000000000000000000000000000000000000000000002"
		}
		)JSON"));
		assert(obs[2] == Jsmn::Object::parse_json(R"JSON(
		{ "time": 3000
		, "scid": "200x2x1"
		, "dir": 1
		, "kind": "policy_fail"
		, "amount_msat": 1
		, "failcode": 4108
		, "erring_node": "020000000000000000000000000000000000000000000000000000000000000003"
		}
		)JSON"));
		/* Filter by scid (positional).  */
		return history_cmd(R"JSON(["200x2x1"])JSON");
	}).then([&]() {
		auto obs = observations();
		assert(obs.size() == 1);
		assert(std::string(obs[0]["scid"]) == "200x2x1");
		/* Filter by hours window (keyword): now 4000, half an
		 * hour back = cutoff 2200, keeps only time 3000.  */
		mock_now = 4000.0;
		return history_cmd(R"JSON({"hours": 0.5})JSON");
	}).then([&]() {
		auto obs = observations();
		assert(obs.size() == 1);
		assert(double(obs[0]["time"]) == 3000);
		return bus.raise(Boss::Msg::SolicitStatus{});
	}).then([&]() {
		assert(lastStatus.key == "xrebalance_history");
		auto st = Jsmn::Object::parse_json(
			lastStatus.value.output().c_str());
		assert(double(st["observations"]) == 3);
		assert(double(st["channel_directions"]) == 2);
		assert(double(st["oldest_time"]) == 1000);
		assert(double(st["newest_time"]) == 3000);
		/* Default retention is a week: place now so that only
		 * the time-1000 row is past it.  */
		mock_now = 604800.0 + 1500.0;
		return bus.raise(Boss::Msg::TimerRandomHourly{});
	}).then([&]() {
		return history_cmd("{}");
	}).then([&]() {
		auto obs = observations();
		assert(obs.size() == 2);
		assert(double(obs[0]["time"]) == 2000);
		/* Shrink retention dynamically (setconfig sends a JSON
		 * string) and trim everything.  */
		return bus.raise(Boss::Msg::Option{
			"clboss-xrebalance-history-age-secs",
			Jsmn::Object::parse_json(R"JSON("3600")JSON")
		});
	}).then([&]() {
		mock_now = 3000.0 + 3600.0 + 100.0;
		return bus.raise(Boss::Msg::TimerRandomHourly{});
	}).then([&]() {
		return history_cmd("{}");
	}).then([&]() {
		auto obs = observations();
		assert(obs.size() == 0);
		return Ev::lift(0);
	});

	return Ev::start(std::move(code));
}
