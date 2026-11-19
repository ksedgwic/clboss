#undef NDEBUG
#include"Boss/Mod/FeeModderByPriceTheory.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/MonitorFeeByTheory.hpp"
#include"Boss/Msg/ProvideChannelFeeModifier.hpp"
#include"Boss/Msg/RecomputeChannelFee.hpp"
#include"Boss/Msg/SolicitChannelFeeModifier.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/Amount.hpp"
#include"Ln/CommandId.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include<assert.h>
#include<cstddef>
#include<cstdint>
#include<functional>
#include<math.h>
#include<set>
#include<string>
#include<vector>

/* `clboss-set-price-level nodeid level`.
 *
 * It stores the level as the peer's center, replaces the peer's
 * cards with a new round around it, puts the card of that level in
 * play first and asks for the fee to be computed again.  The cards
 * of the round before go with what they earned.  A level outside
 * -50..50, or parameters that are not a node id and a whole number,
 * are refused and change nothing.
 */

namespace {

auto const A_hex = std::string("020000000000000000000000000000000000000000000000000000000000000001");
auto const B_hex = std::string("020000000000000000000000000000000000000000000000000000000000000002");
auto const A = Ln::NodeId(A_hex);
auto const B = Ln::NodeId(B_hex);

/* enum CardPos of the module.  */
auto constexpr Deck = 0;
auto constexpr InPlay = 1;
auto constexpr Discarded = 2;

struct Card {
	int pos;
	std::int64_t price;
	std::int64_t lifetime;
	std::int64_t earnings;
};

/* The peer's cards, by price.  */
Ev::Io<std::vector<Card>> cards_of(Sqlite3::Db& db, Ln::NodeId const& node) {
	return db.transact().then([node](Sqlite3::Tx tx) {
		auto rv = std::vector<Card>();
		auto fetch = tx.query(R"QRY(
		SELECT pos, price, lifetime, earnings
		  FROM "FeeModderByPriceTheory_cards"
		 WHERE node = :node
		 ORDER BY price
		     ;
		)QRY")
			.bind(":node", std::string(node))
			.execute()
			;
		for (auto& r : fetch)
			rv.push_back(Card{ r.get<int>(0)
					 , r.get<std::int64_t>(1)
					 , r.get<std::int64_t>(2)
					 , r.get<std::int64_t>(3)
					 });
		tx.commit();
		return Ev::lift(std::move(rv));
	});
}

/* The peer's center rows: none, or the one.  */
Ev::Io<std::vector<std::int64_t>>
centers_of(Sqlite3::Db& db, Ln::NodeId const& node) {
	return db.transact().then([node](Sqlite3::Tx tx) {
		auto rv = std::vector<std::int64_t>();
		auto fetch = tx.query(R"QRY(
		SELECT price
		  FROM "FeeModderByPriceTheory_centerprice"
		 WHERE node = :node
		     ;
		)QRY")
			.bind(":node", std::string(node))
			.execute()
			;
		for (auto& r : fetch)
			rv.push_back(r.get<std::int64_t>(0));
		tx.commit();
		return Ev::lift(std::move(rv));
	});
}

/* A round as the command leaves it: five cards center-2..center+2,
 * the center's in play, the others in the deck, each with its
 * whole lifetime and no earnings.  */
bool is_new_round(std::vector<Card> const& cards, std::int64_t center) {
	if (cards.size() != 5)
		return false;
	for (auto i = std::size_t(0); i < 5; ++i) {
		auto const& c = cards[i];
		auto price = center - 2 + std::int64_t(i);
		if (c.price != price)
			return false;
		if (c.pos != (price == center ? InPlay : Deck))
			return false;
		if (c.lifetime != 288 || c.earnings != 0)
			return false;
	}
	return true;
}

bool about(double a, double b) {
	return fabs(a - b) < 0.0001;
}

}

int main() {
	auto bus = S::Bus();
	auto module_under_test = Boss::Mod::FeeModderByPriceTheory(bus);
	auto db = Sqlite3::Db(":memory:");

	auto modder = std::function< Ev::Io<double>( Ln::NodeId
						   , std::uint32_t
						   , std::uint32_t
						   )>();
	bus.subscribe<Boss::Msg::ProvideChannelFeeModifier
		     >([&](Boss::Msg::ProvideChannelFeeModifier const& m) {
		modder = m.modifier;
		return Ev::lift();
	});

	/* The answers to the command.  */
	auto req_id = std::uint64_t(0);
	auto answered = false;
	auto failed = false;
	auto answer = Jsmn::Object();
	auto fail_message = std::string();
	bus.subscribe<Boss::Msg::CommandResponse
		     >([&](Boss::Msg::CommandResponse const& m) {
		assert(m.id == Ln::CommandId::left(req_id));
		answered = true;
		answer = Jsmn::Object::parse_json(m.response.output().c_str());
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::CommandFail
		     >([&](Boss::Msg::CommandFail const& m) {
		assert(m.id == Ln::CommandId::left(req_id));
		failed = true;
		fail_message = m.message;
		return Ev::lift();
	});

	/* Stands in for the fee manager: says whether it has a median
	 * for the peer.  */
	auto has_median = true;
	auto recompute_requests = std::vector<Ln::NodeId>();
	bus.subscribe<Boss::Msg::RecomputeChannelFee
		     >([&](Boss::Msg::RecomputeChannelFee const& m) {
		recompute_requests.push_back(m.node);
		if (m.recomputed)
			*m.recomputed = has_median;
		return Ev::lift();
	});

	/* What the module last told the fee monitor.  */
	auto monitored = Boss::Msg::MonitorFeeByTheory{};
	bus.subscribe<Boss::Msg::MonitorFeeByTheory
		     >([&](Boss::Msg::MonitorFeeByTheory const& m) {
		monitored = m;
		return Ev::lift();
	});

	auto command = [&](std::string const& params) {
		++req_id;
		answered = false;
		failed = false;
		recompute_requests.clear();
		return bus.raise(Boss::Msg::CommandRequest{
			"clboss-set-price-level",
			Jsmn::Object::parse_json(params.c_str()),
			Ln::CommandId::left(req_id)
		});
	};
	/* `n` ten-minute checks with A connected.  */
	std::function<Ev::Io<void>(std::size_t)> ticks;
	ticks = [&](std::size_t n) {
		if (n == 0)
			return Ev::lift();
		return bus.raise(Boss::Msg::ListpeersAnalyzedResult{
			std::set<Ln::NodeId>{A},
			std::set<Ln::NodeId>(),
			std::set<Ln::NodeId>(),
			std::set<Ln::NodeId>(),
			false, false
		}).then([&ticks, n]() {
			return ticks(n - 1);
		});
	};
	auto earn = [&](std::uint64_t msat) {
		return bus.raise(Boss::Msg::ForwardFee{
			A, A, Ln::Amount::msat(msat), 1.0,
			Ln::Amount::msat(1000000)
		});
	};

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::DbResource{db});
	}).then([&]() {
		return bus.raise(Boss::Msg::SolicitChannelFeeModifier{});
	}).then([&]() {
		assert(modder);

		/* A round of the algorithm's own, with one card played
		 * out and a second in play, both with earnings.  */
		return modder(A, 1, 1);
	}).then([&](double) {
		return earn(5000);
	}).then([&]() {
		return ticks(288);
	}).then([&]() {
		return modder(A, 1, 1);
	}).then([&](double) {
		return earn(7000);
	}).then([&]() {
		return cards_of(db, A);
	}).then([&](std::vector<Card> cards) {
		assert(cards.size() == 5);
		auto in_play = 0, discarded = 0;
		for (auto const& c : cards) {
			if (c.pos == InPlay) {
				++in_play;
				assert(c.earnings == 7000);
			}
			if (c.pos == Discarded) {
				++discarded;
				assert(c.earnings == 5000);
			}
		}
		assert(in_play == 1 && discarded == 1);

		/* Set the level.  */
		return command("[\"" + A_hex + "\", 8]");
	}).then([&]() {
		assert(answered && !failed);
		assert(std::string(answer["nodeid"]) == A_hex);
		assert(double(answer["old_center"]) == 0.0);
		assert(double(answer["new_center"]) == 8.0);
		assert(about(double(answer["multiplier"]), pow(1.2, 8.0)));
		assert(bool(answer["fee_recomputed"]));
		assert(recompute_requests == std::vector<Ln::NodeId>{A});

		return centers_of(db, A);
	}).then([&](std::vector<std::int64_t> centers) {
		assert(centers == std::vector<std::int64_t>{8});
		return cards_of(db, A);
	}).then([&](std::vector<Card> cards) {
		/* The old cards went with their earnings.  */
		assert(is_new_round(cards, 8));

		/* The fee is computed at the level given.  */
		return modder(A, 1, 1);
	}).then([&](double mult) {
		assert(about(mult, pow(1.2, 8.0)));
		assert(monitored.node == A);
		assert(monitored.level == 8);
		assert(monitored.center == 8);
		assert(monitored.cards_left && *monitored.cards_left == 4);

		/* Outside the range: refused, nothing changes.  */
		return command("[\"" + A_hex + "\", 51]");
	}).then([&]() {
		assert(failed && !answered);
		assert(fail_message.find("51") != std::string::npos);
		assert(recompute_requests.empty());
		return command("[\"" + A_hex + "\", \"-51\"]");
	}).then([&]() {
		assert(failed && !answered);

		/* Not a node id and a whole number: refused.  */
		return command("[\"zz\", 3]");
	}).then([&]() {
		assert(failed && !answered);
		return command("[\"" + A_hex + "\", 1.5]");
	}).then([&]() {
		assert(failed && !answered);
		return command("[\"" + A_hex + "\", \"8x\"]");
	}).then([&]() {
		assert(failed && !answered);
		return command("[\"" + A_hex + "\"]");
	}).then([&]() {
		assert(failed && !answered);
		return command("{\"nodeid\": \"" + A_hex + "\"}");
	}).then([&]() {
		assert(failed && !answered);
		assert(recompute_requests.empty());

		return centers_of(db, A);
	}).then([&](std::vector<std::int64_t> centers) {
		assert(centers == std::vector<std::int64_t>{8});
		return cards_of(db, A);
	}).then([&](std::vector<Card> cards) {
		assert(is_new_round(cards, 8));

		/* By keyword, a negative level as a string (how
		 * lightning-cli passes one), at the lower limit.  */
		return command( "{\"nodeid\": \"" + A_hex + "\""
				", \"level\": \"-50\"}"
			      );
	}).then([&]() {
		assert(answered && !failed);
		assert(double(answer["old_center"]) == 8.0);
		assert(double(answer["new_center"]) == -50.0);
		assert(about(double(answer["multiplier"]), pow(0.8, 50.0)));
		return cards_of(db, A);
	}).then([&](std::vector<Card> cards) {
		assert(is_new_round(cards, -50));

		/* Called again, with earnings on the card in play:
		 * one center row and five cards still.  */
		return earn(9000);
	}).then([&]() {
		return command("[\"" + A_hex + "\", 50]");
	}).then([&]() {
		assert(answered && !failed);
		return command("[\"" + A_hex + "\", 3]");
	}).then([&]() {
		assert(answered && !failed);
		assert(double(answer["old_center"]) == 50.0);
		return centers_of(db, A);
	}).then([&](std::vector<std::int64_t> centers) {
		assert(centers == std::vector<std::int64_t>{3});
		return cards_of(db, A);
	}).then([&](std::vector<Card> cards) {
		assert(is_new_round(cards, 3));

		/* The rounds go on from the level set: the card in
		 * play earns, the five cards are played out, and the
		 * next round is around the card that earned.  */
		return earn(100);
	}).then([&]() {
		return ticks(288);
	}).then([&]() {
		return cards_of(db, A);
	}).then([&](std::vector<Card> cards) {
		assert(cards.size() == 5);
		assert(cards[2].price == 3);
		assert(cards[2].pos == Discarded);
		assert(cards[2].earnings == 100);

		return modder(A, 1, 1);
	}).then([&](double) {
		return ticks(288);
	}).then([&]() {
		return modder(A, 1, 1);
	}).then([&](double) {
		return ticks(288);
	}).then([&]() {
		return modder(A, 1, 1);
	}).then([&](double) {
		return ticks(288);
	}).then([&]() {
		return modder(A, 1, 1);
	}).then([&](double) {
		return ticks(288);
	}).then([&]() {
		/* Deck empty: this draws from a new round.  */
		return modder(A, 1, 1);
	}).then([&](double) {
		assert(monitored.center == 3);
		assert(monitored.cards_left && *monitored.cards_left == 4);

		/* A node never seen, and no median for it: its center
		 * and round are set, and the answer says no fee was
		 * computed.  */
		has_median = false;
		return command("[\"" + B_hex + "\", 0]");
	}).then([&]() {
		assert(answered && !failed);
		assert(double(answer["old_center"]) == 0.0);
		assert(double(answer["new_center"]) == 0.0);
		assert(about(double(answer["multiplier"]), 1.0));
		assert(!bool(answer["fee_recomputed"]));
		assert(recompute_requests == std::vector<Ln::NodeId>{B});
		return centers_of(db, B);
	}).then([&](std::vector<std::int64_t> centers) {
		assert(centers == std::vector<std::int64_t>{0});
		return cards_of(db, B);
	}).then([&](std::vector<Card> cards) {
		assert(is_new_round(cards, 0));

		return Ev::lift(0);
	});

	return Ev::start(std::move(code));
}
