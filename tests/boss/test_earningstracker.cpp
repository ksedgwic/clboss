#undef NDEBUG

#include"Boss/Mod/EarningsTracker.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
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

int main() {
	// check the UTC quantitization boundaries
	assert(Boss::Mod::EarningsTracker::bucket_time(1722902400 - 1)		== 1722816000);
	assert(Boss::Mod::EarningsTracker::bucket_time(1722902400)		== 1722902400);
	assert(Boss::Mod::EarningsTracker::bucket_time(1722902400 + 86400 - 1)  == 1722902400);
	assert(Boss::Mod::EarningsTracker::bucket_time(1722902400 + 86400)      == 1722988800);

	auto bus = S::Bus();

	/* Module under test */
	Boss::Mod::EarningsTracker mut(bus, &mock_get_now);

	auto db = Sqlite3::Db(":memory:");

	Boss::Msg::ProvideStatus lastStatus;
	Boss::Msg::ResponseEarningsInfo lastEarningsInfo;

	bus.subscribe<Boss::Msg::ProvideStatus>([&](Boss::Msg::ProvideStatus const& st) {
		lastStatus = st;
		return Ev::lift();
	});

	bus.subscribe<Boss::Msg::ResponseEarningsInfo>([&](Boss::Msg::ResponseEarningsInfo const& ei) {
		lastEarningsInfo = ei;
		return Ev::lift();
	});

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::DbResource{ db });
	}).then([&]() {
		mock_now = 1000.0;
		return bus.raise(
			Boss::Msg::ForwardFee{
				A,			// in_id
				B,			// out_id
				Ln::Amount::sat(1),	// fee
				1.0			// resolution_time
			});
	}).then([&]() {
		mock_now = 2000.0;
		return bus.raise(Boss::Msg::SolicitStatus{});
	}).then([&]() {
		// std::cerr << lastStatus.value.output() << std::endl;
		assert(lastStatus.key == "offchain_earnings_tracker");
		assert(
			Jsmn::Object::parse_json(lastStatus.value.output().c_str()) ==
			Jsmn::Object::parse_json(R"JSON(
		{
		  "020000000000000000000000000000000000000000000000000000000000000001": {
		    "in_earnings": 1000,
		    "in_expenditures": 0,
		    "out_earnings": 0,
		    "out_expenditures": 0
		  },
		  "020000000000000000000000000000000000000000000000000000000000000002": {
		    "in_earnings": 0,
		    "in_expenditures": 0,
		    "out_earnings": 1000,
		    "out_expenditures": 0
		  },
		  "total": {
		    "in_earnings": 1000,
		    "in_expenditures": 0,
		    "out_earnings": 1000,
		    "out_expenditures": 0
		  }
		}
                        )JSON"));
		return Ev::lift();
	}).then([&]() {
		mock_now = 3000.0;
		return bus.raise(
			Boss::Msg::ForwardFee{
				A,			// in_id
				B,			// out_id
				Ln::Amount::sat(1),	// fee
				1.0			// resolution_time
			});
	}).then([&]() {
		return bus.raise(Boss::Msg::SolicitStatus{});
	}).then([&]() {
		// std::cerr << lastStatus.value.output() << std::endl;
		assert(lastStatus.key == "offchain_earnings_tracker");
		assert(
			Jsmn::Object::parse_json(lastStatus.value.output().c_str()) ==
			Jsmn::Object::parse_json(R"JSON(
		{
		  "020000000000000000000000000000000000000000000000000000000000000001": {
		    "in_earnings": 2000,
		    "in_expenditures": 0,
		    "out_earnings": 0,
		    "out_expenditures": 0
		  },
		  "020000000000000000000000000000000000000000000000000000000000000002": {
		    "in_earnings": 0,
		    "in_expenditures": 0,
		    "out_earnings": 2000,
		    "out_expenditures": 0
		  },
		  "total": {
		    "in_earnings": 2000,
		    "in_expenditures": 0,
		    "out_earnings": 2000,
		    "out_expenditures": 0
		  }
		}
                        )JSON"));
		return Ev::lift();
	}).then([&]() {
		mock_now = 4000.0;
		return bus.raise(
			Boss::Msg::RequestMoveFunds{
				NULL,			// requester (match ResponseMoveFunds)
				C,			// source
				A,			// destination
				Ln::Amount::sat(1000),	// amount
				Ln::Amount::sat(3)	// fee_budget
			});
	}).then([&]() {
		mock_now = 5000.0;
		return bus.raise(
			Boss::Msg::ResponseMoveFunds{
				NULL,			// requester (match RequestMoveFunds)
				Ln::Amount::sat(1000),	// amount_moved
				Ln::Amount::sat(2)	// fee_spent
			});
	}).then([&]() {
		return bus.raise(Boss::Msg::SolicitStatus{});
	}).then([&]() {
		// std::cerr << lastStatus.value.output() << std::endl;
		assert(lastStatus.key == "offchain_earnings_tracker");
		assert(
			Jsmn::Object::parse_json(lastStatus.value.output().c_str()) ==
			Jsmn::Object::parse_json(R"JSON(
		{
		  "020000000000000000000000000000000000000000000000000000000000000001": {
		    "in_earnings": 2000,
		    "in_expenditures": 0,
		    "out_earnings": 0,
		    "out_expenditures": 2000
		  },
		  "020000000000000000000000000000000000000000000000000000000000000002": {
		    "in_earnings": 0,
		    "in_expenditures": 0,
		    "out_earnings": 2000,
		    "out_expenditures": 0
		  },
		  "020000000000000000000000000000000000000000000000000000000000000003": {
		    "in_earnings": 0,
		    "in_expenditures": 2000,
		    "out_earnings": 0,
		    "out_expenditures": 0
		  },
		  "total": {
		    "in_earnings": 2000,
		    "in_expenditures": 2000,
		    "out_earnings": 2000,
		    "out_expenditures": 2000
		  }
		}
                        )JSON"));
		return Ev::lift();
	}).then([&]() {
		mock_now = 4000.0;
		return bus.raise(
			Boss::Msg::RequestEarningsInfo{
				NULL,			// requester (match ResponseEarningsInfo)
				A			// node
			});
	}).then([&]() {
		return Ev::yield(42);
	}).then([&]() {
		assert(lastEarningsInfo.node == A);
		assert(lastEarningsInfo.in_earnings == Ln::Amount::msat(2000));
		assert(lastEarningsInfo.in_expenditures == Ln::Amount::msat(0));
		assert(lastEarningsInfo.out_earnings == Ln::Amount::msat(0));
		assert(lastEarningsInfo.out_expenditures == Ln::Amount::msat(2000));
		return Ev::lift();
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(std::move(code));
}
