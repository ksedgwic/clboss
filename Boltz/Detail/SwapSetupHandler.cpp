#include"Bitcoin/hash160.hpp"
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/SwapSetupHandler.hpp"
#include"Boltz/Detail/compute_preimage.hpp"
#include"Boltz/Detail/match_lockscript.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boltz/SwapInfo.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include<sstream>

namespace {

/* Thrown to get out and fail.  */
struct Fail {};

/* Timeout must be within this number of blocks from current block height.  */
auto const min_timeout = std::uint32_t(12);
auto const max_timeout = std::uint32_t(288);

}

namespace Boltz { namespace Detail {

Ev::Io<void> SwapSetupHandler::logd(std::string msg) {
	return env.logd(prefixlog(std::move(msg)));
}
Ev::Io<void> SwapSetupHandler::loge(std::string msg) {
	return env.loge(prefixlog(std::move(msg)));
}
std::string SwapSetupHandler::prefixlog(std::string msg) {
	auto prefix = std::string("Boltz::Service(\"")
		    + api_endpoint
		    + "\"): "
		    ;
	if (swapId)
		prefix += std::string("\"") + *swapId + "\": ";
	return prefix + msg;
}

Ev::Io<SwapInfo> SwapSetupHandler::run() {
	auto self = shared_from_this();
	return self->core_run().then([self]() {
		auto inf = SwapInfo
			{ std::move(self->invoice)
			, std::move(self->preimageHash)
			, self->timeoutBlockheight
			};
		return Ev::lift(std::move(inf));
	});
}

Ev::Io<void> SwapSetupHandler::core_run() {
	return Ev::lift().then([this]() {
		/* Set up.  */
		/* PubKey.  */
		tweakPubKey = signer.get_pubkey_tweak(tweak);
		/* The real preimage is the hash of the privkey plus
		 * the in-database preimage*/
		real_preimage = compute_preimage(signer, preimage);
		/* Compute hash of the real preimage.  */
		std::uint8_t buf[32];
		real_preimage.to_buffer(buf);
		Bitcoin::hash160( preimageHash160, preimageHash
				, buf, sizeof(buf)
				);

		auto params = Json::Out()
			.start_object()
				.field("from", "BTC")
				.field("to", "BTC")
				.field( "preimageHash"
				      , std::string(preimageHash)
				      )
				.field( "claimPublicKey"
				      , std::string(tweakPubKey)
				      )
				.field( "invoiceAmount"
				      , offchainAmount.to_sat()
				      )
			.end_object()
			;
		return conn.api( "/v2/swap/reverse"
			       , Util::make_unique<Json::Out>(
					std::move(params)
				 )
			       );
	}).then([this](Jsmn::Object res) {
		/* Parse result.  */
		auto tmp_swapId = std::string();
		auto tmp_s_claimScript = std::string();
		auto tmp_s_refundScript = std::string();
		auto tmp_refundPubkey = std::string();
		auto tmp_invoice = std::string();
		auto tmp_timeoutBlockheight = std::uint32_t();
		auto tmp_onchainAmount = std::uint64_t();
		try {
			tmp_swapId = (std::string) res["id"];
			tmp_s_claimScript = (std::string) res["swapTree"]["claimLeaf"]["output"];
			tmp_s_refundScript = (std::string) res["swapTree"]["refundLeaf"]["output"];
			tmp_refundPubkey = (std::string) res["refundPublicKey"];
			tmp_invoice = (std::string) res["invoice"];
			tmp_timeoutBlockheight = (std::uint32_t) (double) res["timeoutBlockHeight"];
			tmp_onchainAmount = (std::uint64_t) (double) res["onchainAmount"];
		} catch (Jsmn::TypeError const& e) {
			auto os = std::ostringstream();
			os << "Unexpected result from service: "
			   << res
			    ;
			return loge(os.str()).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		/* Validate claimScript is hex.   */
		if (!Util::Str::ishex(tmp_s_claimScript)) {
			return loge( std::string("invalid claimScript: ")
				   + tmp_s_claimScript
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		auto tmp_claimScript = Util::Str::hexread(tmp_s_claimScript);

		/* Validate refundScript is hex.   */
		if (!Util::Str::ishex(tmp_s_refundScript)) {
			return loge( std::string("invalid refundScript: ")
				   + tmp_s_refundScript
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		auto tmp_refundScript = Util::Str::hexread(tmp_s_refundScript);

		/* Validate the two taptree leaf scripts are correct.  */
		auto script_hash160 = Ripemd160::Hash();
		auto claim_pubkey = Secp256k1::XonlyPubKey();
		auto ok = match_claimscript( script_hash160
								   , claim_pubkey
								   , tmp_claimScript
								   );
		ok = ok && (script_hash160 == preimageHash160);

		if (!ok) {
			return loge( std::string("invalid claimScript: ")
				   + tmp_s_claimScript
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		auto script_locktime = std::uint32_t();
		auto refund_pubkey = Secp256k1::XonlyPubKey();
		ok = match_refundscript( script_locktime
							   , refund_pubkey
							   , tmp_refundScript
							   );
		ok = ok && (script_locktime == tmp_timeoutBlockheight);

		if (!ok) {
			return loge( std::string("invalid refundScript: ")
				   + tmp_s_refundScript
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}

		/* FIXME: We should check the latest quotation!  */
		if ( (Ln::Amount::sat(tmp_onchainAmount) / offchainAmount)
		   < 0.90
		   ) {
			auto os = std::ostringstream();
			os << "Onchain amount "
			   << tmp_onchainAmount
			   << " too low compared to offchain amount "
			   << offchainAmount
			    ;
			return loge(os.str()).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}

		/* Check we have enough time.  */
		if ( current_blockheight + min_timeout > tmp_timeoutBlockheight
		  || current_blockheight + max_timeout < tmp_timeoutBlockheight
		   ) {
			auto os = std::ostringstream();
			os << "Timeout " << tmp_timeoutBlockheight << " "
			   << "is not within "
			   << min_timeout << " -> " << max_timeout << " "
			   << "blocks of current height "
			   << current_blockheight << "."
			    ;
			return loge(os.str()).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}

		/* Validation is okay!  Save the data into the object.  */
		swapId = Util::make_unique<std::string>(std::move(tmp_swapId));
		claimScript = std::move(tmp_claimScript);
		refundScript = std::move(tmp_refundScript);
		refundPubkey = std::move(tmp_refundPubkey);
		timeoutBlockheight = std::move(tmp_timeoutBlockheight);
		onchainAmount = Ln::Amount::sat(tmp_onchainAmount);
		invoice = std::move(tmp_invoice);

		return Ev::lift();
	}).then([this]() {

		/* Now save to DB.  */
		return db.transact();
	}).then([this](Sqlite3::Tx tx) {
		/* First check the swapId does not exist yet.  */
		auto check = tx.query(R"QRY(
		SELECT swapId FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = :swapId
		)QRY")
			.bind(":apiAccess", api_endpoint)
			.bind(":swapId", *swapId)
			.execute()
			;
		auto found = false;
		for (auto& r : check) {
			(void) r;
			found = true;
		}
		/* Unexpected duplicate key!  */
		if (found) {
			return loge("id already exists??").then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		/* Insert it.  */
		tx.query(R"QRY(
		INSERT INTO "BoltzServiceFactory_rsub"
		     ( apiAccess
		     , tweak
		     , preimage
		     , destinationAddress
		     , swapId
		     , claimScript
		     , refundScript
			 , refundPubkey
		     , timeoutBlockheight
		     , onchainAmount
		     , lockedUp
		     , comment
		     )
		VALUES
		     ( :apiAccess
		     , :tweak
		     , :preimage
		     , :destinationAddress
		     , :swapId
		     , :claimScript
		     , :refundScript
			 , :refundPubkey
		     , :timeoutBlockheight
		     , :onchainAmount
		     , :lockedUp
		     , :comment
		     );
		)QRY")
			.bind(":apiAccess", api_endpoint)
			.bind(":tweak", std::string(tweak))
			.bind(":preimage", std::string(preimage))
			.bind(":destinationAddress", destinationAddress)
			.bind(":swapId", *swapId)
			.bind(":claimScript"
			     , Util::Str::hexdump( &claimScript[0]
						 , claimScript.size()
						 )
			     )
			.bind(":refundScript"
			     , Util::Str::hexdump( &refundScript[0]
						 , refundScript.size()
						 )
			     )
			.bind(":refundPubkey", refundPubkey)
			.bind(":timeoutBlockheight", timeoutBlockheight)
			.bind(":onchainAmount", onchainAmount.to_sat())
			.bind(":lockedUp", 0)
			.bind(":comment", "")
			.execute()
			;

		tx.commit();

		return logd(std::string("Swap created: ") + invoice);
	}).catching<Fail>([this](Fail const& _) {
		invoice = "";
		timeoutBlockheight = 0;
		return Ev::lift();
	}).catching<Boltz::ApiError>([this](Boltz::ApiError const& _){
		invoice = "";
		timeoutBlockheight = 0;
		return loge("Failed to access API endpoint.");
	});
}

}}
