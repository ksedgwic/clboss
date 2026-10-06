#undef NDEBUG
#include"Boss/Mod/ChannelCandidateInvestigator/Gumshoe.hpp"
#include"Boss/Msg/RequestConnect.hpp"
#include"Boss/Msg/RequestRpcCommand.hpp"
#include"Boss/Msg/ResponseConnect.hpp"
#include"Boss/Msg/ResponseRpcCommand.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<map>
#include<set>
#include<vector>

namespace {

auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000");
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const C = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
auto const D = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");

class DummyConnector {
private:
	S::Bus& bus;

	std::set<std::string> pending_requests;
	struct Waiter {
		std::function<void()> pass;
		bool success;
	};
	std::map<std::string, Waiter> pending_waiters;

	typedef Boss::Msg::RequestConnect RequestConnect;
	typedef Boss::Msg::ResponseConnect ResponseConnect;

	void start() {
		bus.subscribe<RequestConnect>([this](RequestConnect const& rc) {
			auto it = pending_waiters.find(rc.node);
			if (it != pending_waiters.end()) {
				auto pass = std::move(it->second.pass);
				auto success = it->second.success;
				pending_waiters.erase(it);
				return respond( rc.node, success
					      ).then([pass]() {
					pass();
					return Ev::lift();
				});
			} else {
				pending_requests.insert(rc.node);
				return Ev::lift();
			}
		});
	}

	Ev::Io<void> respond(std::string node, bool success) {
		return bus.raise(ResponseConnect{
			node, success
		}).then([]() {
			return Ev::yield();
		}).then([]() {
			return Ev::yield();
		});
	}

public:
	explicit
	DummyConnector(S::Bus& bus_) : bus(bus_) { start(); }

	Ev::Io<void> wait(std::string node, bool success) {
		return Ev::lift().then([this, node, success]() {
			auto it = pending_requests.find(node);
			if (it != pending_requests.end()) {
				pending_requests.erase(it);
				return respond(node, success);
			}
			return Ev::Io<void>([this, node, success
					    ]( std::function<void()> pass
					     , std::function<void(std::exception_ptr)> _
					     ) {
				pending_waiters[node] = Waiter{pass, success};
			});
		});
	}
};

}

int main() {
	auto bus = S::Bus();
	Boss::Mod::ChannelCandidateInvestigator::Gumshoe g(bus);

	auto last_node = Ln::NodeId();
	auto last_success = bool();
	auto report = [&](Ln::NodeId node, bool success) {
		last_node = node;
		last_success = success;
		return Ev::lift();
	};
	g.set_report_func(report);

	auto connector = DummyConnector(bus);

	/* Mock `listpeers` and `disconnect`.  */
	auto connected = std::set<std::string>();
	auto disconnected = std::vector<std::string>();
	bus.subscribe<Boss::Msg::RequestRpcCommand
		     >([&](Boss::Msg::RequestRpcCommand const& m) {
		auto params = Jsmn::Object::parse_json(
			m.params.output().c_str()
		);
		auto id = std::string(params["id"]);

		auto text = std::string("{}");
		if (m.command == "listpeers") {
			if (connected.count(id) != 0)
				text = "{\"peers\": [{\"id\": \"" + id + "\""
				     + ", \"connected\": true}]}"
				     ;
			else
				text = "{\"peers\": []}";
		} else {
			assert(m.command == "disconnect");
			disconnected.push_back(id);
		}

		auto response = Boss::Msg::ResponseRpcCommand();
		response.requester = m.requester;
		response.succeeded = true;
		response.result = Jsmn::Object::parse_json(text.c_str());

		return bus.raise(std::move(response));
	});

	/* Every connect that is requested.  */
	auto requested = std::multiset<std::string>();
	bus.subscribe<Boss::Msg::RequestConnect
		     >([&](Boss::Msg::RequestConnect const& rc) {
		requested.insert(rc.node);
		return Ev::lift();
	});

	auto code = Ev::lift().then([&]() {

		/* Simple investigate/report check.  */
		last_node = Ln::NodeId();
		return g.investigate(A);
	}).then([&]() {
		/* Should not have reported yet!  */
		assert(last_node != A);

		/* Wait for the request from the investigator.  */
		return connector.wait(std::string(A), true);
	}).then([&]() {
		/* Should have reported it now.  */
		assert(last_node == A);
		assert(last_success);
		/* And dropped the connection it made.  */
		assert(disconnected.size() == 1);
		assert(disconnected.back() == std::string(A));

		/* Unrelated connects should be ignored.  */
		return bus.raise(Boss::Msg::RequestConnect{
			std::string(B)
		});
	}).then([&]() {
		return connector.wait(std::string(B), true);
	}).then([&]() {
		/* Should not change.  */
		assert(last_node == A);
		assert(last_success);
		/* That connection is not ours to drop.  */
		assert(disconnected.size() == 1);

		/* Multiple under investigation.  */
		return g.investigate(A);
	}).then([&]() {
		return g.investigate(B);
	}).then([&]() {
		return g.investigate(C);
	}).then([&]() {
		return connector.wait(std::string(C), false);
	}).then([&]() {
		/* Should report C.  */
		assert(last_node == C);
		assert(!last_success);
		/* Nothing to disconnect after a failed connect.  */
		assert(disconnected.size() == 1);

		/* Re-raising C should not change report.  */
		last_node = Ln::NodeId();
		return bus.raise(Boss::Msg::RequestConnect{
			std::string(C)
		});
	}).then([&]() {
		return connector.wait(std::string(C), true);
	}).then([&]() {
		/* Should not change.  */
		assert(last_node != C);

		/* Now trigger report on A.  */
		return connector.wait(std::string(A), true);
	}).then([&]() {
		/* Should report C.  */
		assert(last_node == A);
		assert(last_success);
		assert(disconnected.size() == 2);
		assert(disconnected.back() == std::string(A));

		/* A node that is already connected is online, without
		 * a connect, and stays connected.  */
		connected.insert(std::string(D));
		return g.investigate(D);
	}).then([&]() {
		return Ev::yield(25);
	}).then([&]() {
		assert(last_node == D);
		assert(last_success);
		assert(requested.count(std::string(D)) == 0);
		assert(disconnected.size() == 2);

		return Ev::lift(0);
	});

	return Ev::start(code);
}
