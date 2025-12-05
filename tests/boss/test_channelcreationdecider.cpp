#undef NDEBUG
#include"Boss/Mod/ChannelCreationDecider.hpp"
#include"Boss/Msg/AmountSettings.hpp"
#include"Boss/Msg/ChannelFunds.hpp"
#include"Boss/Msg/NeedsOnchainFunds.hpp"
#include"Boss/Msg/OnchainFee.hpp"
#include"Boss/Msg/OnchainFunds.hpp"
#include"Boss/Msg/RequestChannelCreation.hpp"
#include"Boss/Msg/RequestGetAutoOpenFlag.hpp"
#include"Boss/Msg/ResponseGetAutoOpenFlag.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Ln/Amount.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>

int main() {
	using Boss::Msg::ChannelFunds;
	using Boss::Msg::OnchainFee;
	using Boss::Msg::OnchainFunds;

	auto bus = S::Bus();
	Boss::Mod::ChannelCreationDecider decider(bus);

	auto create_amount = std::unique_ptr<Ln::Amount>();
	bus.subscribe< Boss::Msg::RequestChannelCreation
		     >([&create_amount
		       ](Boss::Msg::RequestChannelCreation const& rcc) {
		create_amount = Util::make_unique<Ln::Amount>(rcc.amount);
		return Ev::lift();
	});
	auto needs_funds = std::unique_ptr<Ln::Amount>();
	bus.subscribe< Boss::Msg::NeedsOnchainFunds
		     >([&needs_funds
		       ](Boss::Msg::NeedsOnchainFunds const& nof) {
		needs_funds = Util::make_unique<Ln::Amount>(nof.needed);
		return Ev::lift();
	});
	bus.subscribe< Boss::Msg::RequestGetAutoOpenFlag
		     >([&bus](Boss::Msg::RequestGetAutoOpenFlag const& m) {
			auto msg = Boss::Msg::ResponseGetAutoOpenFlag();
			msg.requester = m.requester;
			msg.state = true;
			msg.comment = "test";
			return bus.raise(msg);
	});

	auto yieldx2 = [](Ev::Io<void> act) {
		return std::move(act).then([]() {
			return Ev::yield();
		}).then([]() {
			return Ev::yield();
		});
	};
	auto onchain_funds = [&](Ln::Amount v) {
		return yieldx2(bus.raise(OnchainFunds{v}));
	};
	auto onchain_fee = [&](bool low_fees) {
		return yieldx2(bus.raise(OnchainFee{low_fees}));
	};
	auto channel_funds = [&](Ln::Amount v) {
		return yieldx2(bus.raise(ChannelFunds{v}));
	};

	auto const LOW_FEE = true;
	auto const HIGH_FEE = false;

	auto code = Ev::lift().then([&]() {

		/* Set up first.  */
		auto msg = Boss::Msg::AmountSettings();
		msg.reserve = Ln::Amount::sat(30000);
		msg.min_amount = Ln::Amount::btc(0.010);
		msg.min_remaining = Ln::Amount::btc(0.0105);
		return bus.raise(msg);
	}).then([&]() {

		return onchain_funds(Ln::Amount::btc(1.0));;
	}).then([&]() {
		/* Should not trigger yet, not enough info.  */
		assert(!create_amount);
		return onchain_fee(LOW_FEE);
	}).then([&]() {
		assert(!create_amount);
		return channel_funds(Ln::Amount::btc(1.0));
	}).then([&]() {
		/* Should trigger now since all info is good.  */
		assert(create_amount);
		/* Onchain reserve should have been deducted.  */
		assert(*create_amount < Ln::Amount::btc(1.0));

		/* Should not trigger on fee or channel-funds messages.  */
		create_amount = nullptr;
		return onchain_fee(HIGH_FEE);
	}).then([&]() {
		assert(!create_amount);
		return channel_funds(Ln::Amount::btc(0.1));
	}).then([&]() {
		assert(!create_amount);
		return onchain_fee(LOW_FEE);
	}).then([&]() {
		assert(!create_amount);
		return channel_funds(Ln::Amount::btc(1.0));
	}).then([&]() {
		assert(!create_amount);

		/* Each onchain-funds message should trigger.  */
		create_amount = nullptr;
		return onchain_funds(Ln::Amount::btc(1.0));;
	}).then([&]() {
		assert(create_amount);
		create_amount = nullptr;
		return onchain_funds(Ln::Amount::btc(1.0));;
	}).then([&]() {
		assert(create_amount);

		/* High fees and low onchain-funds amount compared
		 * to channel funds should prevent creation.  */
		create_amount = nullptr;
		return onchain_fee(HIGH_FEE);
	}).then([&]() {
		return channel_funds(Ln::Amount::btc(999.0));
	}).then([&]() {
		return onchain_funds(Ln::Amount::btc(0.1));;
	}).then([&]() {
		assert(!create_amount);

		/* High fees but with most funds onchain should
		 * still create.  */
		create_amount = nullptr;
		return onchain_fee(HIGH_FEE);
	}).then([&]() {
		return channel_funds(Ln::Amount::btc(0.01));
	}).then([&]() {
		return onchain_funds(Ln::Amount::btc(10.0));;
	}).then([&]() {
		assert(create_amount);

		/* Tiny onchain funds should be ignored.  */
		create_amount = nullptr;
		return onchain_fee(LOW_FEE);
	}).then([&]() {
		return channel_funds(Ln::Amount::btc(0.0));
	}).then([&]() {
		return onchain_funds(Ln::Amount::sat(547));;
	}).then([&]() {
		assert(!create_amount);

		/* If we have a good amount of channel funds and
		 * a comparatively small amount of onchain funds,
		 * do not create.
		 * Also, do not ask for more onchain funds as we
		 * that can be very difficult to arrange.
		 */
		create_amount = nullptr;
		needs_funds = nullptr;
		return onchain_fee(LOW_FEE);
	}).then([&]() {
		return channel_funds(Ln::Amount::btc(20.0));
	}).then([&]() {
		return onchain_funds(Ln::Amount::btc(0.02));
	}).then([&]() {
		assert(!create_amount);
		assert(!needs_funds);

		/* Like the above, but the onchain funds are
		 * substantial enough that they would still
		 * make good channels anyway.  */
		create_amount = nullptr;
		needs_funds = nullptr;
		return onchain_fee(LOW_FEE);
	}).then([&]() {
		return channel_funds(Ln::Amount::btc(200.0));
	}).then([&]() {
		return onchain_funds(Ln::Amount::btc(0.2));
	}).then([&]() {
		assert(create_amount);
		assert(!needs_funds);

		return Ev::lift(0);
	});

	return Ev::start(code);
}
