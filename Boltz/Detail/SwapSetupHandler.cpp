#include"Bitcoin/addr_to_scriptPubKey.hpp"
#include"Bitcoin/hash160.hpp"
#include"Bitcoin/pubkey_to_scriptPubKey.hpp"
#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/MusigImpl.hpp"
#include"Boltz/Detail/SwapSetupHandler.hpp"
#include"Boltz/Detail/compute_preimage.hpp"
#include"Boltz/Detail/reverse_terms.hpp"
#include"Boltz/Detail/swaptree.hpp"
#include"Boltz/EnvIF.hpp"
#include"Boltz/SwapInfo.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include<sstream>

namespace {

/* Thrown to get out and fail.  */
struct Fail {};

/* Timeout must be within this number of blocks from current block
 * height.  The minimum leaves room for the claim: the lockup takes
 * a few blocks to confirm, the claim handler waits for it to be
 * three deep, and it does not start a claim within ten blocks of
 * the timeout.  */
auto const min_timeout = std::uint32_t(24);
auto const max_timeout = std::uint32_t(288);

/* The tapscript leaf version (BIP 342).  The version is part of
 * the leaf hash, so a tree with another version pays to another
 * key than the one we compute.  */
auto const tapscript_leaf_version = std::uint32_t(192);

/* How far below the quoted on-chain amount a created swap may
 * come: a quarter of the quoted fee, or this much, whichever is
 * more.  The provider's miner fee estimate may move between the
 * terms and the swap.  */
auto const fee_tolerance_floor = Ln::Amount::sat(1500);
auto const fee_tolerance_ratio = double(0.25);

}

namespace Boltz { namespace Detail {

Ev::Io<void> SwapSetupHandler::logd(std::string msg) {
	return env.logd(prefixlog(std::move(msg)));
}
Ev::Io<void> SwapSetupHandler::logi(std::string msg) {
	return env.logi(prefixlog(std::move(msg)));
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
		/* The provider's current terms, which the created
		 * swap is held to below.  */
		return conn.api("/v2/swap/reverse", nullptr);
	}).then([this](Jsmn::Object res) {
		auto terms = ReverseTerms();
		try {
			terms = parse_reverse_terms(res);
		} catch (Jsmn::TypeError const& _) {
			auto os = std::ostringstream();
			os << "Unexpected terms from service: " << res;
			return loge(os.str()).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		quotedOnchainAmount = quoted_onchain_amount
			( terms
			, offchainAmount
			);
		return Ev::lift();
	}).then([this]() {
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
		auto tmp_claimLeafVersion = std::uint32_t();
		auto tmp_refundLeafVersion = std::uint32_t();
		auto tmp_refundPubkey = std::string();
		auto tmp_lockupAddress = std::string();
		auto tmp_invoice = std::string();
		auto tmp_timeoutBlockheight = std::uint32_t();
		auto tmp_onchainAmount = std::uint64_t();
		try {
			tmp_swapId = (std::string) res["id"];
			tmp_s_claimScript = (std::string) res["swapTree"]["claimLeaf"]["output"];
			tmp_s_refundScript = (std::string) res["swapTree"]["refundLeaf"]["output"];
			tmp_claimLeafVersion = (std::uint32_t) (double) res["swapTree"]["claimLeaf"]["version"];
			tmp_refundLeafVersion = (std::uint32_t) (double) res["swapTree"]["refundLeaf"]["version"];
			tmp_refundPubkey = (std::string) res["refundPublicKey"];
			tmp_lockupAddress = (std::string) res["lockupAddress"];
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

		/* Both leaves must carry the tapscript version our tree
		 * is built with.  */
		if ( tmp_claimLeafVersion != tapscript_leaf_version
		  || tmp_refundLeafVersion != tapscript_leaf_version
		   ) {
			auto os = std::ostringstream();
			os << "swapTree leaf versions "
			   << tmp_claimLeafVersion << " and "
			   << tmp_refundLeafVersion
			   << " are not " << tapscript_leaf_version
			    ;
			return loge(os.str()).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}

		/* The two leaves must be, byte for byte, the ones we
		 * would write from what we know: our key and preimage
		 * hash in the claim leaf, so that the unilateral claim is
		 * ours and nobody else's; the provider's declared refund
		 * key and the swap's timeout in the refund leaf, the same
		 * key the claim later aggregates with ours.  Anything
		 * else, a different key, another hash, another encoding
		 * of the height, is a tree we did not agree to, and the
		 * swap is not paid for.  */
		auto our_claimScript = claim_leaf
			( preimageHash160
			, Secp256k1::XonlyPubKey::from_ecdsa_pk(tweakPubKey)
			);
		if (tmp_claimScript != our_claimScript) {
			return loge( std::string("claimLeaf ")
				   + tmp_s_claimScript
				   + " is not the leaf for our key and "
				     "preimage hash, "
				   + Util::Str::hexdump( &our_claimScript[0]
						       , our_claimScript.size()
						       )
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		/* Validate refundPublicKey is a 33-byte key in hex.
		 * The key parser fails anything else with a hex
		 * error, not InvalidPubKey.  */
		if ( !Util::Str::ishex(tmp_refundPubkey)
		  || tmp_refundPubkey.size() != 66
		   ) {
			return loge( std::string("invalid refundPublicKey: ")
				   + tmp_refundPubkey
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		auto provider_pubkey = Secp256k1::PubKey();
		try {
			provider_pubkey = Secp256k1::PubKey(tmp_refundPubkey);
		} catch (Secp256k1::InvalidPubKey const& _) {
			return loge( std::string("invalid refundPublicKey: ")
				   + tmp_refundPubkey
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}
		auto our_refundScript = refund_leaf
			( Secp256k1::XonlyPubKey::from_ecdsa_pk(provider_pubkey)
			, tmp_timeoutBlockheight
			);
		if (tmp_refundScript != our_refundScript) {
			return loge( std::string("refundLeaf ")
				   + tmp_s_refundScript
				   + " is not the leaf for refundPublicKey "
				   + tmp_refundPubkey
				   + " and timeout "
				   + std::to_string(tmp_timeoutBlockheight)
				   + ", "
				   + Util::Str::hexdump( &our_refundScript[0]
						       , our_refundScript.size()
						       )
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}

		/* The lockup address must pay to the key the claim will
		 * compute: the provider's key and ours aggregated,
		 * tweaked by the root of the two leaves.  Checked here,
		 * before the invoice is paid, so a tree or key
		 * disagreement costs nothing; found at claim time it
		 * would leave the payment pending until the invoice
		 * expires.  */
		auto expected_spk = std::vector<std::uint8_t>();
		auto lockup_spk = std::vector<std::uint8_t>();
		auto key_error = std::string();
		try {
			auto session = Boltz::MusigSession( tweak
							  , provider_pubkey
							  , signer
							  );
			session.apply_xonly_tweak(compute_root_hash
				( tmp_claimScript
				, tmp_refundScript
				));
			expected_spk = Bitcoin::pk_to_scriptpk(
				session.get_output_key()
			);
			lockup_spk = Bitcoin::addr_to_scriptPubKey(
				tmp_lockupAddress
			);
		} catch (Secp256k1::Musig::InvalidArg const& e) {
			key_error = e.what();
		} catch (Bitcoin::UnknownAddrType const& e) {
			key_error = e.what();
		}
		if (!key_error.empty() || expected_spk != lockup_spk) {
			return loge( std::string("lockupAddress ")
				   + tmp_lockupAddress
				   + " does not pay to the swap tree's key"
				   + (key_error.empty() ? "" : ": " + key_error)
				   ).then([]() {
				throw Fail();
				return Ev::lift();
			});
		}

		/* The swap must hold to the terms read just before it
		 * was created: the on-chain amount they promise for this
		 * invoice amount, less a tolerance for a miner fee
		 * estimate that moved in between.  */
		auto tolerance = (offchainAmount - quotedOnchainAmount)
			       * fee_tolerance_ratio
			       ;
		if (tolerance < fee_tolerance_floor)
			tolerance = fee_tolerance_floor;
		if ( Ln::Amount::sat(tmp_onchainAmount) + tolerance
		   < quotedOnchainAmount
		   ) {
			auto os = std::ostringstream();
			os << "Onchain amount " << tmp_onchainAmount
			   << " is below the quoted "
			   << quotedOnchainAmount.to_sat()
			   << " by more than " << tolerance.to_sat()
			   << " for offchain amount "
			   << offchainAmount.to_sat()
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
		lockupAddress = std::move(tmp_lockupAddress);

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
		     -- The API v1 column, NOT NULL without a default
		     -- in databases from before v2.
		     , redeemScript
		     , claimScript
		     , refundScript
		     , refundPubkey
		     , timeoutBlockheight
		     , onchainAmount
		     , createdHeight
		     , lockedUp
		     , comment
		     )
		VALUES
		     ( :apiAccess
		     , :tweak
		     , :preimage
		     , :destinationAddress
		     , :swapId
		     , ''
		     , :claimScript
		     , :refundScript
		     , :refundPubkey
		     , :timeoutBlockheight
		     , :onchainAmount
		     , :createdHeight
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
			.bind(":createdHeight", current_blockheight)
			.bind(":lockedUp", 0)
			.bind(":comment", "")
			.execute()
			;

		tx.commit();

		auto os = std::ostringstream();
		os << "Swap created: " << offchainAmount.to_sat()
		   << " sat sent for " << onchainAmount.to_sat()
		   << " sat on chain (quoted "
		   << quotedOnchainAmount.to_sat()
		   << "), lockup " << lockupAddress
		   << ", timeout " << timeoutBlockheight << "."
		    ;
		return logi(os.str()).then([this]() {
			return logd(std::string("Invoice: ") + invoice);
		});
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
