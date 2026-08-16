#include"Boss/ModG/ReqResp.hpp"
#include"Boss/ModG/Swapper.hpp"
#include"Boss/Msg/Block.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RequestGetAutoSwapFlag.hpp"
#include"Boss/Msg/RequestGetOnchainIgnoreFlag.hpp"
#include"Boss/Msg/ResponseGetAutoSwapFlag.hpp"
#include"Boss/Msg/ResponseGetOnchainIgnoreFlag.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/SwapRequest.hpp"
#include"Boss/Msg/SwapResponse.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include"Uuid.hpp"
#include<cstdint>
#include<sstream>

namespace {

/* Number of blocks to ignore `trigger_swap` after a swap completes
 * (success or failure does not matter).
 */
auto const ignored_blocks = std::uint32_t(6);
/* Number of blocks after creating a new swap, after which, if the
 * swap still has not completed, we ignore the swap and return to
 * our default state that we can swap now.
 */
auto const swap_timeout = std::uint32_t(432);
/* Minimum swapping amount.  */
auto const min_amount = Ln::Amount::btc(0.001);

}

namespace Boss { namespace ModG {

class Swapper::Impl {
private:
	S::Bus& bus;
	std::string name;
	std::string statuskey;

	ReqResp< Msg::RequestGetOnchainIgnoreFlag
	       , Msg::ResponseGetOnchainIgnoreFlag
	       > get_ignore_rr;

        ModG::ReqResp< Msg::RequestGetAutoSwapFlag
                     , Msg::ResponseGetAutoSwapFlag
                     > get_auto_swap_rr;

	std::string why;

	Sqlite3::Db db;
	std::unique_ptr<std::uint32_t> blockheight;

public:
	Impl( S::Bus& bus_
	    , char const* name_
	    , char const* statuskey_
	    ) : bus(bus_), name(name_), statuskey(statuskey_)
	      , get_ignore_rr(bus_)
	      , get_auto_swap_rr(bus_)
	      { start(); }

private:
	void start() {
		why = "Newly-started";

		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& r) {
			db = r.db;
			return db.transact().then([this](Sqlite3::Tx tx) {
				dbinit(tx);
				tx.commit();
				return Ev::lift();
			});
		});

		bus.subscribe<Msg::Block
			     >([this](Msg::Block const& b) {
			if (!blockheight)
				blockheight
					= Util::make_unique<std::uint32_t>();
			*blockheight = b.height;
			if (!db)
				return Ev::lift();
			return on_block();
		});

		bus.subscribe<Msg::SwapResponse
			     >([this](Msg::SwapResponse const& r) {
			return on_swap_response(r);
		});

		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const&) {
			return on_solicit_status();
		});
	}
	void dbinit(Sqlite3::Tx& tx) {
		tx.query_execute(R"QRY(
		CREATE TABLE IF NOT EXISTS "ModG::Swapper"
		     ( name TEXT PRIMARY KEY
		     , swapUuid TEXT NOT NULL
		     -- blockheights
		     -- do not perform swaps until after this
		     -- blockheight.
		     , blocked_until INTEGER NOT NULL
		     -- if we reach this blockheight and
		     -- swapUuid is non-empty, forget the
		     -- current swapUuid.
		     , forget_after INTEGER NOT NULL
		     );
		)QRY");
		tx.query(R"QRY(
		INSERT OR IGNORE
		  INTO "ModG::Swapper"
		VALUES( :name
		      , ''
		      , 0
		      , 5000000
		      )
		)QRY")
			.bind(":name", name)
			.execute();
	}

	Ev::Io<void> on_swap_response(Msg::SwapResponse const& r) {
		/* Borrow the transaction, do not acquire it.  */
		auto& tx = *r.dbtx;
		/* If db is not valid yet, we might not have initialized
		 * the table yet.
		 * So, make sure we have a valid table in that case.
		 */
		if (!db)
			dbinit(tx);

		/* Get current time if we match the swapUuid.  */
		auto fetch = tx.query(R"QRY(
		SELECT blocked_until
		  FROM "ModG::Swapper"
		 WHERE name = :name
		   AND swapUuid = :swapUuid
		     ;
		)QRY")
			.bind(":name", name)
			.bind(":swapUuid", std::string(r.id))
			.execute();
		auto found = false;
		auto blocked_until = std::uint32_t();
		for (auto& r : fetch) {
			found = true;
			blocked_until = r.get<std::uint32_t>(0);
		}

		/* Not us.  */
		if (!found)
			return Ev::lift();

		/* Bump up the `blocked_until`.  */
		if (blockheight) {
			auto proposed = *blockheight + ignored_blocks;
			if (proposed > blocked_until)
				blocked_until = proposed;
		} else
			blocked_until += ignored_blocks;

		tx.query(R"QRY(
		UPDATE "ModG::Swapper"
		   SET swapUuid = ''
		     , blocked_until = :blocked_until
		     , forget_after = 5000000
		 WHERE name = :name
		     ;
		)QRY")
			.bind(":blocked_until", blocked_until)
			.bind(":name", name)
			.execute();

		if (r.success) {
			auto os = std::ostringstream();
			os << "Completed with " << r.onchain_amount
			   << "swapped onchain"
			    ;
			why = os.str();
			return Boss::log( bus, Info
					, "%s: Done, got %s onchain."
					, name.c_str()
					, std::string(r.onchain_amount).c_str()
					);
		} else {
			why = "Swap failed";
			return Boss::log( bus, Info
					, "%s: Swap failed."
					, name.c_str()
					);
		}
	}

	Ev::Io<void> on_block() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			/* Clear the data if we have reached the
			 * `forget_after` timeout.  */
			tx.query(R"QRY(
			UPDATE "ModG::Swapper"
			   SET swapUuid = ''
			     , forget_after = 5000000
			 WHERE name = :name
			   AND forget_after < :current_time
			     ;
			)QRY")
				.bind(":name", name)
				.bind(":current_time", *blockheight)
				.execute();
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> on_solicit_status() {
		if (!db || !blockheight)
			return Ev::lift();
		auto ignore = std::make_shared<bool>();
		return Ev::lift().then([this]() {

			return get_ignore_rr.execute(
				Msg::RequestGetOnchainIgnoreFlag{}
			);
		}).then([this, ignore](Msg::ResponseGetOnchainIgnoreFlag res) {
			*ignore = res.ignore;

			return db.transact();
		}).then([this, ignore](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
			SELECT swapUuid, blocked_until
			  FROM "ModG::Swapper"
			 WHERE name = :name
			     ;
			)QRY")
				.bind(":name", name)
				.execute();
			auto swapUuid = std::string();
			auto blocked_until = std::uint32_t();
			for (auto& r : fetch) {
				swapUuid = r.get<std::string>(0);
				blocked_until = r.get<std::uint32_t>(1);
			}

			auto status = std::string();
			if (swapUuid != "")
				status = "swapping";
			else if (*ignore)
				status = "ignoring";
			else if (blocked_until >= *blockheight)
				status = "waiting";
			else
				status = "idle";
			auto out = Json::Out()
				.start_object()
					.field("status", status)
					.field("message", why)
				.end_object()
				;
			return bus.raise(Msg::ProvideStatus{
				statuskey, std::move(out)
			});
		});
	}

public:
	Ev::Io<void>
	trigger_swap(std::function<bool(Ln::Amount&, std::string&)> n_ok) {
		if (!db || !blockheight)
			return Ev::lift();
		auto ok = std::make_shared<std::function<bool(Ln::Amount&, std::string&)>>(
			std::move(n_ok)
		);
		return get_ignore_rr.execute(Msg::RequestGetOnchainIgnoreFlag{
			nullptr
		}).then([this, ok](Msg::ResponseGetOnchainIgnoreFlag res) {
			if (res.ignore)
				return Boss::log( bus, Info
						, "%s: Ignoring onchain funds "
						  "until %f seconds from now."
						, name.c_str()
						, res.seconds
						);
			return Ev::lift().then([this]() {
				return get_auto_swap_rr.execute(Msg::RequestGetAutoSwapFlag{
					nullptr
				});
			}).then([this, ok](
					Msg::ResponseGetAutoSwapFlag res) {
				if (!res.state)
					return Boss::log( bus, Info
						, "%s: %s. Will not send "
						  "funds."
						, name.c_str()
						, res.comment.c_str());
				return continue_swap(ok);
			});
		});
	}
private:
	Ev::Io<void>
	continue_swap(std::shared_ptr<std::function<bool( Ln::Amount&
							, std::string&
							)>> const& ok) {
		return db.transact().then([this, ok](Sqlite3::Tx tx) {
			/* Do we currently have a swap, or is the time
			 * done?
			 */
			auto check = tx.query(R"QRY(
			SELECT blocked_until
			  FROM "ModG::Swapper"
			 WHERE name = :name
			   AND swapUuid = ''
			   AND blocked_until < :current_time
			     ;
			)QRY")
				.bind(":name", name)
				.bind(":current_time", *blockheight)
				.execute();
			auto found = false;
			for (auto& r : check) {
				(void) r;
				found = true;
			}

			/* Something is blocking us.  */
			if (!found)
				return Ev::lift();

			auto blocked_until = *blockheight + ignored_blocks;

			/* Check if the ok function is, well, ok.  */
			auto amt = Ln::Amount::sat(0);
			if (!(*ok)(amt, why) || amt == Ln::Amount::sat(0)) {
				/* Failed, delay.  */
				tx.query(R"QRY(
				UPDATE "ModG::Swapper"
				   SET blocked_until = :blocked_until
				 WHERE name = :name
				     ;
				)QRY")
					.bind(":blocked_until", blocked_until)
					.bind(":name", name)
					.execute();

				tx.commit();
				return Boss::log( bus, Info
						, "%s: %s: "
						  "Will wait %u blocks."
						, name.c_str()
						, why.c_str()
						, (unsigned int)
						  ignored_blocks
						);
			}

			if (amt < min_amount)
				amt = min_amount;

			auto swapUuid = Uuid::random();
			auto forget_after = *blockheight + swap_timeout;
			/* Update our database.  */
			tx.query(R"QRY(
			UPDATE "ModG::Swapper"
			   SET swapUuid = :swapUuid
			     , blocked_until = :blocked_until
			     , forget_after = :forget_after
			 WHERE name = :name
			     ;
			)QRY")
				.bind(":swapUuid", std::string(swapUuid))
				.bind(":blocked_until", blocked_until)
				.bind(":forget_after", forget_after)
				.bind(":name", name)
				.execute();

			/* Create the message.  */
			auto msg = std::make_shared<Msg::SwapRequest>();
			msg->dbtx = std::make_shared<Sqlite3::Tx>(
				std::move(tx)
			);
			msg->id = swapUuid;
			msg->min_offchain_amount = min_amount;
			msg->max_offchain_amount = amt;

			/* Print log, then send message.  */
			return Boss::log( bus, Info
					, "%s: %s: "
					  "Swapping %s onchain (id: %s)."
					, name.c_str()
					, why.c_str()
					, std::string(amt).c_str()
					, std::string(swapUuid).c_str()
					).then([this, msg]() {
				return bus.raise(std::move(*msg));
			});
		});
	}
};

Swapper::~Swapper() =default;
Swapper::Swapper( S::Bus& bus_
		, char const* name
		, char const* statuskey
		) : bus(bus_)
		  , pimpl(Util::make_unique<Impl>(bus_, name, statuskey))
		  { }

Ev::Io<void>
Swapper::trigger_swap(std::function<bool(Ln::Amount&, std::string&)> ok) {
	return pimpl->trigger_swap(std::move(ok));
}
}}
