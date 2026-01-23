#undef NDEBUG

#include"Boss/Mod/FeeMonitor.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/MonitorFeeByBalance.hpp"
#include"Boss/Msg/MonitorFeeBySize.hpp"
#include"Boss/Msg/MonitorFeeByTheory.hpp"
#include"Boss/Msg/MonitorFeeSetChannel.hpp"
#include"Boss/Msg/PeerMedianChannelFee.hpp"
#include"Ev/coroutine.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"

#include<assert.h>
#include<cstdint>
#include<iomanip>
#include<sstream>

namespace {
auto const A = Ln::NodeId(
	"020000000000000000000000000000000000000000000000000000000000000001"
);
auto const B = Ln::NodeId(
	"020000000000000000000000000000000000000000000000000000000000000002"
);
auto const C = Ln::NodeId(
	"020000000000000000000000000000000000000000000000000000000000000003"
);
auto const far_past = std::int64_t(0);
auto const far_future = std::int64_t(4102444800);

std::string make_since_query(char const* nodeid, std::int64_t since) {
	auto os = std::ostringstream();
	os << std::setprecision(17)
	   << "{\"nodeid\":\"" << nodeid << "\",\"since\":" << since << "}";
	return os.str();
}

std::string make_before_query(char const* nodeid, std::int64_t before) {
	auto os = std::ostringstream();
	os << std::setprecision(17)
	   << "{\"nodeid\":\"" << nodeid << "\",\"before\":" << before << "}";
	return os.str();
}
}

Ev::Io<int> run() {
	auto bus = S::Bus();
	auto mut = Boss::Mod::FeeMonitor(bus);
	auto db = Sqlite3::Db(":memory:");

	auto req_id = std::uint64_t();
	auto last_rsp = Boss::Msg::CommandResponse{};
	auto rsp = false;
	bus.subscribe<Boss::Msg::CommandResponse
		     >([&](Boss::Msg::CommandResponse const& m) {
		last_rsp = m;
		rsp = true;
		return Ev::yield();
	});

	co_await bus.raise(Boss::Msg::DbResource{db});
	co_await bus.raise(Boss::Msg::PeerMedianChannelFee{A, 10, 100});
	co_await bus.raise(Boss::Msg::MonitorFeeBySize{A, 10, 3, 1.1});
	co_await bus.raise(Boss::Msg::MonitorFeeByBalance{A, 1.2, 1000, 2000});
	co_await bus.raise(Boss::Msg::MonitorFeeByTheory{A, 5, 1.3});
	co_await bus.raise(Boss::Msg::MonitorFeeSetChannel{A, 1000, 10});
	co_await bus.raise(Boss::Msg::MonitorFeeSetChannel{B, 3000, 30});

	++req_id;
	rsp = false;
	co_await bus.raise(Boss::Msg::CommandRequest{
			"clboss-feemon-history",
			Jsmn::Object::parse_json(
				"{\"nodeid\":\"020000000000000000000000000000000000000000000000000000000000000001\"}"
			),
			Ln::CommandId::left(req_id)
		});
	assert(rsp);
	assert(last_rsp.id == Ln::CommandId::left(req_id));
	auto result = Jsmn::Object::parse_json(
		last_rsp.response.output().c_str()
	);
	auto history = result["history"];
	assert(history.size() == 1);
	assert(double(history[0]["set_base"]) == 1000.0);

	++req_id;
	rsp = false;
	co_await bus.raise(Boss::Msg::CommandRequest{
			"clboss-feemon-history",
			Jsmn::Object::parse_json(
				make_since_query(
					"020000000000000000000000000000000000000000000000000000000000000001",
					far_past
				).c_str()
			),
			Ln::CommandId::left(req_id)
		});
	assert(rsp);
	assert(last_rsp.id == Ln::CommandId::left(req_id));
	result = Jsmn::Object::parse_json(
		last_rsp.response.output().c_str()
	);
	assert(result["history"].size() == 1);
	assert(double(result["history"][0]["set_base"]) == 1000.0);

	++req_id;
	rsp = false;
	co_await bus.raise(Boss::Msg::CommandRequest{
			"clboss-feemon-history",
			Jsmn::Object::parse_json(
				make_before_query(
					"020000000000000000000000000000000000000000000000000000000000000001",
					far_past
				).c_str()
			),
			Ln::CommandId::left(req_id)
		});
	assert(rsp);
	assert(last_rsp.id == Ln::CommandId::left(req_id));
	result = Jsmn::Object::parse_json(
		last_rsp.response.output().c_str()
	);
	assert(result["history"].size() == 0);

	++req_id;
	rsp = false;
	co_await bus.raise(Boss::Msg::CommandRequest{
			"clboss-feemon-history",
			Jsmn::Object::parse_json(
				make_since_query(
					"020000000000000000000000000000000000000000000000000000000000000001",
					far_future
				).c_str()
			),
			Ln::CommandId::left(req_id)
		});
	assert(rsp);
	assert(last_rsp.id == Ln::CommandId::left(req_id));
	result = Jsmn::Object::parse_json(
		last_rsp.response.output().c_str()
	);
	assert(result["history"].size() == 0);

	++req_id;
	rsp = false;
	co_await bus.raise(Boss::Msg::CommandRequest{
			"clboss-feemon-history",
			Jsmn::Object::parse_json(
				make_before_query(
					"020000000000000000000000000000000000000000000000000000000000000001",
					far_future
				).c_str()
			),
			Ln::CommandId::left(req_id)
		});
	assert(rsp);
	assert(last_rsp.id == Ln::CommandId::left(req_id));
	result = Jsmn::Object::parse_json(
		last_rsp.response.output().c_str()
	);
	assert(result["history"].size() == 1);
	assert(double(result["history"][0]["set_base"]) == 1000.0);

	++req_id;
	rsp = false;
	co_await bus.raise(Boss::Msg::CommandRequest{
			"clboss-feemon-history",
			Jsmn::Object::parse_json(
				"{\"nodeid\":\"020000000000000000000000000000000000000000000000000000000000000002\"}"
			),
			Ln::CommandId::left(req_id)
		});
	assert(rsp);
	assert(last_rsp.id == Ln::CommandId::left(req_id));
	result = Jsmn::Object::parse_json(
		last_rsp.response.output().c_str()
	);
	assert(result["history"].size() == 1);
	assert(double(result["history"][0]["set_base"]) == 3000.0);

	++req_id;
	rsp = false;
	co_await bus.raise(Boss::Msg::CommandRequest{
			"clboss-feemon-history",
			Jsmn::Object::parse_json(
				"{\"nodeid\":\"020000000000000000000000000000000000000000000000000000000000000003\"}"
			),
			Ln::CommandId::left(req_id)
		});
	assert(rsp);
	assert(last_rsp.id == Ln::CommandId::left(req_id));
	result = Jsmn::Object::parse_json(
		last_rsp.response.output().c_str()
	);
	assert(result["history"].size() == 0);

	co_return 0;
}

int main() {
	auto io = run();
	auto ec = Ev::start(io);
	assert(ec == 0);
	return 0;
}
