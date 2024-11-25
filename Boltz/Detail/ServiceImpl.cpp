#include"Bitcoin/Tx.hpp"
#include"Boltz/Detail/ClaimTxHandler.hpp"
#include"Boltz/Detail/ServiceImpl.hpp"
#include"Boltz/Detail/SwapSetupHandler.hpp"
#include"Boltz/EnvIF.hpp"
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
		/* Filter out rows we created that have completely finished
		 * the locktime.
		 */
		dbtx.query(R"QRY(
		DELETE FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND timeoutBlockheight <= :blockheight
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
	return conn->api( "/swap/" + *p_swapId, nullptr
			).then([this, p_swapId](Jsmn::Object sres) {
			/* Save, then log.  */
			auto tmp_sres = std::make_shared<Jsmn::Object>(
				std::move(sres)
			);
			auto os = std::ostringstream();
			os << "/swap \"" << *p_swapId << "\" => "
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

			/* FIXME: we should ignore this state, and instead
			 * monitor the txout moving from unspent to spent.
			 */
			if (status == "invoice.settled") {
				kont = [this, p_swapId]() {
					return swap_finished(p_swapId);
				};
				return Ev::lift(kont);
			}

			// TODO TODO check this status exists in v2 API
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
			os << "/swapstatus \"" << *p_swapId << "\" "
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
			return loge( std::string("/swapstatus \"")
				   + *p_swapId + "\" not accessible."
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

Ev::Io<std::unique_ptr<Ln::Amount>>
ServiceImpl::get_quotation(Ln::Amount offchainAmount) {
	return conn->api("/getpairs", nullptr
			).then([this, offchainAmount](Jsmn::Object res) {

		auto rate = double();
		auto fee_percent = double();
		auto fee_lockup = Ln::Amount();
		auto minimal = Ln::Amount();
		auto maximal = Ln::Amount();

		try {
			auto info = res["info"];
			for (auto i = std::size_t(0); i < info.size(); ++i) {
				auto inf = info[i];
				if (!inf.is_string())
					continue;
				if (std::string(inf) == "prepay.minerfee") {
					/* Not supported yet.  */
					return loge("prepay.minerfee needed "
						    "by server, "
						    "not supported by client."
						   ).then([]() {
						return Ev::lift(
							std::unique_ptr<Ln::Amount>()
						);
					});
				}
			}
			auto ws = res["warnings"];
			for (auto i = std::size_t(0); i < ws.size(); ++i) {
				auto w = ws[i];
				if (!w.is_string())
					continue;
				if (std::string(w) == "reverse.swaps.disabled") {
					return loge("reverse.swaps.disabled "
						    "by server."
						   ).then([]() {
						return Ev::lift(
							std::unique_ptr<Ln::Amount>()
						);
					});
				}
			}

			auto pair = res["pairs"]["BTC/BTC"];
			rate = (double) pair["rate"];
			minimal = Ln::Amount::sat(
				(double) pair["limits"]["minimal"]
			);
			maximal = Ln::Amount::sat(
				(double) pair["limits"]["maximal"]
			);
			fee_percent = (double) pair["fees"]["percentage"];
			fee_lockup = Ln::Amount::sat((double)
				pair
				["fees"]
				["minerFees"]
				["baseAsset"]
				["reverse"]
				["lockup"]
			);
		} catch (Jsmn::TypeError const& _) {
			auto os = std::ostringstream();
			os << "/getpairs unexpected result: " << res;
			return loge(os.str()).then([]() {
				return Ev::lift(std::unique_ptr<Ln::Amount>());
			});
		}

		/* Check range.  */
		if (minimal > offchainAmount || maximal < offchainAmount) {
			auto os = std::ostringstream();
			os << "/getpairs out-of-range "
			   << minimal.to_sat() << " < "
			   << offchainAmount.to_sat() << " < "
			   << maximal.to_sat()
			    ;
			return loge(os.str()).then([]() {
				return Ev::lift(std::unique_ptr<Ln::Amount>());
			});
		}

		/* Compute.  */
		auto onchainAmount = (offchainAmount * rate * 100)
				   / (100 + fee_percent)
				   ;
		if (onchainAmount > offchainAmount)
			onchainAmount = offchainAmount;
		if (onchainAmount > fee_lockup)
			onchainAmount -= fee_lockup;
		else
			onchainAmount = Ln::Amount::sat(0);
		return Ev::lift(Util::make_unique<Ln::Amount>(
			offchainAmount - onchainAmount
		));
	}).catching<ApiError>([this](ApiError const& e) {
		return logd("/getpairs not accessible.").then([]() {
			return Ev::lift(std::unique_ptr<Ln::Amount>());
		});
	});
}


}}
