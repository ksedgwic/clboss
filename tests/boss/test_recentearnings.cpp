#undef NDEBUG

#include"Boss/Mod/EarningsTracker.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"

#include <cassert>
#include<iostream>
#include<sstream>

namespace {
auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
auto const C = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");
}

double mock_now = 0.0;
double mock_get_now() {
	return mock_now;
}

Ev::Io<void> raiseForwardFeeLoop(S::Bus& bus, int count) {
	// Creates a series of forwards, one per hour
	if (count == 0) {
		return Ev::lift();
	}
	return bus.raise(Boss::Msg::ForwardFee{
			A,              	// in_id
			B,              	// out_id
			Ln::Amount::sat(1),	// fee
			1.0             	// resolution_time
		})
		.then([&, count]() {
			mock_now += 60 * 60;
			return raiseForwardFeeLoop(bus, count - 1);
		});
}

Ev::Io<void> raiseMoveFundsLoop(S::Bus& bus, int count) {
	if (count == 0) {
		return Ev::lift();
	}
	return bus.raise(
		Boss::Msg::RequestMoveFunds{
			nullptr,        	// requester (match ResponseMoveFunds)
			C,              	// source
			A,              	// destination
			Ln::Amount::sat(1000),  // amount
			Ln::Amount::sat(3)      // fee_budget
		})
		.then([&bus]() {
			return bus.raise(
				Boss::Msg::ResponseMoveFunds{
					nullptr,        	// requester (see RequestMoveFunds)
					Ln::Amount::sat(1000),  // amount_moved
					Ln::Amount::sat(2)      // fee_spent
				});
		})
		.then([&bus, count]() {
			mock_now += 24 * 60 * 60; // advance one day
			return raiseMoveFundsLoop(bus, count - 1);
		});
}

int main() {
	auto bus = S::Bus();

	/* Module under test */
	Boss::Mod::EarningsTracker mut(bus, &mock_get_now);

	auto db = Sqlite3::Db(":memory:");

	auto req_id = std::uint64_t();
	auto lastRsp = Boss::Msg::CommandResponse{};
	auto rsp = false;
	bus.subscribe<Boss::Msg::CommandResponse>([&](Boss::Msg::CommandResponse const& m) {
		lastRsp = m;
		rsp = true;
		return Ev::yield();
	});


	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::DbResource{ db });
	}).then([&]() {
		++req_id;
		return bus.raise(Boss::Msg::CommandRequest{
				"clboss-recent-earnings",
				Jsmn::Object(),
				Ln::CommandId::left(req_id)
			});
	}).then([&]() {
		// With no fees should see an empty result collection
		auto result = Jsmn::Object::parse_json(lastRsp.response.output().c_str());
		assert(result["recent"] == Jsmn::Object::parse_json(R"JSON(
				{}
                                )JSON"));
		return Ev::lift();
	}).then([&]() {
		// Insert 2 months of one fee per hour
		mock_now = 1722902400; // stroke of midnight
		return raiseForwardFeeLoop(bus, 24 * 60);
	}).then([&]() {
		// Rewind 1 week and add 7 days of one rebalance per day
		mock_now -= (7 * 24 * 60 * 60);
		return raiseMoveFundsLoop(bus, 7);
	}).then([&]() {
		// Default report should include 14 days
		++req_id;
		return bus.raise(Boss::Msg::CommandRequest{
				"clboss-recent-earnings",
				Jsmn::Object(),
				Ln::CommandId::left(req_id)
			});
	}).then([&]() {
		// std::cerr << lastRsp.response.output() << std::endl;
		assert(rsp);
		assert(lastRsp.id == Ln::CommandId::left(req_id));
		auto result = Jsmn::Object::parse_json(lastRsp.response.output().c_str());
		assert(result["recent"] == Jsmn::Object::parse_json(R"JSON(
			{
			  "020000000000000000000000000000000000000000000000000000000000000001": {
			    "in_earnings": 336000,
			    "in_expenditures": 0,
			    "out_earnings": 0,
			    "out_expenditures": 14000
			  },
			  "020000000000000000000000000000000000000000000000000000000000000002": {
			    "in_earnings": 0,
			    "in_expenditures": 0,
			    "out_earnings": 336000,
			    "out_expenditures": 0
			  },
			  "020000000000000000000000000000000000000000000000000000000000000003": {
			    "in_earnings": 0,
			    "in_expenditures": 14000,
			    "out_earnings": 0,
			    "out_expenditures": 0
			  }
			}
                        )JSON"));
		assert(result["total"] == Jsmn::Object::parse_json(R"JSON(
			{
			  "in_earnings": 336000,
			  "in_expenditures": 14000,
			  "out_earnings": 336000,
			  "out_expenditures": 14000
			}
                        )JSON"));
		return Ev::lift();
	}).then([&]() {
		// Check a 30 day report
		++req_id;
		return bus.raise(Boss::Msg::CommandRequest{
				"clboss-recent-earnings",
				Jsmn::Object::parse_json(R"JSON( [30] )JSON"),
				Ln::CommandId::left(req_id)
			});
	}).then([&]() {
		// std::cerr << lastRsp.response.output() << std::endl;
		assert(rsp);
		assert(lastRsp.id == Ln::CommandId::left(req_id));
		auto result = Jsmn::Object::parse_json(lastRsp.response.output().c_str());
		// The expenditures stay the same because they are entirely in the last
		// week but the earnings increase.
		assert(result["recent"] == Jsmn::Object::parse_json(R"JSON(
			{
			  "020000000000000000000000000000000000000000000000000000000000000001": {
			    "in_earnings": 720000,
			    "in_expenditures": 0,
			    "out_earnings": 0,
			    "out_expenditures": 14000
			  },
			  "020000000000000000000000000000000000000000000000000000000000000002": {
			    "in_earnings": 0,
			    "in_expenditures": 0,
			    "out_earnings": 720000,
			    "out_expenditures": 0
			  },
			  "020000000000000000000000000000000000000000000000000000000000000003": {
			    "in_earnings": 0,
			    "in_expenditures": 14000,
			    "out_earnings": 0,
			    "out_expenditures": 0
			  }
			}
                        )JSON"));
		assert(result["total"] == Jsmn::Object::parse_json(R"JSON(
			{
			  "in_earnings": 720000,
			  "in_expenditures": 14000,
			  "out_earnings": 720000,
			  "out_expenditures": 14000
			}
                        )JSON"));
		return Ev::lift();
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(std::move(code));
}
