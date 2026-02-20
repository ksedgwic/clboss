#undef NDEBUG
#include"Boss/Mod/JsonOutputter.hpp"
#include"Boss/Mod/EarningsRebalancer.hpp"
#include"Boss/Mod/RebalanceUnmanager.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Mod/ConstructedListpeers.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestGetCircularRebalanceFlag.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseGetCircularRebalanceFlag.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Json/Out.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<iostream>
#include<map>
#include<sstream>

namespace {

/* A model for channels with nodes.  */
class Model {
public:
	/* Information about a channel.  */
	struct ChanInfo {
		Ln::Amount to_us;
		Ln::Amount total;
		Ln::Amount out_earnings;
		Ln::Amount out_expenditures;
		Ln::Amount in_earnings;
		Ln::Amount in_expenditures;
	};
	std::map<Ln::NodeId, ChanInfo> peers;

private:
	S::Bus& bus;
	void start() {
		bus.subscribe< Boss::Msg::RequestEarningsInfo
			     >([this](Boss::Msg::RequestEarningsInfo const& m) {
			auto res = Boss::Msg::ResponseEarningsInfo();
			res.requester = m.requester;
			res.node = m.node;

			auto it = peers.find(m.node);
			if (it == peers.end()) {
				res.in_earnings = Ln::Amount::sat(0);
				res.in_expenditures = Ln::Amount::sat(0);
				res.out_earnings = Ln::Amount::sat(0);
				res.out_expenditures = Ln::Amount::sat(0);
			} else {
				res.in_earnings = it->second.in_earnings;
				res.in_expenditures = it->second.in_expenditures;
				res.out_earnings = it->second.out_earnings;
				res.out_expenditures = it->second.out_expenditures;
			}

			return bus.raise(std::move(res));
		});
	}

public:
	explicit
	Model(S::Bus& bus_) : bus(bus_) { start(); }

	Ev::Io<void> listpeers() {
		return Ev::lift().then([this]() {
			auto j = Json::Out();
			auto arr = j.start_array();
			for (auto const& p : peers) {
				arr.start_object()
					.field("id", std::string(p.first))
					.start_array("channels")
						.start_object()
							.field("state", "CHANNELD_NORMAL")
							.field("to_us_msat", std::string(p.second.to_us))
							.field("total_msat", std::string(p.second.total))
							.start_array("htlcs")
							.end_array()
						.end_object()
					.end_array()
				.end_object();
			}
			arr.end_array();

			auto is = std::istringstream(j.output());
			auto js = Jsmn::Object();
			is >> js;
			return bus.raise(Boss::Msg::ListpeersResult{
					std::move(Boss::Mod::convert_legacy_listpeers(js)), false
			});
		});
	}
};

Ev::Io<void> multiyield() {
	return Ev::yield(100);
}

/* Node IDs.  */
auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000");
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const C = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
auto const D = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");
/* Unmanaged node IDs.  */
auto const Y = Ln::NodeId("0200000000000000000000000000000000000000000000000000000000000000FE");
auto const Z = Ln::NodeId("0200000000000000000000000000000000000000000000000000000000000000FF");

}

int main() {
	auto bus = S::Bus();

	/* Utility outputter.  */
	Boss::Mod::JsonOutputter cout(std::cout, bus);
	/* Unmanager.  */
	Boss::Mod::RebalanceUnmanager unmanager(bus, {
		"0200000000000000000000000000000000000000000000000000000000000000FE",
		"0200000000000000000000000000000000000000000000000000000000000000FF"
	});

	/* Testing model.  */
	Model model(bus);
	auto& peers = model.peers;
	/* Function to trigger the earnings rebalancer.  */
	auto trigger_id = std::uint64_t(0);
	auto trigger = [&bus, &trigger_id]() {
		return bus.raise(Boss::Msg::CommandRequest{
			"clboss-earnings-rebalancer",
			Jsmn::Object(),
			Ln::CommandId::left(trigger_id++)
		}) + multiyield();
	};
	/* Keep track of funds movement.  */
	auto funds_moved = false;
	auto source = Ln::NodeId();
	auto destination = Ln::NodeId();
	bus.subscribe<Boss::Msg::RequestMoveFunds
		     >([&](Boss::Msg::RequestMoveFunds const& m) {
		funds_moved = true;
		source = m.source;
		destination = m.destination;
		return Ev::lift();
	});
	bus.subscribe< Boss::Msg::RequestGetCircularRebalanceFlag
		     >([&bus](Boss::Msg::RequestGetCircularRebalanceFlag const& m) {
			auto msg = Boss::Msg::ResponseGetCircularRebalanceFlag();
			msg.requester = m.requester;
			msg.state = true;
			msg.comment = "test";
			return bus.raise(msg);
	});

	/* Module under test.  */
	auto mut = Boss::Mod::EarningsRebalancer(bus);

	auto code = Ev::lift().then([&]() {

		/* Start with an empty set of peers.  */
		funds_moved = false;
		peers.clear();
		return model.listpeers() + trigger();
	}).then([&]() {
		/* Nothing for it to do...   */
		assert(funds_moved == false);

		/* Give it some unmanaged peers.  */
		funds_moved = false;
		peers.clear();
		peers[Y].to_us = Ln::Amount::msat(0);
		peers[Y].total = Ln::Amount::msat(10000000);
		peers[Y].in_earnings = Ln::Amount::msat(100000);
		peers[Y].in_expenditures = Ln::Amount::msat(1);
		peers[Y].out_earnings = Ln::Amount::msat(100000);
		peers[Y].out_expenditures = Ln::Amount::msat(1);
		peers[Z].to_us = Ln::Amount::msat(10000000);
		peers[Z].total = Ln::Amount::msat(10000000);
		peers[Z].in_earnings = Ln::Amount::msat(100000);
		peers[Z].in_expenditures = Ln::Amount::msat(1);
		peers[Z].out_earnings = Ln::Amount::msat(100000);
		peers[Z].out_expenditures = Ln::Amount::msat(1);
		return model.listpeers() + trigger();
	}).then([&]() {
		/* Nothing for it to do...   */
		assert(funds_moved == false);

		/* Now give it some peers that are all high
		 * on our side.
		 */
		funds_moved = false;
		peers.clear();
		peers[A].to_us = Ln::Amount::msat(1000000);
		peers[A].total = Ln::Amount::msat(1100000);
		peers[B].to_us = Ln::Amount::msat(1000000);
		peers[B].total = Ln::Amount::msat(1100000);
		peers[C].to_us = Ln::Amount::msat(1000000);
		peers[C].total = Ln::Amount::msat(1100000);
		peers[D].to_us = Ln::Amount::msat(1000000);
		peers[D].total = Ln::Amount::msat(1100000);
		return model.listpeers() + trigger();
	}).then([&]() {
		/* Nothing for it to do...  */
		assert(funds_moved == false);

		/* Now give a mix of peers, but only some are
		 * high-earnings.  */
		funds_moved = false;
		peers.clear();
		peers[A].to_us = Ln::Amount::msat(0);
		peers[A].total = Ln::Amount::msat(1100000);
		peers[A].out_earnings = Ln::Amount::msat(1000);
		peers[A].out_expenditures = Ln::Amount::msat(0);
		peers[B].to_us = Ln::Amount::msat(0);
		peers[B].total = Ln::Amount::msat(1100000);
		peers[B].out_earnings = Ln::Amount::msat(1000);
		peers[B].out_expenditures = Ln::Amount::msat(10000);
		peers[C].to_us = Ln::Amount::msat(1000000);
		peers[C].total = Ln::Amount::msat(1100000);
		peers[C].in_earnings = Ln::Amount::msat(1000);
		peers[C].in_expenditures = Ln::Amount::msat(0);
		peers[D].to_us = Ln::Amount::msat(1000000);
		peers[D].total = Ln::Amount::msat(1100000);
		peers[D].in_earnings = Ln::Amount::msat(1000);
		peers[D].in_expenditures = Ln::Amount::msat(10000);
		/* Add an unmanaged node.  */
		peers[Z].to_us = Ln::Amount::msat(100);
		peers[Z].total = Ln::Amount::msat(1000000);
		peers[Z].out_earnings = Ln::Amount::msat(10000);
		peers[Z].in_earnings = Ln::Amount::msat(10000);
		return model.listpeers() + trigger();
	}).then([&]() {
		/* Should trigger move from C to A.  */
		assert(funds_moved == true);
		assert(source == C);
		assert(destination == A);

		//TODO

		return Ev::lift(0);
	});

	return Ev::start(code);
}

