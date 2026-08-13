#include"Boss/Mod/SwapManager.hpp"
#include"Boss/Msg/AcceptSwapQuotation.hpp"
#include"Boss/Msg/Block.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ListfundsResult.hpp"
#include"Boss/Msg/PayInvoice.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/ProvideSwapQuotation.hpp"
#include"Boss/Msg/RequestListpays.hpp"
#include"Boss/Msg/RequestNewaddr.hpp"
#include"Boss/Msg/ResponseListpays.hpp"
#include"Boss/Msg/ResponseNewaddr.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/SolicitSwapQuotation.hpp"
#include"Boss/Msg/SwapCompleted.hpp"
#include"Boss/Msg/SwapCreation.hpp"
#include"Boss/Msg/SwapRequest.hpp"
#include"Boss/Msg/SwapResponse.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Ev/yield.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include"Uuid.hpp"
#include<algorithm>
#include<assert.h>
#include<queue>
#include<random>
#include<sstream>

namespace Boss { namespace Mod {

class SwapManager::Impl {
private:
	S::Bus& bus;
	Sqlite3::Db db;
	bool getting_address;
	bool getting_invoice;

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl( S::Bus& bus_
	    ) : bus(bus_)
	      , getting_address(false)
	      , getting_invoice(false)
	      { start(); }

private:
	/* /!\ db column 'state', do not change numbers!  */
	enum State
	{ NeedsOnchainAddress = 0
	, NeedsInvoice = 1
	, AwaitingResult = 2
	};

	void start() {
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& r) {
			db = r.db;
			return on_init();
		});
		bus.subscribe<Msg::Timer10Minutes
			     >([this](Msg::Timer10Minutes const& _) {
			return Boss::concurrent(on_periodic());
		});
		bus.subscribe<Msg::ResponseNewaddr
			     >([this](Msg::ResponseNewaddr const& r) {
			if (r.requester != this)
				return Ev::lift();
			return Boss::concurrent(
				on_response_newaddr(r.address)
			);
		});
		bus.subscribe<Msg::ProvideSwapQuotation
			     >([this](Msg::ProvideSwapQuotation const& q) {
			if (q.solicitor != this)
				return Ev::lift();
			/* Not concurrent since the processing expects
			 * synchronous response.  */
			return on_provide_swap_quotation( q.fee, q.provider
							, q.provider_name
							);
		});
		bus.subscribe<Msg::SwapCreation
			     >([this](Msg::SwapCreation const& s) {
			if (s.solicitor != this)
				return Ev::lift();
			return Boss::concurrent(
				on_swap_creation(s)
			);
		});
		bus.subscribe<Msg::ResponseListpays
			     >([this](Msg::ResponseListpays const& r) {
			return Boss::concurrent(
				on_response_listpays(r)
			);
		});
		bus.subscribe<Msg::ListfundsResult
			     >([this](Msg::ListfundsResult const& r) {
			return Boss::concurrent(
				on_listfunds_result(r)
			);
		});
		bus.subscribe<Msg::SwapRequest
			     >([this](Msg::SwapRequest const& r) {
			/* Not concurrent since we need the db transaction.  */
			return on_swap_request(r);
		});
		bus.subscribe<Msg::Block
			     >([this](Msg::Block const& b) {
			return Boss::concurrent(
				on_block(b.height)
			);
		});
		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const&) {
			/* Not concurrent, this has to reply
			 * synchronously.  */
			return on_solicit_status();
		});
	}

	/* On initialize.  */
	Ev::Io<void> on_init() {
		return db.transact().then([](Sqlite3::Tx tx) {
			tx.query_execute(R"QRY(
			CREATE TABLE IF NOT EXISTS "SwapManager"
			     ( uuid TEXT UNIQUE
			     -- in millisatoshis
			     , amount INTEGER NOT NULL
			     , min_amount INTEGER NOT NULL

			     -- current state.
			     , state INTEGER NOT NULL

			     -- onchain address.
			     , address TEXT

			     -- current being tried
			     , payment_hash TEXT
			     , timeout INTEGER
			     );
			CREATE INDEX IF NOT EXISTS "SwapManager_hash_index"
			    ON "SwapManager"(payment_hash)
			     ;
			CREATE INDEX IF NOT EXISTS "SwapManager_address_index"
			    ON "SwapManager"(address)
			     ;

			-- This is just a list of addresses whose swap has
			-- failed.
			-- When a swap is failed, we delete the swap and
			-- put its address here, and future swaps can
			-- use these instead of generating new addresses.
			CREATE TABLE IF NOT EXISTS "SwapManager_addrcache"
			     ( id INTEGER PRIMARY KEY -- ROWID
			     , address TEXT NOT NULL
			     );

			-- A mapping of swaps and the name of the provider
			-- of the swap service.
			CREATE TABLE IF NOT EXISTS "SwapManager_provider"
			     ( uuid TEXT UNIQUE
			       REFERENCES "SwapManager"(uuid)
			       ON DELETE CASCADE
			       ON UPDATE RESTRICT
			     , name TEXT NOT NULL
			     );

			-- Used to coordinate information on completion
			-- of swaps.
			-- This table is for swaps that have already been
			-- seen onchain.
			CREATE TABLE IF NOT EXISTS "SwapManager_comp_onchain"
			     ( payment_hash TEXT UNIQUE
			     , amount_received INTEGER NOT NULL
			     , provider_name TEXT NOT NULL
			     );
			-- This table is for swaps that have already been
			-- succeeded on-Lightning payment.
			CREATE TABLE IF NOT EXISTS "SwapManager_comp_pay"
			     ( payment_hash TEXT UNIQUE
			     , amount_sent INTEGER NOT NULL
			     );

			-- Sanity check to remove erroneous blank addresses
			-- from the address cache.
			DELETE FROM SwapManager_addrcache
			 WHERE address IS NULL
			  OR address = '';
			)QRY");
			tx.commit();

			return Ev::lift();
		}).then([this]() {
			return Boss::concurrent(on_periodic());
		});
	}

	/* Queue of swaps that need a new address.  */
	std::queue<Uuid> needs_address;
	/* Queue of swaps that need an invoice from a swap provider.  */
	std::queue<Uuid> needs_invoice;

	Ev::Io<void> on_periodic() {
		return Ev::lift().then([this]() {
			return load_queue( needs_address
					 , NeedsOnchainAddress
					 ).then([this]() {
				return Boss::concurrent(
					process_needs_address()
				);
			});
		}).then([this]() {
			return load_queue( needs_invoice
					 , NeedsInvoice
					 ).then([this]() {
				return Boss::concurrent(
					process_needs_invoice()
				);
			});
		}).then([this]() {
			return initiate_check_payments();
		});
	}
	Ev::Io<void> load_queue(std::queue<Uuid>& q, State s) {
		return db.transact().then([ &q
					  , s
					  ](Sqlite3::Tx tx) {
			if (!q.empty()) {
				/* Abort after all.  */
				tx.rollback();
				return Ev::lift();
			}

			auto res = tx.query(R"QRY(
			SELECT uuid FROM "SwapManager"
			 WHERE state = :state
			     ;
			)QRY")
				.bind(":state", int(s))
				.execute();
			for (auto& r : res) {
				q.push(Uuid(
					r.get<std::string>(0)
				));
			}
			tx.commit();
			return Ev::lift();
		});
	}

	/* Processing of items in needs-address.  */
	Ev::Io<void> process_needs_address() {
		if (getting_address)
			return Ev::lift();
		getting_address = true;
		return loop_needs_address();
	}
	Ev::Io<void> loop_needs_address() {
		if (needs_address.empty()) {
			getting_address = false;
			return Boss::log( bus, Debug
					, "SwapManager: "
					  "no more swaps need addresses"
					);
		}
		auto uuid = needs_address.front();
		return Boss::log( bus, Debug
				, "SwapManager: Swap %s getting address."
				, std::string(uuid).c_str()
				).then([this]() {
			return db.transact();
		}).then([this](Sqlite3::Tx tx) {
			auto uuid = needs_address.front();

			/* First, try to get an address from the addrcache.  */
			auto check = tx.query(R"QRY(
			SELECT id, address FROM "SwapManager_addrcache"
                         WHERE address IS NOT NULL
                          AND address <> ''
			 ORDER BY id
			 LIMIT 1
			     ;
			)QRY").execute();
			for (auto& r : check) {
				/* Got one!  Move from address cache to
				 * actual swap.
				 */
				auto id = r.get<std::uint64_t>(0);
				auto address = r.get<std::string>(1);
				tx.query(R"QRY(
				DELETE FROM SwapManager_addrcache
				 WHERE id = :id
				     ;
				)QRY")
					.bind(":id", id)
					.execute();
				return load_address( std::move(tx)
						   , std::move(uuid)
						   , std::move(address)
						   );
			}
			tx.commit();

			return bus.raise(Msg::RequestNewaddr{this});
		});
	}
	Ev::Io<void> on_response_newaddr(std::string n_address) {
		assert(getting_address);
		auto address = std::make_shared<std::string>(
			std::move(n_address)
		);
		return db.transact().then([this, address](Sqlite3::Tx tx) {
			auto uuid = needs_address.front();
			return load_address( std::move(tx)
					   , std::move(uuid)
					   , std::move(*address)
					   );
		});
	}
	Ev::Io<void> load_address( Sqlite3::Tx tx
				 , Uuid uuid
				 , std::string address
				 ) {
		tx.query(R"QRY(
		UPDATE SwapManager
		   SET address = :address
		     , state = :newstate
		 WHERE uuid = :uuid
		   AND state = :oldstate
		     ;
		)QRY")
			.bind(":address", address)
			.bind(":oldstate"
			     , (int) NeedsOnchainAddress
			     )
			.bind(":newstate", (int) NeedsInvoice)
			.bind(":uuid", std::string(uuid))
			.execute();
		tx.commit();

		/* Pop it off.  */
		needs_address.pop();

		auto act = Boss::log( bus, Debug
				    , "SwapManager: Swap %s got address %s"
				    , std::string(uuid).c_str()
				    , std::string(address).c_str()
				    );
		/* Do we need to start up the invoice-getting loop as well?  */
		if (needs_invoice.empty()) {
			act = std::move(act).then([this]() {
				return Boss::concurrent(
					process_needs_invoice()
				);
			});
		}
		/* Push it in the needs-invoice loop.  */
		needs_invoice.push(std::move(uuid));

		return std::move(act).then([]() {
			return Ev::yield();
		}).then([this]() {
			return loop_needs_address();
		});
	}

	/* Processing of items in needs-invoice.  */
	Ev::Io<void> process_needs_invoice() {
		if (getting_invoice)
			return Ev::lift();
		getting_invoice = true;
		return loop_needs_invoice();
	}

	struct Quotation {
		std::uint64_t score;
		void* provider;
		std::string provider_name;
		Quotation( std::uint64_t score_
			 , void* provider_
			 , std::string provider_name_
			 ) : score(score_)
			   , provider(provider_)
			   , provider_name(std::move(provider_name_))
			   { }
	};

	std::vector<Quotation> quotations;
	Ln::Amount amount;
	std::string address;

	Ev::Io<void> loop_needs_invoice() {
		if (needs_invoice.empty()) {
			getting_invoice = false;
			return Ev::lift();
		}
		quotations.clear();
		return db.transact().then([this](Sqlite3::Tx tx) {
			/* Get the amount of the swap.  */
			auto uuid = needs_invoice.front();
			auto fetch = tx.query(R"QRY(
			SELECT amount, address FROM "SwapManager"
			 WHERE uuid = :uuid
			     ;
			)QRY")
				.bind(":uuid", std::string(uuid))
				.execute()
				;
			for (auto& r : fetch) {
				amount = Ln::Amount::msat(
					r.get<std::uint64_t>(0)
				);
				address = r.get<std::string>(1);
			}
			tx.commit();

			return bus.raise(Msg::SolicitSwapQuotation{
				amount, this
			});
		}).then([this]() {
			auto uuid = needs_invoice.front();
			return Boss::log( bus, Debug
					, "SwapManager: Swap %s got %zu "
					  "quotes for amount %s."
					, std::string(uuid).c_str()
					, quotations.size()
					, std::string(amount).c_str()
					);
		}).then([this]() {
			/* Randomize quotations.  */
			for (auto& q : quotations) {
				auto dist = std::uniform_int_distribution<std::uint64_t>(
					0, q.score
				);
				q.score = dist(Boss::random_engine);
			}

			/* Sort quotations, from highest-fee to lowest-fee.  */
			std::sort( quotations.begin(), quotations.end()
				 , []( Quotation const& a
				     , Quotation const& b
				     ) {
				return a.score > b.score;
			});

			/* Enter the quotations loop.  */
			return loop_quotations();
		});
	}
	Ev::Io<void> on_provide_swap_quotation( Ln::Amount fee
					      , void* provider
					      , std::string provider_name
					      ) {
		quotations.emplace_back( fee.to_msat()
				       , provider
				       , std::move(provider_name)
				       );
		return Ev::lift();
	}
	/* Process the quotations.  */
	Ev::Io<void> loop_quotations() {
		if (quotations.size() == 0) {
			auto uuid = std::move(needs_invoice.front());
			/* Fail the current swap.  */
			needs_invoice.pop();
			return Ev::yield().then([this, uuid]() {
				return swap_reduce_or_fail(uuid);
			}).then([this]() {
				/* Go to next swap.  */
				return loop_needs_invoice();
			});
		}

		auto const& quotation = quotations[quotations.size() - 1];
		return bus.raise(Msg::AcceptSwapQuotation{
			amount, address,
			this, quotation.provider
		});
	}
	/* On swap creation.  */
	Ev::Io<void> on_swap_creation(Msg::SwapCreation const& s) {
		if (!s.success) {
			/* Remove a quotation and try again.  */
			quotations.pop_back();
			return Ev::yield().then([this]() {
				return loop_quotations();
			});
		}

		auto const& quotation = quotations[quotations.size() - 1];
		auto const& provider_name = quotation.provider_name;

		auto swap = std::make_shared<Msg::SwapCreation>(s);

		/* Otherwise, set up the swap.  */
		return db.transact().then([ this
					  , swap
					  , provider_name
					  ](Sqlite3::Tx tx) {
			auto uuid = needs_invoice.front();
			auto swap_amount = Ln::Amount::sat(0);

			auto amt_query = tx.query(R"QRY(
			SELECT amount FROM "SwapManager"
			 WHERE uuid = :uuid
			     ;
			)QRY")
				.bind(":uuid", std::string(uuid))
				.execute();
			for (auto& r : amt_query) {
				swap_amount = Ln::Amount::sat(r.get<std::uint64_t>(0));
				break;
			}

			tx.query(R"QRY(
			UPDATE "SwapManager"
			   SET state = :state
			     , payment_hash = :payment_hash
			     , timeout = :timeout
			 WHERE uuid = :uuid
			     ;
			)QRY")
				.bind(":state", (int)AwaitingResult)
				.bind(":payment_hash"
				     , std::string(swap->hash)
				     )
				.bind(":timeout"
				     , swap->timeout_blockheight
				     )
				.bind(":uuid", std::string(uuid))
				.execute();
			tx.query(R"QRY(
			INSERT OR REPLACE INTO "SwapManager_provider"
			     VALUES( :uuid
				   , :provider
				   );
			)QRY")
				.bind(":uuid", std::string(uuid))
				.bind(":provider", provider_name)
				.execute();
			tx.commit();
			/* Now send the PayInvoice message.  */
			return bus.raise(Msg::PayInvoice{swap->invoice, std::string(swap->hash), swap_amount.to_msat()});
		}).then([this, swap]() {
			auto uuid = needs_invoice.front();
			return Boss::log( bus, Debug
					, "SwapManager: Swap %s got "
					  "invoice %s hash %s "
					  "timeout %u"
					, std::string(uuid).c_str()
					, swap->invoice.c_str()
					, std::string(swap->hash).c_str()
					, (unsigned int)
					  swap->timeout_blockheight
					);
		}).then([]() {
			return Ev::yield();
		}).then([this]() {
			needs_invoice.pop();
			return loop_needs_invoice();
		});
	}

	/* Try to reduce the specified swap, and if we reduced,
	 * put it back into the needs-invoice loop.  */
	Ev::Io<void> swap_reduce_or_fail(Uuid uuid) {
		return db.transact().then([this, uuid](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
			SELECT amount, min_amount
			  FROM "SwapManager"
			 WHERE uuid = :uuid
			     ;
			)QRY")
				.bind(":uuid", std::string(uuid))
				.execute();
			auto amount = std::uint64_t();
			auto min_amount = std::uint64_t();
			for (auto& r : fetch) {
				amount = r.get<std::uint64_t>(0);
				min_amount = r.get<std::uint64_t>(1);
			}

			/* cannot go lower!  */
			if (amount == min_amount)
				return fail_swap(std::move(tx), uuid);

			/* Reduce.  */
			auto dist = std::uniform_int_distribution<std::uint64_t>(
				amount / 4, amount * 3 / 4
			);
			amount = dist(Boss::random_engine);
			if (amount < min_amount)
				amount = min_amount;
			tx.query(R"QRY(
			UPDATE "SwapManager"
			   SET state = :state
			     , amount = :amount
			     , payment_hash = NULL
			     , timeout = NULL
			 WHERE uuid = :uuid
			     ;
			)QRY")
				.bind(":state", (int)NeedsInvoice)
				.bind(":amount", amount)
				.bind(":uuid", std::string(uuid))
				.execute();
			tx.commit();
			/* Push it back to the needs-invoice queue.  */
			needs_invoice.push(uuid);
			/* Log, then restart needs-invoice if needed.  */
			return Boss::log( bus, Debug
					, "SwapManager: Swap %s reduced to "
					  "%zu msat."
					, std::string(uuid).c_str()
					, std::size_t(amount)
					).then([this]() {
				return Boss::concurrent(
					process_needs_invoice()
				);
			});
		});
	}

	/* Called when we know that the swap will definitely fail
	 * as we cannot go lower.  */
	Ev::Io<void> fail_swap(Sqlite3::Tx tx, Uuid uuid) {
		/* Move the address to the addrcache.  */
		auto fetch = tx.query(R"QRY(
		SELECT address
		  FROM "SwapManager"
		 WHERE uuid = :uuid
		     ;
		)QRY")
			.bind(":uuid", std::string(uuid))
			.execute();
		auto address = std::string();
		for (auto& r : fetch)
			address = r.get<std::string>(0);
		if (!address.empty()) {
			tx.query(R"QRY(
		        INSERT INTO "SwapManager_addrcache"
		        VALUES(NULL, :address);
		        )QRY")
				.bind(":address", address)
				.execute();
		}

		/* Delete the swap itself.  */
		tx.query(R"QRY(
		DELETE FROM "SwapManager"
		 WHERE uuid = :uuid
		     ;
		)QRY")
			.bind(":uuid", std::string(uuid))
			.execute();

		/* Move the db tx to a shared pointer.  */
		auto sh_tx = std::make_shared<Sqlite3::Tx>(std::move(tx));
		/* Inform the failure.  */
		return Boss::log( bus, Info
				, "SwapManager: Swap %s failed."
				, std::string(uuid).c_str()
				).then([this, sh_tx, uuid]() {
			return bus.raise(Msg::SwapResponse{
				sh_tx, uuid, false, Ln::Amount()
			});
		}).then([sh_tx]() {
			if (*sh_tx)
				sh_tx->commit();
			return Ev::lift();
		});
	}

	/* Raise RequestListpays on all AwaitingResult
	 * swaps.  */
	Ev::Io<void> initiate_check_payments() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
			SELECT payment_hash
			  FROM "SwapManager"
			 WHERE state = :state
			     ;
			)QRY")
				.bind(":state", (int)AwaitingResult)
				.execute();
			auto hashes = std::vector<Sha256::Hash>();
			for (auto& r : fetch)
				hashes.push_back(Sha256::Hash(
					r.get<std::string>(0)
				));

			tx.commit();

			auto f = [this](Sha256::Hash h) {
				return bus.raise(Msg::RequestListpays{h});
			};

			return Ev::foreach(f, std::move(hashes));
		});
	}
	/* Check response to RequestListpays.  */
	Ev::Io<void> on_response_listpays(Msg::ResponseListpays const& r) {
		auto status = r.status;

		/* If it succeeded, trigger on_pay_success.  */
		if (status == Msg::StatusListpays_success)
			return on_pay_success( r.payment_hash
					     , r.amount_sent
					     );

		if (status != Msg::StatusListpays_failed)
			return Ev::lift();
		auto hash = r.payment_hash;
		return db.transact().then([this, hash](Sqlite3::Tx tx) {
			/* Check if it is in our table.  */
			auto check = tx.query(R"QRY(
			SELECT uuid
			  FROM "SwapManager"
			 WHERE payment_hash = :hash
			     ;
			)QRY")
				.bind(":hash", std::string(hash))
				.execute();
			auto found = false;
			auto uuid = Uuid();
			for (auto& r : check) {
				found = true;
				uuid = Uuid(r.get<std::string>(0));
			}
			tx.commit();

			/* Not in our table.  */
			if (!found)
				return Ev::lift();

			/* Lower it or fail!  */
			return swap_reduce_or_fail(uuid);
		});
	}

	/* Check for funds appearing onchain when the swap completes.  */
	std::queue<std::pair<std::string, Ln::Amount>> onchain_funds;
	Ev::Io<void> on_listfunds_result(Msg::ListfundsResult const& r) {
		auto outputs = r.outputs;
		return Ev::lift().then([this, outputs]() {
			auto was_empty = onchain_funds.empty();
			/* Iterate over the outputs array.  */
			for (auto out : outputs)
				onchain_funds.push(std::make_pair(
					std::string(out["address"]),
					Ln::Amount::object(out["amount_msat"]
					)
				));

			if (was_empty)
				return Boss::concurrent(loop_onchain_funds());
			return Ev::lift();
		});
	}
	Ev::Io<void> loop_onchain_funds() {
		return Ev::yield().then([this]() {
			return db.transact();
		}).then([this](Sqlite3::Tx tx) {
			if (onchain_funds.empty())
				return Ev::lift();

			auto fund = std::move(onchain_funds.front());
			onchain_funds.pop();

			auto check = tx.query(R"QRY(
			SELECT uuid, payment_hash
			  FROM "SwapManager"
			 WHERE address = :address
			   AND state = :state
			)QRY")
				.bind(":address", fund.first)
				.bind(":state", (int)AwaitingResult)
				.execute();
			auto found = false;
			auto uuid = Uuid();
			auto hash = Sha256::Hash();
			for (auto& r : check) {
				found = true;
				uuid = Uuid(r.get<std::string>(0));
				hash = Sha256::Hash(r.get<std::string>(1));
			}

			auto act = Ev::lift();
			if (found) {
				/* Get provider name.  */
				auto fetch_pn = tx.query(R"QRY(
				SELECT name
				  FROM "SwapManager_provider"
				 WHERE uuid = :uuid
				     ;
				)QRY")
					.bind(":uuid", std::string(uuid))
					.execute();
				auto provider_name = std::string("<Unknown>");
				for (auto& r : fetch_pn) {
					provider_name = r.get<std::string>(0);
					break;
				}

				act += Boss::concurrent(
					on_onchain_funds_seen( hash
							     , fund.second
							     , provider_name
							     )
				);

				/* Remove it.  */
				tx.query(R"QRY(
				DELETE FROM "SwapManager"
				 WHERE uuid = :uuid
				     ;
				)QRY")
					.bind(":uuid", std::string(uuid))
					.execute();

				/* Construct action to log it
				 * and broadcast.  */
				auto sh_tx = std::make_shared<Sqlite3::Tx>(
					std::move(tx)
				);
				auto amount = fund.second;
				act += Boss::log( bus, Info
						, "SwapManager: "
						  "Swap %s completed "
						  "with %s onchain.  "
						  "Provider: %s"
						, std::string(uuid)
							.c_str()
						, std::string(amount)
							.c_str()
						, provider_name.c_str()
						);
				act += bus.raise(Msg::SwapResponse{
					sh_tx,
					uuid, true, amount
				}).then([sh_tx]() {
					if (*sh_tx)
						sh_tx->commit();
					return Ev::lift();
				});
				act = Boss::concurrent(std::move(act));
			} else
				tx.commit();

			return std::move(act).then([this]() {
				return loop_onchain_funds();
			});
		});
	}

	Ev::Io<void> on_swap_request(Msg::SwapRequest const& r) {
		return Ev::lift().then([this, r]() {
			auto tx = std::move(*r.dbtx);
			auto uuid = r.id;
			/* Check the uuid is not exist yet.  */
			auto check = tx.query(R"QRY(
			SELECT uuid
			  FROM "SwapManager"
			 WHERE uuid = :uuid
			     ;
			)QRY")
				.bind(":uuid", std::string(uuid))
				.execute();
			for (auto& r : check) {
				(void) r;
				tx.rollback();
				return Boss::log( bus, Error
						, "SwapManager: Swap %s "
						  "duplicated."
						, std::string(uuid).c_str()
						);
			}

			auto amount = r.max_offchain_amount;
			auto min_amount = r.min_offchain_amount;

			/* Make new entry.  */
			tx.query(R"QRY(
			INSERT INTO "SwapManager"
			      ( uuid
			      , amount
			      , min_amount
			      , state
			      )
			VALUES( :uuid
			      , :amount
			      , :min_amount
			      , :state
			      );
			)QRY")
				.bind(":uuid", std::string(uuid))
				.bind(":amount"
				     , amount.to_msat()
				     )
				.bind(":min_amount"
				     , min_amount.to_msat()
				     )
				.bind(":state", (int)NeedsOnchainAddress)
				.execute();
			tx.commit();

			needs_address.push(uuid);
			return Boss::log( bus, Info
					, "SwapManager: Swap %s started "
					  "for %s."
					, std::string(uuid).c_str()
					, std::string(amount).c_str()
					).then([this]() {
				return Boss::concurrent(
					process_needs_address()
				);
			});
		});
	}


	Ev::Io<void> on_block(std::uint32_t height) {
		return db.transact().then([this, height](Sqlite3::Tx tx) {
			/* Gather items to remove.  */
			auto remove = std::vector<Uuid>();
			auto fetch = tx.query(R"QRY(
			SELECT uuid
			  FROM "SwapManager"
			 WHERE state = :state
			   AND timeout <= :height
			     ;
			)QRY")
				.bind(":height", height)
				.bind(":state", (int)AwaitingResult)
				.execute();
			for (auto& r : fetch)
				remove.push_back(Uuid(
					r.get<std::string>(0)
				));
			tx.commit();

			/* Process items to remove.  */
			auto f = [this](Uuid uuid) {
				return Boss::log( bus, Warn
						, "SwapManager: Swap %s "
						  "timed out."
						, std::string(uuid).c_str()
						).then([this, uuid]() {
					return swap_reduce_or_fail(uuid);
				});
			};
			return Ev::foreach(f, std::move(remove));
		});
	}

	Ev::Io<void> on_solicit_status() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto out = Json::Out();
			auto arr = out.start_array();

			auto fetch = tx.query(R"QRY(
			SELECT uuid -- 0
			     , amount -- 1
			     , min_amount -- 2
			     , state -- 3
			     , address -- 4
			     , payment_hash -- 5
			     , timeout -- 6
			  FROM "SwapManager";
			)QRY").execute();
			for (auto& r : fetch) {
				auto obj = arr.start_object();

				obj.field("uuid", r.get<std::string>(0));
				obj.field("amount", r.get<std::uint64_t>(1));
				obj.field("min_amount"
					 , r.get<std::uint64_t>(2)
					 );
				obj.field("state", r.get<int>(3));
				auto state = State(r.get<int>(3));
				if (state != NeedsOnchainAddress)
					obj.field("address"
						 , r.get<std::string>(4)
						 );
				if (state == AwaitingResult) {
					obj.field("hash"
						 , r.get<std::string>(5)
						 );
					obj.field("timeout"
						 , r.get<std::uint32_t>(6)
						 );
				}

				obj.end_object();
			}

			arr.end_array();

			return bus.raise(Msg::ProvideStatus{
				"swap_manager", std::move(out)
			});
		});
	}

	Ev::Io<void>
	on_pay_success( Sha256::Hash const& hash
		      , Ln::Amount amount_sent
		      ) {
		return db.transact().then([ this
					  , hash
					  , amount_sent
					  ](Sqlite3::Tx tx) {
			/* First, check if it was already seen onchain.  */
			auto check_comp = tx.query(R"QRY(
			SELECT amount_received, provider_name
			  FROM "SwapManager_comp_onchain"
			 WHERE payment_hash = :hash
			     ;
			)QRY")
				.bind(":hash", std::string(hash))
				.execute()
				;
			auto found_comp = false;
			auto amount_received = Ln::Amount::sat(0);
			auto provider_name = std::string();
			for (auto& r : check_comp) {
				found_comp = true;
				amount_received = Ln::Amount::msat(
					r.get<std::uint64_t>(0)
				);
				provider_name = r.get<std::string>(1);
			}
			if (found_comp) {
				/* Delete the completion record.  */
				tx.query(R"QRY(
				DELETE FROM "SwapManager_comp_onchain"
				 WHERE payment_hash = :hash
				)QRY")
					.bind(":hash", std::string(hash))
					.execute()
					;
				/* Broadcast, handing over the database
				 * transaction.  */
				return broadcast_completed(
					std::move(tx),
					amount_sent,
					amount_received,
					provider_name
				);
			}

			/* Otherwise create a record that it was seen
			 * paid.  */
			tx.query(R"QRY(
			INSERT OR IGNORE
			  INTO "SwapManager_comp_pay"
			VALUES( :hash
			      , :amount_sent
			      );
			)QRY")
				.bind(":hash", std::string(hash))
				.bind(":amount_sent", amount_sent.to_msat())
				.execute()
				;

			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void>
	on_onchain_funds_seen( Sha256::Hash const& hash
			     , Ln::Amount amount_received
			     , std::string const& provider_name
			     ) {
		return db.transact().then([ this
					  , hash
					  , amount_received
					  , provider_name
					  ](Sqlite3::Tx tx) {
			/* First, check if it was already seen on outgoing
			 * pay.
			 */
			auto check_comp = tx.query(R"QRY(
			SELECT amount_sent
			  FROM "SwapManager_comp_pay"
			 WHERE payment_hash = :hash
			     ;
			)QRY")
				.bind(":hash", std::string(hash))
				.execute()
				;
			auto found_comp = false;
			auto amount_sent = Ln::Amount::sat(0);
			for (auto& r : check_comp) {
				found_comp = true;
				amount_sent = Ln::Amount::msat(
					r.get<std::uint64_t>(0)
				);
			}
			if (found_comp) {
				/* Delete the completion record.  */
				tx.query(R"QRY(
				DELETE FROM "SwapManager_comp_pay"
				 WHERE payment_hash = :hash
				     ;
				)QRY")
					.bind(":hash", std::string(hash))
					.execute()
					;
				/* Broadcast, handing over the database
				 * transaction.
				 */
				return broadcast_completed(
					std::move(tx),
					amount_sent,
					amount_received,
					provider_name
				);
			}

			/* Otherwise create a completion record that we
			 * have seen it onchain.
			 */
			tx.query(R"QRY(
			INSERT OR IGNORE INTO "SwapManager_comp_onchain"
			VALUES( :hash
			      , :amount_received
			      , :provider_name
			      );
			)QRY")
				.bind(":hash", std::string(hash))
				.bind( ":amount_received"
				     , amount_received.to_msat()
				     )
				.bind(":provider_name", provider_name)
				.execute()
				;

			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void>
	broadcast_completed( Sqlite3::Tx tx
			   , Ln::Amount amount_sent
			   , Ln::Amount amount_received
			   , std::string const& provider_name
			   ) {
		auto sh_tx = std::make_shared<Sqlite3::Tx>(
			std::move(tx)
		);
		auto act = Ev::lift();
		act += Boss::log( bus, Info
				, "SwapManager: Sent %s on Lightning, got %s onchain.  "
				, std::string(amount_sent).c_str()
				, std::string(amount_received).c_str()
				);
		act += bus.raise(Msg::SwapCompleted{
			sh_tx, amount_sent, amount_received,
			provider_name
		});
		return std::move(act).then([sh_tx]() {
			if (*sh_tx)
				sh_tx->commit();
			return Ev::lift();
		});
	}
};


SwapManager::SwapManager(SwapManager&&) =default;
SwapManager& SwapManager::operator=(SwapManager&&) =default;
SwapManager::~SwapManager() =default;

SwapManager::SwapManager(S::Bus& bus
			) : pimpl(Util::make_unique<Impl>(bus))
			  { }

}}
