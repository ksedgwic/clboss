#include"Bitcoin/Tx.hpp"
#include"Boltz/Detail/ClaimTxHandler.hpp"
#include"Boltz/Detail/ServiceImpl.hpp"
#include"Boltz/Detail/SwapSetupHandler.hpp"
#include"Boltz/Detail/reverse_terms.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boltz/SwapStatus.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include<sstream>

/* General note: the database schema is in `Boltz/ServiceFactory.cpp`.  */

namespace Boltz { namespace Detail {

std::string ServiceImpl::prefixlog(std::string msg) {
	return std::string("Boltz::Service(\"") + label + "\"): " + msg;
}
Ev::Io<void> ServiceImpl::logd(std::string msg) {
	return env.logd(prefixlog(std::move(msg)));
}
Ev::Io<void> ServiceImpl::loge(std::string msg) {
	return env.loge(prefixlog(std::move(msg)));
}

Ev::Io<void> ServiceImpl::on_block(std::uint32_t blockheight) {
	return db.transact().then([this, blockheight](Sqlite3::Tx dbtx) {
		/* A swap past its timeout with the preimage still here
		 * is over: the payment fails back to us and the provider
		 * refunds itself.  One whose preimage is out stays: the
		 * claim handler tracks it until the lockup is spent.  */
		dbtx.query(R"QRY(
		DELETE FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND timeoutBlockheight <= :blockheight
		   AND preimageRevealed = 0
		     ;
		)QRY")
			.bind(":apiAccess", label)
			.bind(":blockheight", blockheight)
			.execute();

		/* Get all the IDs of swaps still ongoing.  */
		auto swaps = dbtx.query(R"QRY(
		SELECT swapId FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		     ;
		)QRY")
			.bind(":apiAccess", label)
			.execute();
		auto swapIds = std::vector<std::string>();
		for (auto& r : swaps)
			swapIds.push_back(r.get<std::string>(0));

		dbtx.commit();

		return Ev::lift(std::move(swapIds));
	}).then([this](std::vector<std::string> swapIds) {

		/* Do not pollute log if nothing to swap anyway.  */
		if (swapIds.empty())
			return Ev::lift(std::move(swapIds));

		/* Print a log.  */
		auto tmp_swapIds = std::make_shared<std::vector<std::string>>(
			std::move(swapIds)
		);
		auto os = std::ostringstream();
		os << "Swaps: ";
		auto first = true;
		for (auto const& swapId : *tmp_swapIds) {
			if (first)
				first = false;
			else
				os << ", ";
			os << "\"" << swapId << "\"";
		}
		return logd(os.str()).then([tmp_swapIds]() {
			return Ev::lift(std::move(*tmp_swapIds));
		});
	}).then([this, blockheight](std::vector<std::string> swapIds) {
		auto f = [this, blockheight](std::string swapId) {
			return swap_on_block(blockheight, std::move(swapId));
		};

		return Ev::foreach(f, std::move(swapIds));
	});
}

Ev::Io<void> ServiceImpl::swap_on_block( std::uint32_t blockheight
				       , std::string swapId
				       ) {
	auto p_swapId = std::make_shared<std::string>(std::move(swapId));
	return db.transact().then([this, p_swapId](Sqlite3::Tx tx) {
		auto fetch = tx.query(R"QRY(
		SELECT preimageRevealed FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = :swapId
		     ;
		)QRY")
			.bind(":apiAccess", label)
			.bind(":swapId", *p_swapId)
			.execute();
		auto revealed = false;
		for (auto& r : fetch)
			revealed = r.get<bool>(0);
		tx.commit();
		return Ev::lift(revealed);
	}).then([this, blockheight, p_swapId](bool revealed) {
		/* Once the preimage is out the provider's status is
		 * advisory: the claim is tracked from our own node until
		 * it is confirmed there.  The handler works from the
		 * lockup recorded at the reveal.  */
		if (revealed)
			return swap_onchain( p_swapId, blockheight
					   , std::make_shared<Bitcoin::Tx>()
					   );
		return swap_poll(blockheight, p_swapId);
	});
}

Ev::Io<void> ServiceImpl::swap_poll( std::uint32_t blockheight
				   , std::shared_ptr<std::string> p_swapId
				   ) {
	return conn->api( "/v2/swap/" + *p_swapId, nullptr
			).then([this, p_swapId](Jsmn::Object sres) {
			/* Save, then log.  */
			auto tmp_sres = std::make_shared<Jsmn::Object>(
				std::move(sres)
			);
			auto os = std::ostringstream();
			os << "/v2/swap/" << *p_swapId << " => "
			   << *tmp_sres
			    ;
			return logd(os.str()).then([tmp_sres]() {
				return Ev::lift(std::move(*tmp_sres));
			});
		}).then([ this
			, blockheight
			, p_swapId
			](Jsmn::Object sres) {
		auto kont = std::function<Ev::Io<void>()>();
		try {
			if (sres.has("error")) {
				auto p_err = std::make_shared<std::string>(
					sres["error"]
				);
				kont = [this, p_swapId, p_err]() {
					auto os = std::ostringstream();
					os << "\"" << *p_swapId << "\": "
					   << "Server error: "
					   << *p_err
					    ;
					return loge(os.str()).then([ this
								   , p_swapId
								   ]() {
						return swap_errored(p_swapId);
					});
				};
				return Ev::lift(kont);
			}
			auto status = std::string(sres["status"]);

			if ( status == "swap.created"
			  || status == "transaction.mempool"
			   ) {
				/* Do nothing.  Wait for transaction to
				 * confirm.  */
				kont = []() { return Ev::lift(); };
				return Ev::lift(kont);
			}

			if ( status == "transaction.failed"
			  || status == "transaction.refunded"
			  || status == "swap.expired"
			   ) {
				kont = [this, p_swapId]() {
					return swap_errored(p_swapId);
				};
				return Ev::lift(kont);
			}

			/* Only the preimage settles the invoice, and this
			 * node has not sent it: a revealed swap does not
			 * come here.  Nothing to finish; the row stays
			 * until its timeout.  */
			if (status == "invoice.settled") {
				kont = [this, p_swapId]() {
					return loge( "\"" + *p_swapId + "\": the "
						     "provider reports the invoice "
						     "settled, but this node never "
						     "revealed the preimage."
					   );
				};
				return Ev::lift(kont);
			}

			if (status == "transaction.confirmed") {
				auto txhex = std::string(
					sres["transaction"]["hex"]
				);
				auto tx = std::make_shared<Bitcoin::Tx>(
					txhex
				);
				kont = [this, p_swapId, blockheight, tx]() {
					return swap_onchain( p_swapId
							   , blockheight
							   , tx
							   );
				};
				return Ev::lift(kont);
			}

			/* Ignore all other states.  */
			kont = []() { return Ev::lift(); };
			return Ev::lift(kont);
		} catch (Jsmn::TypeError const& t) {
			/* Log invalid JSON.  */
			auto os = std::ostringstream();
			os << "/v2/swap/" << *p_swapId << " "
			   << "returned invalid JSON: "
			   << sres
			    ;
			auto str = std::make_shared<std::string>(os.str());
			kont = [this, str]() {
				return loge(*str);
			};
			return Ev::lift(kont);
		}
	}).catching<Boltz::ApiError>([this, p_swapId](Boltz::ApiError e) {
		/* Log that there is an error.  */
		auto kont = std::function<Ev::Io<void>()>();
		kont = [this, p_swapId]() {
			return loge( std::string("/v2/swap/")
				   + *p_swapId + " not accessible."
				   );
		};
		return Ev::lift(kont);
	}).then([](std::function<Ev::Io<void>()> kont) {
		return kont();
	});
}

Ev::Io<void>
ServiceImpl::swap_onchain( std::shared_ptr<std::string> swapId
			 , std::uint32_t blockheight
			 , std::shared_ptr<Bitcoin::Tx> tx
			 ) {
	return Ev::lift().then([ this
			       , swapId
			       , blockheight
			       , tx
			       ]() {
		auto handler = ClaimTxHandler::create
			( signer
			, db
			, env
			, label
			, *conn
			, random
			, *swapId
			, blockheight
			, std::move(*tx)
			);
		return handler->run();
	});
}

Ev::Io<void>
ServiceImpl::swap_errored(std::shared_ptr<std::string> swapId) {
	return Ev::lift().then([this, swapId]() {
		auto os = std::ostringstream();
		os << "\"" << *swapId << "\": Server will no longer swap.";
		return loge(os.str());
	}).then([this, swapId]() {
		return delete_swap(swapId);
	});
}
Ev::Io<void>
ServiceImpl::swap_finished(std::shared_ptr<std::string> swapId) {
	return Ev::lift().then([this, swapId]() {
		auto os = std::ostringstream();
		os << "\"" << *swapId << "\": Done.";
		return logd(os.str());
	}).then([this, swapId]() {
		return delete_swap(swapId);
	});
}

Ev::Io<void>
ServiceImpl::delete_swap(std::shared_ptr<std::string> swapId) {
	return Ev::lift().then([this]() {
		return db.transact();
	}).then([this, swapId](Sqlite3::Tx tx) {
		tx.query(R"QRY(
		DELETE FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = :swapId
		     ;
		)QRY")
			.bind(":apiAccess", label)
			.bind(":swapId", *swapId)
			.execute()
			;
		tx.commit();
		return Ev::lift();
	});
}

Ev::Io<SwapInfo>
ServiceImpl::swap( Ln::Amount offchainAmount
		 , std::string onchain_address
		 , std::uint32_t current_blockheight
		 ) {
	auto handler = SwapSetupHandler::create
		( signer
		, db
		, env
		, label
		, *conn
		, random
		, onchain_address
		, offchainAmount
		, current_blockheight
		);
	return handler->run();
}

Ev::Io<std::vector<SwapStatus>> ServiceImpl::list_swaps() {
	return db.transact().then([this](Sqlite3::Tx tx) {
		auto fetch = tx.query(R"QRY(
		SELECT swapId -- 0
		     , claimScript -- 1
		     , destinationAddress -- 2
		     , createdHeight -- 3
		     , timeoutBlockheight -- 4
		     , onchainAmount -- 5
		     , COALESCE(lockupTxid, '') -- 6
		     , COALESCE(lockupConfirmedHeight, 0) -- 7
		     , COALESCE(lockupSearchedHeight, 0) -- 8
		     , preimageRevealed -- 9
		     , claimTx -- 10
		     , claimFeerate -- 11
		     , COALESCE(lockupClaimFees, 0) -- 12
		     , COALESCE(spendSearchedHeight, 0) -- 13
		     , COALESCE(comment, '') -- 14
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		 ORDER BY id
		     ;
		)QRY")
			.bind(":apiAccess", label)
			.execute();
		auto rv = std::vector<SwapStatus>();
		for (auto& r : fetch) {
			auto s = SwapStatus();
			s.swap_id = r.get<std::string>(0);
			/* A v1 row has no leaves.  */
			s.api_v2 = !r.get<std::string>(1).empty();
			s.destination_address = r.get<std::string>(2);
			s.created_height = r.get<std::uint32_t>(3);
			s.timeout_height = r.get<std::uint32_t>(4);
			s.onchain_amount = Ln::Amount::sat(
				r.get<std::uint64_t>(5)
			);
			s.lockup_txid = r.get<std::string>(6);
			s.lockup_confirmed_height = r.get<std::uint32_t>(7);
			s.lockup_searched_height = r.get<std::uint32_t>(8);
			s.preimage_revealed = r.get<bool>(9);
			auto claim_hex = r.get<std::string>(10);
			if (!claim_hex.empty()) {
				try {
					s.claim_txid = std::string(
						Bitcoin::Tx(claim_hex).get_txid()
					);
				} catch (std::exception const&) {
					s.claim_txid = "unparseable";
				}
			}
			s.claim_feerate = r.get<std::uint32_t>(11);
			s.claim_fee = Ln::Amount::sat(r.get<std::uint64_t>(12));
			s.spend_searched_height = r.get<std::uint32_t>(13);
			s.comment = r.get<std::string>(14);
			rv.push_back(std::move(s));
		}
		tx.commit();
		return Ev::lift(std::move(rv));
	});
}

Ev::Io<std::unique_ptr<Ln::Amount>>
ServiceImpl::get_quotation(Ln::Amount offchainAmount) {
	return conn->api("/v2/swap/reverse", nullptr
			).then([this, offchainAmount](Jsmn::Object res) {

		auto terms = ReverseTerms();
		try {
			terms = parse_reverse_terms(res);
		} catch (Jsmn::TypeError const& _) {
			auto os = std::ostringstream();
			os << "/v2/swap/reverse unexpected result: " << res;
			return loge(os.str()).then([]() {
				return Ev::lift(std::unique_ptr<Ln::Amount>());
			});
		}

		/* Check range.  */
		if (terms.minimal > offchainAmount || terms.maximal < offchainAmount) {
			auto os = std::ostringstream();
			os << "/v2/swap/reverse out-of-range "
			   << terms.minimal.to_sat() << " < "
			   << offchainAmount.to_sat() << " < "
			   << terms.maximal.to_sat()
			    ;
			return loge(os.str()).then([]() {
				return Ev::lift(std::unique_ptr<Ln::Amount>());
			});
		}

		return Ev::lift(Util::make_unique<Ln::Amount>(
			offchainAmount - quoted_onchain_amount(terms, offchainAmount)
		));
	}).catching<ApiError>([this](ApiError const& e) {
		return logd("/v2/swap/reverse not accessible.").then([]() {
			return Ev::lift(std::unique_ptr<Ln::Amount>());
		});
	});
}


}}
