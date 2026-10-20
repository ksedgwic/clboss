#undef NDEBUG
#include"Boss/Mod/SwapManager.hpp"
#include"Boss/Msg/AcceptSwapQuotation.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/ListfundsResult.hpp"
#include"Boss/Msg/PayInvoice.hpp"
#include"Boss/Msg/ProvideSwapQuotation.hpp"
#include"Boss/Msg/RequestNewaddr.hpp"
#include"Boss/Msg/ResponseNewaddr.hpp"
#include"Boss/Msg/SolicitSwapQuotation.hpp"
#include"Boss/Msg/SwapCreation.hpp"
#include"Boss/Msg/SwapRequest.hpp"
#include"Boss/Msg/SwapResponse.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include"Uuid.hpp"
#include<assert.h>
#include<iostream>
#include<memory>
#include<queue>
#include<set>
#include<sstream>
#include<unordered_map>
#include<unordered_set>

namespace {

/*-----------------------------------------------------------------------------
Mocks
-----------------------------------------------------------------------------*/

class MockLog {
public:
	MockLog(S::Bus& bus) {
		bus.subscribe<Boss::Msg::JsonCout
			     >([](Boss::Msg::JsonCout const& j) {
			std::cout << j.obj.output() << std::endl;
			return Ev::lift();
		});
	}
};
class MockNewaddr {
private:
	std::queue<std::string> addresses;
	bool swallow_next_flag = false;
public:
	MockNewaddr(S::Bus& bus) {
		bus.subscribe<Boss::Msg::RequestNewaddr
			     >([ this, &bus
			       ](Boss::Msg::RequestNewaddr const& r) {
			/* Simulate the newaddr chain dying mid-flight:
			 * no response ever arrives.  */
			if (swallow_next_flag) {
				swallow_next_flag = false;
				return Ev::lift();
			}
			assert(!addresses.empty());
			auto addr = addresses.front();
			addresses.pop();
			return bus.raise(Boss::Msg::ResponseNewaddr{
				addr, r.requester
			});
		});
	}
	void add(std::string addr) {
		addresses.emplace(std::move(addr));
	}
	void swallow_next() { swallow_next_flag = true; }
	bool empty() const { return addresses.empty(); }
};
struct MockProvider {};
std::vector<std::unique_ptr<MockProvider>> providers;
class MockSwapQuotation {
private:
	/* A vector, since we want to check what happens when multiple
	 * quotations exist.
	 */
	std::queue<std::vector<Ln::Amount>> fees;
public:
	MockSwapQuotation(S::Bus& bus) {
		using Boss::Msg::ProvideSwapQuotation;
		using Boss::Msg::SolicitSwapQuotation;
		bus.subscribe<SolicitSwapQuotation
			     >([ this
			       , &bus
			       ](SolicitSwapQuotation const& s) {
			assert(!fees.empty());
			auto set = std::move(fees.front());
			fees.pop();

			/* Ensure there are enough providers.  */
			while (set.size() > providers.size()) {
				providers.push_back(
					Util::make_unique<MockProvider>()
				);
			}
			/* Create the messages.  */
			auto messages = std::vector<ProvideSwapQuotation>();
			for (auto i = std::size_t(0); i < set.size(); ++i) {
				messages.emplace_back(ProvideSwapQuotation{
					set[i],
					s.solicitor,
					providers[i].get(),
					"MockSwapQuotation"
				});
			}
			/* Send them.  */
			auto f = [&bus](ProvideSwapQuotation q) {
				return bus.raise(std::move(q));
			};
			return Ev::foreach(f, std::move(messages));
		});
	}

	void add(std::vector<Ln::Amount> set) {
		fees.emplace(std::move(set));
	}
	bool empty() const { return fees.empty(); }
};

class MockSwapCreation {
public:
	struct Entry {
		/* The expected address, if not match, fail.  */
		std::string address;
		/* Details.  */
		bool success;
		std::string invoice;
		Sha256::Hash hash;
		std::uint32_t timeout;
	};
private:
	std::queue<Entry> entries;
	std::set<std::string> invoices;
public:
	MockSwapCreation(S::Bus& bus) {
		using Boss::Msg::AcceptSwapQuotation;
		using Boss::Msg::PayInvoice;
		using Boss::Msg::SwapCreation;
		bus.subscribe<AcceptSwapQuotation
			     >([ this
			       , &bus
			       ](AcceptSwapQuotation const& q) {
			assert(!entries.empty());
			auto entry = std::move(entries.front());
			entries.pop();

			/* Validate.  */
			assert(q.onchain_address == entry.address);
			auto found = false;
			for (auto const& p : providers) {
				if (p.get() == q.provider) {
					found = true;
					break;
				}
			}
			assert(found);

			/* If success, add invoice to set of invoices
			 * we expect.  */
			if (entry.success)
				invoices.insert(entry.invoice);

			return bus.raise(SwapCreation{
				entry.success,
				entry.invoice, entry.hash, entry.timeout,
				q.solicitor, q.provider
			});
		});
		bus.subscribe<PayInvoice
			     >([this](PayInvoice const& p) {
			auto it = invoices.find(p.invoice);
			assert(it != invoices.end());
			invoices.erase(it);
			return Ev::lift();
		});
	}

	void add(Entry e) {
		entries.emplace(std::move(e));
	}
	bool okay() const {
		return entries.empty() && invoices.empty();
	}
};
class MockListfunds {
private:
	S::Bus& bus;
public:
	MockListfunds(S::Bus& bus_) : bus(bus_) { }
	Ev::Io<void>
	listfunds(std::vector<std::pair< std::string
					, Ln::Amount
					>> const& addr_amount) {
		/* Generate first via Json::Out.  */
		auto outputs = Json::Out();
		auto arr = outputs.start_array();
		for (auto const& e : addr_amount) {
			auto obj = arr.start_object();
			obj.field("address", e.first);
			obj.field("amount_msat", std::string(e.second));
			obj.end_object();
		}
		arr.end_array();
		/* Convert to Jsmn::Object.  */
		auto outputs_s = outputs.output();
		auto is = std::istringstream(std::move(outputs_s));
		auto outputs_j = Jsmn::Object();
		is >> outputs_j;

		return bus.raise(Boss::Msg::ListfundsResult{
			std::move(outputs_j), Jsmn::Object()
		});
	}
};
class MockRequester {
private:
	S::Bus& bus;
	Sqlite3::Db db;
	std::unordered_set<Uuid> pendings;
	std::unordered_map<Uuid, bool> finished;

public:
	MockRequester(S::Bus& bus_) : bus(bus_) {
		bus.subscribe<Boss::Msg::DbResource
			     >([this](Boss::Msg::DbResource const& r) {
			db = r.db;
			return Ev::lift();
		});
		bus.subscribe<Boss::Msg::SwapResponse
			     >([this](Boss::Msg::SwapResponse const& r) {
			auto tx = std::move(*r.dbtx);

			auto it = pendings.find(r.id);
			assert(it != pendings.end());
			pendings.erase(it);

			auto it_m = finished.find(r.id);
			assert(it_m == finished.end());
			finished[r.id] = r.success;

			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> start_swap(Uuid uuid, Ln::Amount min, Ln::Amount max) {
		return db.transact().then([this, uuid, min, max
					  ](Sqlite3::Tx tx) {
			pendings.insert(uuid);
			auto sh_tx = std::make_shared<Sqlite3::Tx>(
				std::move(tx)
			);
			return bus.raise(Boss::Msg::SwapRequest{
				sh_tx, uuid, min, max
			}).then([sh_tx]() {
				assert(sh_tx);
				assert(!*sh_tx);
				return Ev::lift();
			});
		});
	}

	void expect_finished(Uuid uuid, bool expect_success) {
		auto it = finished.find(uuid);
		assert(it != finished.end());
		assert(it->second == expect_success);
	}
};

Ev::Io<void> wait() {
	auto act = Ev::yield();
	for (auto i = std::size_t(0); i < 200; ++i)
		act = std::move(act).then([]() { return Ev::yield(); });
	return act;
}

/*-----------------------------------------------------------------------------
Test Harness
-----------------------------------------------------------------------------*/

S::Bus bus;
Sqlite3::Db db(":memory:");
MockLog logger(bus);
MockNewaddr addresses(bus);
MockSwapQuotation quotations(bus);
MockSwapCreation swapsetups(bus);
MockListfunds onchain(bus);
MockRequester requester(bus);
/* Object under test.  */
Boss::Mod::SwapManager swapper(bus);

/*-----------------------------------------------------------------------------
Tests
-----------------------------------------------------------------------------*/

Ev::Io<void> test_simple() {

	Uuid uuid = Uuid::random();

	return Ev::lift().then([&, uuid]() {
		addresses.add("address1");
		quotations.add({Ln::Amount::sat(1000), Ln::Amount::sat(100)});
		/* Fail the first provider, but succeed the next one.  */
		swapsetups.add({"address1", false, "", Sha256::Hash(), 0});
		swapsetups.add({"address1", true, "invoice1"
			       , Sha256::Hash("ec8103d3f57a561fd213bf6bb45b2cd0e1b37738232c7d1fd13b1a7729bf0982")
			       , 12345
			       });

		return requester.start_swap( uuid
					   , Ln::Amount::btc(0.001)
					   , Ln::Amount::btc(0.012)
					   );
	}).then([&]() {
		return wait();
	}).then([&]() {
		assert(addresses.empty());
		assert(quotations.empty());
		assert(swapsetups.okay());

		return onchain.listfunds({ { "unrelated-address"
					   , Ln::Amount::sat(42)
					   }
					 });
	}).then([&]() {
		return onchain.listfunds({ { "unrelated-address"
					   , Ln::Amount::sat(42)
					   }
					 , { "address1"
					   , Ln::Amount::sat(12100)
					   }
					 });
	}).then([&]() {
		return wait();
	}).then([&, uuid]() {
		requester.expect_finished(uuid, true);
		return Ev::lift();
	});
}

/* A newaddr chain that never responds must not wedge the address
 * loop forever: the periodic tick's watchdog clears the stuck
 * serialization flag and the restarted loop retries the swap.  */
Ev::Io<void> test_stall_recovery() {

	Uuid uuid = Uuid::random();

	return Ev::lift().then([&, uuid]() {
		/* Make the watchdog fire on the next tick.  */
		swapper.set_stall_timeout_secs(0.0);

		/* The address request will get no response, wedging
		 * the loop with its flag set.  */
		addresses.swallow_next();
		return requester.start_swap( uuid
					   , Ln::Amount::btc(0.001)
					   , Ln::Amount::btc(0.012)
					   );
	}).then([&]() {
		return wait();
	}).then([&]() {
		/* Stock the mocks for the retry, then tick: the
		 * watchdog resets the flag and the restarted loop
		 * re-requests an address.  */
		addresses.add("address2");
		quotations.add({Ln::Amount::sat(1000)});
		swapsetups.add({"address2", true, "invoice2"
			       , Sha256::Hash("ec8103d3f57a561fd213bf6bb45b2cd0e1b37738232c7d1fd13b1a7729bf0983")
			       , 23456
			       });
		return bus.raise(Boss::Msg::Timer10Minutes{});
	}).then([&]() {
		return wait();
	}).then([&]() {
		assert(addresses.empty());
		assert(quotations.empty());
		assert(swapsetups.okay());

		return onchain.listfunds({ { "address2"
					   , Ln::Amount::sat(12100)
					   }
					 });
	}).then([&]() {
		return wait();
	}).then([&, uuid]() {
		requester.expect_finished(uuid, true);
		return Ev::lift();
	});
}

}

/*-----------------------------------------------------------------------------
Setup
-----------------------------------------------------------------------------*/

int main() {
	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::DbResource{db});
	}).then([&]() {
		return test_simple();
	}).then([&]() {
		return test_stall_recovery();
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
