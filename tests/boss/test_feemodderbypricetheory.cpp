#undef NDEBUG
#include"Boss/Mod/FeeModderByPriceTheory.hpp"
#include"Boss/Mod/FeeMonitor.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/MonitorFeeSetChannel.hpp"
#include"Boss/Msg/ProvideChannelFeeModifier.hpp"
#include"Boss/Msg/SolicitChannelFeeModifier.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include<assert.h>
#include<cstddef>
#include<deque>
#include<functional>
#include<iostream>
#include<map>
#include<math.h>
#include<random>
#include<vector>

namespace {

/* Set this preprocessor flag to use a more realistic
 * simulation.
 * Changes:
 * - Sometimes we just do not earn fees.
 * - Sometimes we earn fees even if we are far from
 *   the best price.
 * - We simulate longer time periods.
 */
#if TEST_FEEMODDERBYPRICETHEORY_REALISTIC
auto constexpr probability_no_earn = double(0.5);
auto constexpr probability_noise = double(0.08);
auto constexpr num_iterations = 10000;
#else
auto constexpr probability_no_earn = double(0.0);
auto constexpr probability_noise = double(0.0);
auto constexpr num_iterations = 3000;
#endif

auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");

/* Probability of a peer being simulated as disconnected.  */
auto constexpr prob_disconnect = 0.1;
/* Probability of asking for new multipliers.  */
auto constexpr prob_getmult = 0.16667;

/* The multiplier that has optimum price for A.  */
auto constexpr optimumA = 2.1;
/* The multiplier that has optimum price for B.  */
auto constexpr optimumB = 0.49;

/* Running mean with limited number of samples; only the last N samples are saved.  */
template<std::size_t N>
class LimitedMean {
private:
	std::deque<double> samples;
	std::size_t num_samples;

public:
	LimitedMean() : samples()
       		      , num_samples(0)
		      { }
	void sample(double s) {
		samples.push_back(s);
		++num_samples;
		while (num_samples > N) {
			samples.pop_front();
			--num_samples;
		}
	}
	double get() const {
		auto sum = double(0.0);
		for (auto& s : samples)
			sum += s;
		return sum / double(num_samples);
	}
};

/* Testing model.  */
class Tester {
private:
	S::Bus& bus;
	/* Provides a random number from 0.0 to just below 1.0.  */
	std::uniform_real_distribution<double> dist;

	std::size_t iterations_remaining;
	bool first;
	std::function< Ev::Io<double>( Ln::NodeId
				     , std::uint32_t
				     , std::uint32_t
				     )> modder;

	/* The latest multipliers.  */
	std::map<Ln::NodeId, double> multiplier;
	/* The running mean of the multipliers.  */
	std::map<Ln::NodeId, LimitedMean<300>> mean_multiplier;
	/* The optimal multipliers.  */
	std::map<Ln::NodeId, double> optimum;

	/* Given a particular node, whether to model earning a fee.  */
	bool should_earn(Ln::NodeId n) {
		/* LN nodes do not earn all that often.... */
		if ( (probability_no_earn != 0.0)
		  && (dist(Boss::random_engine) < probability_no_earn)
		   )
			return false;
		/* Sometimes noise just gets in.... */
		if ( (probability_noise != 0.0)
		  && (dist(Boss::random_engine) < probability_noise)
		   )
			return true;
		auto mult = multiplier[n];
		auto opt = optimum[n];
		auto distance = fabs(mult - opt);
		return (dist(Boss::random_engine) > (distance / 1.5));
	}

	Ev::Io<void> iteration() {
		return Ev::lift().then([this]() {
			/* Models channel fee querying.  */
			if ( !first
			  && dist(Boss::random_engine) >= prob_getmult
			   )
				return Ev::lift();

			return modder(A, 1, 1).then([this](double mA) {
				multiplier[A] = mA;
				mean_multiplier[A].sample(mA);

				return modder(B, 1, 1);
			}).then([this](double mB) {
				multiplier[B] = mB;
				mean_multiplier[B].sample(mB);

				return Ev::lift();
			});
		}).then([this]() {
			/* Models informing the module under test that
			 * particular peers are connected.  */
			auto connected = std::set<Ln::NodeId>();
			auto disconnected = std::set<Ln::NodeId>();
			if (dist(Boss::random_engine) >= prob_disconnect)
				connected.insert(A);
			else
				disconnected.insert(A);
			if (dist(Boss::random_engine) >= prob_disconnect)
				connected.insert(B);
			else
				disconnected.insert(B);

			return bus.raise(Boss::Msg::ListpeersAnalyzedResult{
				connected, disconnected,
				std::set<Ln::NodeId>(),
				std::set<Ln::NodeId>(),
				false
			});
		}).then([this]() {
			auto fun = [this](Ln::NodeId n) {
				return Ev::lift().then([this, n]() {
					if (!should_earn(n))
						return Ev::lift();
					return bus.raise(Boss::Msg::ForwardFee{
						n, n, Ln::Amount::msat(1),
						1.0
					});
				});
			};
			auto nodes = std::vector<Ln::NodeId>();
			for (auto const& np : optimum)
				nodes.push_back(np.first);
			return Ev::foreach(std::move(fun), std::move(nodes));
		}).then([this]() {
			first = false;
			return Ev::lift();
		});
	}

	Ev::Io<void> loop() {
		return Ev::yield().then([this]() {
			if (iterations_remaining == 0)
				return Ev::lift();
			--iterations_remaining;

			return iteration().then([this]() {
				return loop();
			});
		});
	}

public:
	Tester(S::Bus& bus_) : bus(bus_), dist(0.0, 1.0) { }
	Ev::Io<void> run() {
		iterations_remaining = num_iterations;
		first = true;
		modder = nullptr;
		optimum[A] = optimumA;
		optimum[B] = optimumB;

		bus.subscribe<Boss::Msg::ProvideChannelFeeModifier
			     >([this](Boss::Msg::ProvideChannelFeeModifier const& m) {
			assert(!modder);
			modder = m.modifier;
			return Ev::lift();
		});

		return Ev::lift().then([this]() {

			/* After raising this, modder should have been provided.  */
			return bus.raise(Boss::Msg::SolicitChannelFeeModifier{});
		}).then([this](){
			assert(modder);

			return loop();
		}).then([this]() {

			/* After simulating te number of iterations,
			 * we should be within some % of the optimum.
			 */
			auto within_reason = [](double actual, double expected) {
				/* Well, very very roughly, if the expected
				 * is less than 1, actual should be less
				 * than 1, and so on.
				 * This is very rough "within reason", we
				 * are ultimately testing that our design
				 * does not crash or do the opposite of
				 * what we expect.
				 */
				if (expected < 1.0)
					return actual < 1.0;
				else
					return actual > 1.0;
			};
			std::cout << "mean A = " << mean_multiplier[A].get() << std::endl;
			std::cout << "optm A = " << optimum[A] << std::endl;
			std::cout << "mean B = " << mean_multiplier[B].get() << std::endl;
			std::cout << "optm B = " << optimum[B] << std::endl;
			assert(within_reason( mean_multiplier[A].get()
					    , optimum[A]
					    ));
			assert(within_reason( mean_multiplier[B].get()
					    , optimum[B]
					    ));
			return Ev::lift();
		});
	}
};

}

int main() {
	auto bus = S::Bus();
	auto module_under_test = Boss::Mod::FeeModderByPriceTheory(bus);
	auto fee_monitor = Boss::Mod::FeeMonitor(bus);
	auto tester = Tester(bus);

	auto modder = std::function< Ev::Io<double>( Ln::NodeId
						   , std::uint32_t /* base */
						   , std::uint32_t /* proportional */
						   )>();
	bus.subscribe<Boss::Msg::ProvideChannelFeeModifier
		     >([&](Boss::Msg::ProvideChannelFeeModifier const& m) {
		assert(!modder);
		modder = m.modifier;
		return Ev::lift();
	});

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

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::DbResource{
			db
		});
	}).then([&]() {

		return tester.run();
	}).then([&]() {
		return bus.raise(Boss::Msg::MonitorFeeSetChannel{
			A, 1000, 10
		});
	}).then([&]() {
		++req_id;
		rsp = false;
		return bus.raise(Boss::Msg::CommandRequest{
			"clboss-feemon-history",
			Jsmn::Object::parse_json(
				"[\"020000000000000000000000000000000000000000000000000000000000000001\"]"
			),
			Ln::CommandId::left(req_id)
		});
	}).then([&]() {
		assert(rsp);
		assert(last_rsp.id == Ln::CommandId::left(req_id));
		auto result = Jsmn::Object::parse_json(
			last_rsp.response.output().c_str()
		);
		assert(result["history"].size() == 1);
		assert(double(result["history"][0]["set_base"]) == 1000.0);

		return Ev::lift(0);
	});

	return Ev::start(std::move(code));
}
