#undef NDEBUG
#include"Boss/Mod/ChannelFeeManager.hpp"
#include"Boss/Msg/PeerMedianChannelFee.hpp"
#include"Boss/Msg/ProvideChannelFeeModifier.hpp"
#include"Boss/Msg/RecomputeChannelFee.hpp"
#include"Boss/Msg/SetChannelFee.hpp"
#include"Boss/Msg/SolicitChannelFeeModifier.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<cstddef>
#include<cstdint>
#include<memory>
#include<vector>

/* Boss::Msg::RecomputeChannelFee.
 *
 * The fee of a peer is computed again from the median last received
 * for it, with the modifiers asked again, and it is set before the
 * message returns.  For a peer without a median nothing is computed
 * and `recomputed` says so.
 */

namespace {

auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");

Ev::Io<void> yields(std::size_t n) {
	if (n == 0)
		return Ev::lift();
	return Ev::yield().then([n]() { return yields(n - 1); });
}

}

int main() {
	auto bus = S::Bus();
	auto mut = Boss::Mod::ChannelFeeManager(bus);

	/* One modifier, whose multiplier the test changes.  */
	auto multiplier = double(1.0);
	bus.subscribe<Boss::Msg::SolicitChannelFeeModifier
		     >([&](Boss::Msg::SolicitChannelFeeModifier const&) {
		return bus.raise(Boss::Msg::ProvideChannelFeeModifier{
			[&]( Ln::NodeId, std::uint32_t, std::uint32_t) {
				return Ev::lift(multiplier);
			}
		});
	});
	auto sets = std::vector<Boss::Msg::SetChannelFee>();
	bus.subscribe<Boss::Msg::SetChannelFee
		     >([&](Boss::Msg::SetChannelFee const& m) {
		sets.push_back(m);
		return Ev::lift();
	});

	auto recomputed = std::make_shared<bool>(true);

	auto code = Ev::lift().then([&]() {
		/* No median for the peer yet.  */
		return bus.raise(Boss::Msg::RecomputeChannelFee{A, recomputed});
	}).then([&]() {
		assert(!*recomputed);
		assert(sets.empty());

		/* A survey result; the fee is set in the background.  */
		return bus.raise(Boss::Msg::PeerMedianChannelFee{A, 1000, 100});
	}).then([&]() {
		return yields(100);
	}).then([&]() {
		assert(sets.size() == 1);
		assert(sets[0].node == A);
		assert(sets[0].base == 1000);
		assert(sets[0].proportional == 100);

		/* The modifier changes; the fee is computed again from
		 * the same median, and is set when raise() returns.  */
		multiplier = 2.5;
		return bus.raise(Boss::Msg::RecomputeChannelFee{A, recomputed});
	}).then([&]() {
		assert(*recomputed);
		assert(sets.size() == 2);
		assert(sets[1].node == A);
		assert(sets[1].base == 2500);
		assert(sets[1].proportional == 250);

		/* Without the flag.  */
		multiplier = 0.5;
		return bus.raise(Boss::Msg::RecomputeChannelFee{A, nullptr});
	}).then([&]() {
		assert(sets.size() == 3);
		assert(sets[2].base == 500);
		assert(sets[2].proportional == 50);

		/* Another peer still has no median.  */
		return bus.raise(Boss::Msg::RecomputeChannelFee{B, recomputed});
	}).then([&]() {
		assert(!*recomputed);
		assert(sets.size() == 3);

		return Ev::lift(0);
	});

	return Ev::start(std::move(code));
}
