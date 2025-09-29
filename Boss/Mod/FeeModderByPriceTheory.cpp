#include"Boss/Mod/FeeModderByPriceTheory.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/ProvideChannelFeeModifier.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/SolicitChannelFeeModifier.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/coroutine.hpp"
#include"Ev/yield.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include<algorithm>
#include<cstdint>
#include<inttypes.h>
#include<map>
#include<math.h>
#include<sstream>
#include<vector>

/*

Here is a sketch for an algorithm.

First, let us define a `std::int64_t`, called `price`, for each channel.
Let us consider that it generates a multiplier: if `price` is negative, the multiplier is 0.8 ^ -`price`, if it is positive the multiplier is 1.2 ^ `price`.

Initially, each channel starts with a `price` of 0.
The algorithm operates on a per-channel (per-peer?) basis.

At the start, the algorithm shuffles a small deck of cards with `price - 2`, `price - 1`, `price`, `price + 1`, and `price + 2` and initializes the deck with those cards.

It draws the topmost card in the deck, then sets the multiplier by the `price` marked on the card.
While the current card is in play, it records every forwarding fee it successfully receives on the card.
Once it has some number of days (2-3 days?) of data, it discards the card and draws a new one.

If the deck is empty when the algorithm wants to draw a card, it looks at the discard pile and selects the card with the highest total earned forwarding fee.
It then sets the `price` to that card, then destroys the discard pile and creates and shuffles a new deck of cards using the new `price`.

* The above algorithm naturally walks around some current `price` level that is assumed to be optimal, but seeks out nearby alternatives that may be better.
* We assume that there are no local optima; price theory suggests that local optima should not exist, rather, there is some single globally-optimum price that delivers the most amount of goods at the most economic cost.
  * Thus, we do not need to perform random large steps in order to escape local optima.
* The algorithm "forgets" old data so it can adapt to changes in LN connectivity and changes in economic conditions.
* Shuffling the deck is necessary to prevent locking onto some regular periodic phenomenon on the network.
  The shuffling ensures any regular periodic phenomenon that matches our sampling period (or multiple thereof) will randomly select one of he possibilities uniformly at random.

*/

namespace {

/* Initial price for nodes that have not been played yet.
 * Later we can check what is typical price that nodes
 * settle in, and change this initial price so future
 * instances of CLBOSS can achieve optimum price more
 * quickly.
 */
auto constexpr initial_price = std::int64_t(0);

/* Number of 10-minute events that each card is put
 * in play.
 * This is put on the cards when initially created
 * for shuffling in the deck, so this can be changed
 * and what is in the database will be followed, the
 * change will only be put in play when a game is
 * finished and new cards are created for a new game.
 */
auto constexpr initial_lifetime = std::uint64_t(288);

/* Maximum step around the center that we will
 * try.
 * The larger, the longer games last before we move
 * the center.
 */
auto constexpr max_step = std::size_t(2);

/* The hardcoded numbers below cannot be changed.  */
double price_to_multiplier(std::int64_t price) {
	if (price < 0)
		return pow(0.8, double(-price));
	else if (price > 0)
		return pow(1.2, double(price));
	else
		return 1.0;
}

}

namespace Boss { namespace Mod {

class FeeModderByPriceTheory::Impl {
private:
	S::Bus& bus;

	Sqlite3::Db db;

	/* /!\ Do NOT change the numbers, this is used in the database.  */
	enum CardPos
	{ CardPos_Deck = 0		// Card is currently shuffled in deck
	, CardPos_InPlay = 1		// Card is currently in play
	, CardPos_Discarded = 2		// Card is in discard pile
	};

	static
	std::string card_pos_string(CardPos pos) {
		switch (pos) {
		case CardPos_Deck:	return "Deck";
		case CardPos_InPlay:	return "InPlay";
		case CardPos_Discarded:	return "Discarded";
		}
		return "invalid";
	}

	void start() {
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& m) {
			db = m.db;
			return initialize();
		});

		bus.subscribe<Msg::SolicitChannelFeeModifier
			     >([ this
			       ](Msg::SolicitChannelFeeModifier const& _) {
			auto fun = [this]( Ln::NodeId id
					 , std::uint32_t b
					 , std::uint32_t p
					 ) {
				return get_fee_mod(id);
			};
			return bus.raise(Msg::ProvideChannelFeeModifier{
				std::move(fun)
			});
		});

		bus.subscribe<Msg::ListpeersAnalyzedResult
			     >([this](Msg::ListpeersAnalyzedResult const& m) {
			/* Ignore init.  */
			if (m.initial)
				return Ev::lift();
			/* This occurs every 10 minutes.  Nodes we have channels
			 * with, that are connected, are presumed to be potentially
			 * earning, so should reduce lifetime.
			 */
			return update_10_minutes(m.connected_channeled);
		});
		bus.subscribe<Msg::ForwardFee
			     >([this](Msg::ForwardFee const& m) {
			return forward_fee(m.out_id, m.fee);
		});

		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const& _) {
			return solicit_status();
		});
	}

	Ev::Io<void> initialize() {
		return db.transact().then([](Sqlite3::Tx tx) {
			tx.query_execute(R"QRY(
			-- Contains the current center price of
			-- nodes.
			CREATE TABLE IF NOT EXISTS
			       "FeeModderByPriceTheory_centerprice"
			     ( node TEXT PRIMARY KEY
			     , price INTEGER NOT NULL
			     );

			-- Contains cards.
			CREATE TABLE IF NOT EXISTS
			       "FeeModderByPriceTheory_cards"

			     -- arbitrary unique card id
			     ( id INTEGER PRIMARY KEY

			     -- which node this card is for.
			     , node TEXT NOT NULL

			     -- enum CardPos
			     , pos INTEGER NOT NULL

			     -- random integer that dictates the shuffle
			     -- order while in-deck.
			     , deckorder INTEGER NOT NULL

			     -- the price marked on this card.
			     , price INTEGER NOT NULL

			     -- remaining number of 10-minute events
			     -- allowed before we have to discard this
			     -- card if it is currently in play.
			     , lifetime INTEGER NOT NULL

			     -- total amount of fees earned while this
			     -- card was in play.
			     , earnings INTEGER NOT NULL
			     );

			CREATE INDEX IF NOT EXISTS
			       "FeeModderByPriceTheory_cardsidx"
			    ON "FeeModderByPriceTheory_cards"
			       ( node, pos, deckorder
			       );
			)QRY");

			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<Sqlite3::Tx> db_transact() {
		while (!db) {
			co_await Ev::yield();
		}
		co_return (co_await db.transact());
	}

	Ev::Io<double> get_fee_mod(Ln::NodeId const& node_) {
		/* Create our own copy of node, as it might
		not survive across awaits.
		*/
		auto node = node_;
		auto mult = double();
		auto tx = co_await db_transact();
		auto os = std::ostringstream();

		auto price = get_price(tx, node, os);
		tx.commit();

		mult = price_to_multiplier(price);

		co_await Boss::log( bus, Debug
				  , "FeeModderByPriceTheory: %s: "
				    "level = %" PRIi64 ", mult = %f"
				    "%s"
				  , std::string(node).c_str()
				  , price
				  , mult
				  , os.str().c_str()
				  );
		co_return mult;
	}

	std::int64_t get_price( Sqlite3::Tx& tx, Ln::NodeId const& node
			      , std::ostringstream& msg
			      ) {
		auto fetch = tx.query(R"QRY(
		SELECT price FROM "FeeModderByPriceTheory_cards"
		 WHERE node = :node
		   AND pos = :CardPos_InPlay
		     ;
		)QRY")
			.bind(":node", std::string(node))
			.bind(":CardPos_InPlay", int(CardPos_InPlay))
			.execute()
			;
		for (auto& r : fetch)
			return r.get<std::int64_t>(0);

		/* If we reached here, there is no in-play card, so
		 * draw a card.
		 */
		msg << "; ";
		draw_a_card(tx, node, msg);
		return get_price(tx, node, msg);
	}
	void draw_a_card( Sqlite3::Tx& tx, Ln::NodeId const& node
			, std::ostringstream& msg
			) {
		auto drew_a_card = false;
		auto id = std::uint64_t();
		/* Fetch a card from the deck.  */
		auto fetch = tx.query(R"QRY(
		SELECT id FROM "FeeModderByPriceTheory_cards"
		 WHERE node = :node
		   AND pos = :CardPos_Deck
		 ORDER BY deckorder DESC
		     ;
		)QRY")
			.bind(":node", std::string(node))
			.bind(":CardPos_Deck", int(CardPos_Deck))
			.execute()
			;
		for (auto& r :fetch) {
			drew_a_card = true;
			id = r.get<std::uint64_t>(0);
			break;
		}

		if (drew_a_card) {
			/* Add message.  */
			msg << "set to new level for this measurement round";
			/* Set the card to its new position as in-play.  */
			tx.query(R"QRY(
			UPDATE "FeeModderByPriceTheory_cards"
			   SET pos = :CardPos_InPlay
			 WHERE id = :id
			)QRY")
				.bind(":CardPos_InPlay", int(CardPos_InPlay))
				.bind(":id", id)
				.execute()
				;
			return;
		} else {
			/* Out of cards!  End game round (which creates
			 * new cards) and draw a card again.  */
			end_game_round(tx, node, msg);
			draw_a_card(tx, node, msg);
		}
	}
	void end_game_round( Sqlite3::Tx& tx, Ln::NodeId const& node
			   , std::ostringstream& msg
			   ) {
		update_centerprice(tx, node, msg);
		reshuffle_deck(tx, node);
	}
	void update_centerprice( Sqlite3::Tx& tx, Ln::NodeId const& node
			       , std::ostringstream& msg
			       ) {
		/* Default to initial price, if we cannot find any discarded
		 * cards (meaning we have not actually created a deck.
		 */
		auto new_price = initial_price;
		/* Look for best (i.e. highest-earning) discarded card.  */
		auto fetch = tx.query(R"QRY(
		SELECT price, earnings FROM "FeeModderByPriceTheory_cards"
		 WHERE node = :node
		   AND pos = :CardPos_Discarded
		 ORDER BY earnings DESC
		 LIMIT 1
		     ;
		)QRY")
			.bind(":node", std::string(node))
			.bind(":CardPos_Discarded", int(CardPos_Discarded))
			.execute()
			;
		for (auto& r : fetch) {
			/* If the best earner earned nothing, do not
			 * change the center price.
			 * We will not enter this code if we have not
			 * ever played the game, so this is fine.
			 */
			if (r.get<std::int64_t>(1) == 0) {
				msg << "earned nothing in previous "
				    << "measurement round; "
				    ;
				return;
			}

			new_price = r.get<std::int64_t>(0);
		}

		/* Now update center price.  */
		msg << "measuring around new level "
		    << new_price << " for new mesurement round; "
		    ;
		tx.query(R"QRY(
		INSERT OR REPLACE INTO "FeeModderByPriceTheory_centerprice"
		VALUES( :node
		      , :new_price
		      );
		)QRY")
			.bind(":node", std::string(node))
			.bind(":new_price", new_price)
			.execute()
			;
	}
	void reshuffle_deck(Sqlite3::Tx& tx, Ln::NodeId const& node) {
		/* Destroy all cards of the node.  */
		tx.query(R"QRY(
		DELETE FROM "FeeModderByPriceTheory_cards"
		 WHERE node = :node
		     ;
		)QRY")
			.bind(":node", std::string(node))
			.execute()
			;
		/* Get center price.  */
		auto centerprice = initial_price;
		auto fetch_centerprice = tx.query(R"QRY(
		SELECT price FROM "FeeModderByPriceTheory_centerprice"
		 WHERE node = :node
		     ;
		)QRY")
			.bind(":node", std::string(node))
			.execute()
			;
		for (auto& r : fetch_centerprice)
			centerprice = r.get<std::int64_t>(0);

		/* Generate the cards.  */
		auto prices = std::vector<std::int64_t>();
		prices.push_back(centerprice);
		for (auto i = std::size_t(0); i < max_step; ++i) {
			prices.push_back(centerprice + std::int64_t(i + 1));
			prices.push_back(centerprice - std::int64_t(i + 1));
		}
		/* Now shuffle it.  */
		std::shuffle(prices.begin(), prices.end(), Boss::random_engine);

		/* Load cards into table.  */
		for (auto i = std::size_t(0); i < prices.size(); ++i) {
			auto price = prices[i];
			tx.query(R"QRY(
			INSERT INTO "FeeModderByPriceTheory_cards"
			VALUES( NULL -- id, autogenerated
			      , :node
			      , :CardPos_Deck
			      , :i
			      , :price
			      , :initial_lifetime
			      , 0
			      );
			)QRY")
				.bind(":node", std::string(node))
				.bind(":CardPos_Deck", int(CardPos_Deck))
				.bind(":i", i)
				.bind(":price", price)
				.bind(":initial_lifetime", initial_lifetime)
				.execute()
				;
		}
	}

	Ev::Io<void> update_10_minutes(std::set<Ln::NodeId> const& connected) {
		return db_transact().then([connected](Sqlite3::Tx tx) {
			/* Decrement cards in play of nodes that are
			 * connected.  */
			for (auto const& node : connected)
				tx.query(R"QRY(
				UPDATE "FeeModderByPriceTheory_cards"
				   SET lifetime = lifetime - 1
				 WHERE pos = :CardPos_InPlay
				   AND node = :node
				     ;
				)QRY")
					.bind(":CardPos_InPlay"
					     , int(CardPos_InPlay)
					     )
					.bind(":node", std::string(node))
					.execute()
					;
			/* Discard in-play cards whose lifetime has dropped
			 * to 0.  */
			tx.query(R"QRY(
			UPDATE "FeeModderByPriceTheory_cards"
			   SET pos = :CardPos_Discarded
			 WHERE pos = :CardPos_InPlay
			   AND lifetime <= 0
			     ;
			)QRY")
				.bind(":CardPos_InPlay", int(CardPos_InPlay))
				.bind(":CardPos_Discarded", int(CardPos_Discarded))
				.execute()
				;
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> forward_fee( Ln::NodeId const& node
				, Ln::Amount fee
				) {
		return db_transact().then([node, fee](Sqlite3::Tx tx) {
			/* Add fee to in-play card.  */
			tx.query(R"QRY(
			UPDATE "FeeModderByPriceTheory_cards"
			   SET earnings = earnings + :fee
			 WHERE node = :node
			   AND pos = :CardPos_InPlay
			     ;
			)QRY")
				.bind(":fee", fee.to_msat())
				.bind(":node", std::string(node))
				.bind(":CardPos_InPlay", int(CardPos_InPlay))
				.execute()
				;

			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> solicit_status() {
		struct Card {
			CardPos pos;
			std::int64_t price;
			std::uint64_t lifetime;
			Ln::Amount earnings;
		};

		return db.transact().then([this](Sqlite3::Tx tx) {
			/* Gather cards.  */
			auto cards = std::map<Ln::NodeId, std::vector<Card>>();
			auto fetch = tx.query(R"QRY(
			SELECT node, pos, price, lifetime, earnings
			  FROM "FeeModderByPriceTheory_cards"
			     ;
			)QRY")
				.execute()
				;
			for (auto& r : fetch) {
				auto node = Ln::NodeId(r.get<std::string>(0));
				auto card = Card();
				card.pos = CardPos(r.get<int>(1));
				card.price = r.get<std::int64_t>(2);
				card.lifetime = r.get<std::uint64_t>(3);
				card.earnings = Ln::Amount::msat(
					r.get<std::uint64_t>(4)
				);
				cards[node].push_back(card);
			}
			tx.commit();

			/* JSONize them.  */
			auto out = Json::Out();
			auto obj = out.start_object();
			for (auto const& node_cards : cards) {
				auto node = std::string(node_cards.first);
				auto const& cards = node_cards.second;

				auto msg = std::ostringstream();
				auto first = true;
				for (auto const& card : cards) {
					if (first)
						first = false;
					else
						msg << "; ";
					switch (card.pos) {
					case CardPos_Deck:
						msg << "(" << card.price << ")";
						break;
					case CardPos_InPlay:
						msg << "<"
						    << card.price
						    << ": "
						    << card.earnings
						    << ", "
						    << card.lifetime
						    << "blocks"
						    << ">"
						     ;
						break;
					case CardPos_Discarded:
						msg << card.price
						    << ": "
						    << card.earnings
						    ;
						break;
					}
				}

				obj.field(node, msg.str());
			}
			obj.end_object();

			return bus.raise(Msg::ProvideStatus{
				"price_theory_investigating",
				std::move(out)
			});
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;

	explicit
	Impl( S::Bus& bus_
	    ) : bus(bus_) { start(); }
};

FeeModderByPriceTheory::FeeModderByPriceTheory(FeeModderByPriceTheory&&)
	=default;
FeeModderByPriceTheory::~FeeModderByPriceTheory() =default;

FeeModderByPriceTheory::FeeModderByPriceTheory(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
