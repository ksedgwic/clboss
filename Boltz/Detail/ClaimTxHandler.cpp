#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/ClaimTxHandler.hpp"
#include"Boltz/Detail/compute_preimage.hpp"
#include"Boltz/Detail/find_lockup_outnum.hpp"
#include"Boltz/Detail/initial_claim_tx.hpp"
#include"Boltz/Detail/swaptree.hpp"
#include"Boltz/EnvIF.hpp"
#include"Bitcoin/pubkey_to_scriptPubKey.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/TapscriptTree.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include<string.h>
#include<sstream>

namespace {

/* Throw to early-out of the run.  */
struct End { };

}

using namespace Secp256k1;

namespace Boltz { namespace Detail {

std::string ClaimTxHandler::logprefix(std::string msg) {
	return std::string("Boltz::Service(\"") + api_endpoint + "\"): "
	     + " \"" + swapId + "\": "
	     + msg
	     ;
}
Ev::Io<void> ClaimTxHandler::logd(std::string msg) {
	return env.logd(logprefix(msg));
}
Ev::Io<void> ClaimTxHandler::loge(std::string msg) {
	return env.loge(logprefix(msg));
}

Ev::Io<void> ClaimTxHandler::run() {
	auto self = shared_from_this();
	/* Keep self alive until the run ends.  */
	return Ev::lift().then([self]() {
		return self->core_run();
	}).then([self]() {
		return Ev::lift();
	});
}

Ev::Io<void> ClaimTxHandler::core_run() {
	return Ev::lift().then([this]() {

		/* Fetch data from database.  */
		return db.transact();
	}).then([this](Sqlite3::Tx tx) {
		/* First do a quick check.  */
		auto check1 = tx.query(R"QRY(
		SELECT lockedUp, timeoutBlockheight, destinationAddress
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = :swapId
		     ;
		)QRY")
			.bind(":apiAccess", api_endpoint)
			.bind(":swapId", swapId)
			.execute();
		auto found = 0;
		auto lockedUp = false;
		for (auto& r : check1) {
			++found;
			lockedUp = r.get<bool>(0);
			timeoutBlockheight = r.get<std::uint32_t>(1);
			destinationAddress = r.get<std::string>(2);
			break;
		}
		if (found == 0) {
			/* Unexpected not found!  */
			return loge("Swap ID not in table.").then([]() {
				throw End();
				return Ev::lift();
			});
		}
		if (found > 1) {
			/* Unexpected duplicate!  */
			return loge("Swap ID duplicated??").then([]() {
				throw End();
				return Ev::lift();
			});
		}
		if (lockedUp) {
			/* TODO: Bump up instead of doing nothing.  */
			return logd( "Already broadcasted claim tx."
				   ).then([]() {
				throw End();
				return Ev::lift();
			});
		}
		if (destinationAddress.empty()) {
			return loge("Swap destination address is empty??"
				   ).then([]() {
				throw End();
				return Ev::lift();
			});
		}
		// TODO: Check for validity of destination address?

		auto claimScript = std::vector<std::uint8_t>();
		auto refundScript = std::vector<std::uint8_t>();
		/* Perform the actual fetch of the data.  */
		auto fetch = tx.query(R"QRY(
		SELECT tweak -- 0
		     , preimage -- 1
		     , destinationAddress -- 2
		     , claimScript -- 3
		     , refundScript -- 4
		     , timeoutBlockheight -- 5
		     , onchainAmount -- 6
		     , refundPubKey -- 7
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = :swapId
		     ;
		)QRY")
			.bind(":apiAccess", api_endpoint)
			.bind(":swapId", swapId)
			.execute();
		for (auto& r : fetch) {
			tweak = Secp256k1::PrivKey(r.get<std::string>(0));
			preimage = Ln::Preimage(r.get<std::string>(1));
			destinationAddress = r.get<std::string>(2);
			claimScript = Util::Str::hexread(
				r.get<std::string>(3)
			);
			refundScript = Util::Str::hexread(
				r.get<std::string>(4)
			);
			timeoutBlockheight = r.get<std::uint32_t>(5);
			onchainAmount = Ln::Amount::sat(
				r.get<std::uint64_t>(6)
			);
			refund_pubkey = Secp256k1::PubKey(r.get<std::string>(7));
		}
		tx.commit();

		/* Is the timeout too near?  */
		if (timeoutBlockheight <= blockheight + 10) {
			auto os = std::ostringstream();
			os << "Timeout " << timeoutBlockheight
			   << " too near to current time "
			   << blockheight
			    ;
			return logd(os.str()).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		/* create aggregate pubkey
		 * role ordered: boltz key #0, our key #1.  */
		try {
			musigsession = Boltz::MusigSession(tweak, refund_pubkey, signer);
		} catch (Secp256k1::InvalidPubKey const& e) {
			return loge(e.what()).then([]() {
				throw End();
				return Ev::lift();
			});
		} catch (Musig::InvalidArg const& e) {
			return loge(e.what()).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		/* compute script hash (represented as an x-only pubkey) that constitutes
		 * the redeem script for signing the cooperative (key) path of the swap. */
		auto roothash = compute_root_hash(claimScript, refundScript);

		try {
			/* apply (add to generator point, muliply by the internal key) the
			 * taptweak hash to the aggregate (internal) key.	*/
			musigsession.apply_xonly_tweak(roothash);
			redeemScript = Bitcoin::pk_to_scriptpk(musigsession.get_output_key());
		} catch (Musig::InvalidArg const& e) {
			return loge(e.what()).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		/* Find the outnum from the lockup_tx.  */
		auto outnum = Detail::find_lockup_outnum( lockup_tx
							, redeemScript
							);
		if (outnum < 0) {
			auto msg = std::string("Service lockup tx ")
				 + std::string(lockup_tx) + " "
				 + "does not pay to lockscript."
				 ;
			return loge(msg).then([]() {
				throw End();
				return Ev::lift();
			});
		}
		lockupOut = std::size_t(outnum);

		auto const& lockupoutput = lockup_tx.outputs[lockupOut];

		/* compare locally computed lockscript hash with what Boltz expects.  */
		if (memcmp(redeemScript.data(), lockupoutput.scriptPubKey.data() +2, 32) != 0) {
			auto msg = std::string("Unexpected lockscript received from Boltz");
			return loge(msg).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		/* Check the onchain amount is correct.  */
		if (lockupoutput.amount != onchainAmount) {
			auto msg = std::string("Service lockup tx ")
				 + std::string(lockup_tx) + " "
				 + "does not pay expected amount."
				 ;
			return loge(msg).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		/* Now compute the real preimage from the signer key
		 * and the in-database preimage.  */

		real_preimage = Detail::compute_preimage(signer, preimage);

		return Ev::lift();
	}).then([this]() {

		/* Now look up the feerate.  */
		return env.get_feerate();
	}).then([this](std::uint32_t feerate) {
		/* Generate the claim tx.  */
		auto sighash = Detail::initial_claim_tx( claim_tx
						, lockupClaimFees

						, feerate
						, blockheight
						, lockup_txid
						, lockupOut
						, onchainAmount

						, redeemScript

						, destinationAddress
						);

		try {
			musigsession.load_sighash(sighash);

			/* Create our nonce pair for the musig session.  */
			local_pubnonce = musigsession.generate_local_nonces(random);
		} catch (Musig::InvalidArg const& e) {
			return loge(e.what()).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		return Ev::lift();
	}).then([this]() {
		/* send preimage, public nonce and the unsigned claim tx to Boltz.  */
		auto params = Json::Out()
			.start_object()
				.field( "preimage"
				      , std::string(preimage)
					  )
				.field( "pubNonce"
				      , local_pubnonce
					  )
				.field( "transaction"
				      , std::string(claim_tx)
				      )
				.field( "index", 0)
			.end_object()
			;

		return conn.api( "/v2/swap/reverse/" + swapId + "/claim"
					   , Util::make_unique<Json::Out>(
							std::move(params)
					     )
					   );
	}).then([this](Jsmn::Object res) {
		/* Parse result.  */
		std::string boltzpubnonce;
		std::string boltzpartialsig;

		/* we receive from Boltz their partial sig and public nonce in reply.	*/
		try {
			boltzpubnonce = (std::string) res["pubNonce"];
			boltzpartialsig = (std::string) res["partialSignature"];
		} catch (Jsmn::TypeError const& e) {
			auto os = std::ostringstream();
			os << "Unexpected result from service: "
			   << res
			    ;
			return loge(os.str()).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		/* Validate pubnonce from Boltz is hex.   */
		if (!Util::Str::ishex(boltzpubnonce)) {
			return loge( std::string("invalid pubnonce from boltz: ")
				   + boltzpubnonce
				   ).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		/* Validate partial sig from Boltz is hex.   */
		if (!Util::Str::ishex(boltzpartialsig)) {
			return loge( std::string("invalid partialsig from boltz: ")
				   + boltzpartialsig
				   ).then([]() {
				throw End();
				return Ev::lift();
			});
		}

		std::uint8_t aggsig_buffer[64];
		auto aggregatesig = Secp256k1::SchnorrSig();
		try {
			/* aggregate our public nonce with Boltz's, initiate session.  */
			musigsession.load_pubnonce(boltzpubnonce);
			musigsession.aggregate_pubnonces();

			/* sign our partial signature and combine into a single sig.  */
			musigsession.load_partial(boltzpartialsig);
			musigsession.sign_partial();
			musigsession.verify_part_sigs();
			musigsession.aggregate_partials(aggsig_buffer);
			aggregatesig = SchnorrSig::from_buffer(aggsig_buffer);
			if (!aggregatesig.valid( musigsession.get_output_key()
								   , musigsession.get_sighash() )) {
				return loge( std::string("invalid aggsig for given sighash and output key")
					   ).then([]() {
					throw End();
					return Ev::lift();
				});
			}
		} catch (Musig::InvalidArg const& e) {
			return loge(e.what()).then([]() {
				throw End();
				return Ev::lift();
			});
		} catch (Secp256k1::InvalidPrivKey const& e) {
			return loge(e.what()).then([]() {
				throw End();
				return Ev::lift();
			});
		}
		affix_aggregated_signature( claim_tx
								  , real_preimage
								  , redeemScript
								  , aggregatesig.to_buffer()
								  );
		/* Log it.  */
		auto msg = std::string("Broadcasting claim tx: ")
			 + std::string(claim_tx)
			 ;
		return logd(msg);
	}).then([this]() {

		/* Do the broadcast.  */
		return env.broadcast_tx(claim_tx);
	}).then([this](bool success) {
		if (!success) {
			auto msg = std::string("Broadcast failed! ")
				 + std::string(claim_tx)
				 ;
			return loge(msg).then([]() {
				throw End();
				return Ev::lift();
			});
		}
		return Ev::lift();
	}).then([this]() {

		/* Update the db.  */
		return db.transact();
	}).then([this](Sqlite3::Tx tx) {
		tx.query(R"QRY(
		UPDATE "BoltzServiceFactory_rsub"
		   SET lockedUp = 1
		     , lockupTxid = :lockup_txid
		     , lockupOut = :lockupOut
		     , lockupConfirmedHeight = :blockheight
		     , lockupClaimFees = :lockupClaimFees
		     ;
		)QRY")
			.bind(":lockup_txid", std::string(lockup_txid))
			.bind(":lockupOut", lockupOut)
			.bind(":blockheight", blockheight)
			.bind(":lockupClaimFees", lockupClaimFees.to_sat())
			.execute();
		tx.commit();

		return Ev::lift();
	}).catching<End>([](End const&) {
		/* Lambda, the ultimate Swiss Army Knife.... */
		return Ev::lift();
	});
}

}}
