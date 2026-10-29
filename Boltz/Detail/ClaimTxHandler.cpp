#include"Boltz/ConnectionIF.hpp"
#include"Boltz/Detail/ClaimTxHandler.hpp"
#include"Boltz/Detail/compute_preimage.hpp"
#include"Boltz/Detail/find_lockup_outnum.hpp"
#include"Boltz/Detail/initial_claim_tx.hpp"
#include"Boltz/Detail/swaptree.hpp"
#include"Boltz/EnvIF.hpp"
#include"Bitcoin/Block.hpp"
#include"Bitcoin/addr_to_scriptPubKey.hpp"
#include"Bitcoin/pubkey_to_scriptPubKey.hpp"
#include"Ev/Io.hpp"
#include"Ev/coroutine.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Secp256k1/KeyPair.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/TapscriptTree.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include<sstream>
#include<stdexcept>

namespace {

/* Throw to early-out of the run.  */
struct End { };

/* The lockup must be this deep in our own node's chain before
 * the preimage leaves the node.  The provider's report that it
 * confirmed is not enough: with the preimage it can settle our
 * payment, so the lockup must exist first, and deep enough that
 * a reorganization cannot take it back; the same depth
 * OnchainFundsAnnouncer asks of our own funds.  */
auto const lockup_confirmations = std::uint32_t(3);
/* A row without a creation height is searched from this far
 * before its timeout, the widest window the setup accepts.  */
auto const search_floor_blocks = std::uint32_t(288);
/* The search starts this many blocks below the swap's creation
 * height and, while the lockup is unfound, resumes this many
 * blocks below where it stopped, so that a reorganization no
 * deeper than this cannot move the lockup out of its sight.  */
auto const search_margin_blocks = std::uint32_t(6);
/* A revealed swap whose lockup is still unspent this many blocks
 * after its timeout is given up: the same horizon the Swapper
 * modules write a swap off at.  */
auto const give_up_blocks = std::uint32_t(432);

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
	}).catching<End>([self](End const&) {
		return Ev::lift();
	});
}

Ev::Io<void> ClaimTxHandler::core_run() {
	co_await load_row();

	if (preimageRevealed) {
		/* The provider may already hold the preimage, so from
		 * here on its status is advisory and the claim is
		 * tracked from our own node.  */
		co_await track_claim();
		co_return;
	}

	/* Too near the timeout to start a claim: it would race the
	 * provider's refund.  Nothing has been revealed, so the swap
	 * is left to end at its timeout with the payment failing back.
	 * This repeats each block until the row goes.  */
	if (timeoutBlockheight <= blockheight + 10) {
		auto os = std::ostringstream();
		os << "Timeout " << timeoutBlockheight
		   << " is within 10 blocks of height " << blockheight
		   << "; not claiming, the swap ends at its timeout."
		    ;
		co_await logd(os.str());
		throw End();
	}

	co_await prepare_keys();

	/* Find the outnum from the lockup_tx.  */
	auto outnum = Detail::find_lockup_outnum(lockup_tx, redeemScript);
	if (outnum < 0) {
		co_await loge( std::string("Service lockup tx ")
			     + std::string(lockup_tx) + " "
			     + "does not pay to lockscript."
			     );
		throw End();
	}
	lockupOut = std::size_t(outnum);

	/* Check the onchain amount is correct.  */
	if (lockup_tx.outputs[lockupOut].amount != onchainAmount) {
		co_await loge( std::string("Service lockup tx ")
			     + std::string(lockup_tx) + " "
			     + "does not pay expected amount."
			     );
		throw End();
	}

	/* The provider reports the lockup confirmed.  The preimage
	 * leaves this node only once our own node has the lockup in
	 * a block.  */
	co_await confirm_lockup();

	/* Build the claim before anything is revealed: a claim the
	 * fee would consume waits for lower fees instead.  */
	auto feerate = co_await env.get_feerate();
	auto sighash = co_await build_claim(feerate, ClaimPath::KeyPath);
	if (!sighash)
		throw End();

	/* From the next step on the provider can settle our payment,
	 * so the row says so first: whatever stops the claim after
	 * this, a crash, a refusal, a rejected broadcast, is retried
	 * at each block until our node sees the claim confirmed.  */
	co_await persist_reveal();

	co_await claim(*sighash, feerate);
}

Ev::Io<void> ClaimTxHandler::load_row() {
	auto tx = co_await db.transact();
	auto fetch = tx.query(R"QRY(
	SELECT tweak -- 0
	     , preimage -- 1
	     , destinationAddress -- 2
	     , claimScript -- 3
	     , refundScript -- 4
	     , timeoutBlockheight -- 5
	     , onchainAmount -- 6
	     , refundPubKey -- 7
	     , createdHeight -- 8
	     , COALESCE(lockupConfirmedHeight, 0) -- 9
	     , COALESCE(lockupSearchedHeight, 0) -- 10
	     , preimageRevealed -- 11
	     , lockupTx -- 12
	     , COALESCE(lockupOut, 0) -- 13
	     , claimTx -- 14
	     , claimFeerate -- 15
	     , COALESCE(spendSearchedHeight, 0) -- 16
	     , COALESCE(lockupClaimFees, 0) -- 17
	  FROM "BoltzServiceFactory_rsub"
	 WHERE apiAccess = :apiAccess
	   AND swapId = :swapId
	     ;
	)QRY")
		.bind(":apiAccess", api_endpoint)
		.bind(":swapId", swapId)
		.execute();
	auto found = 0;
	auto db_lockupOut = std::uint32_t(0);
	auto refund_pubkey_hex = std::string();
	for (auto& r : fetch) {
		++found;
		tweak = Secp256k1::PrivKey(r.get<std::string>(0));
		preimage = Ln::Preimage(r.get<std::string>(1));
		destinationAddress = r.get<std::string>(2);
		claimScript = Util::Str::hexread(r.get<std::string>(3));
		refundScript = Util::Str::hexread(r.get<std::string>(4));
		timeoutBlockheight = r.get<std::uint32_t>(5);
		onchainAmount = Ln::Amount::sat(r.get<std::uint64_t>(6));
		refund_pubkey_hex = r.get<std::string>(7);
		createdHeight = r.get<std::uint32_t>(8);
		lockupConfirmedHeight = r.get<std::uint32_t>(9);
		lockupSearchedHeight = r.get<std::uint32_t>(10);
		preimageRevealed = r.get<bool>(11);
		stored_lockup_tx = r.get<std::string>(12);
		db_lockupOut = r.get<std::uint32_t>(13);
		claimTxHex = r.get<std::string>(14);
		claimFeerate = r.get<std::uint32_t>(15);
		spendSearchedHeight = r.get<std::uint32_t>(16);
		lockupClaimFees = Ln::Amount::sat(r.get<std::uint64_t>(17));
	}
	tx.commit();

	if (found == 0) {
		co_await loge("Swap ID not in table.");
		throw End();
	}
	if (found > 1) {
		co_await loge("Swap ID duplicated??");
		throw End();
	}
	if (destinationAddress.empty()) {
		co_await loge("Swap destination address is empty??");
		throw End();
	}

	/* A row from the API v1 code has no tapscript leaves, and
	 * the v1 claim went with that code: the swap ends at its
	 * timeout, with the payment failing back to us.  */
	if (claimScript.empty() || refundScript.empty()) {
		co_await loge( "Swap predates API v2 and cannot be "
			       "claimed by this version; it ends at "
			       "its timeout."
			     );
		throw End();
	}
	/* A v1 row has no refund key either, so it is parsed only
	 * past the check above.  */
	refund_pubkey = Secp256k1::PubKey(refund_pubkey_hex);

	/* A revealed swap works from the lockup recorded at the
	 * reveal, not from whatever the provider reports now.  */
	if (preimageRevealed) {
		if (!stored_lockup_tx.empty()) {
			auto error = std::string();
			try {
				lockup_tx = Bitcoin::Tx(stored_lockup_tx);
				lockup_txid = lockup_tx.get_txid();
			} catch (std::exception const& e) {
				error = e.what();
			}
			if (!error.empty()) {
				co_await loge( "Stored lockup transaction did "
					       "not parse: " + error
					     );
				throw End();
			}
		}
		lockupOut = db_lockupOut;
	}
}

Ev::Io<void> ClaimTxHandler::prepare_keys() {
	/* The aggregate key, provider's key first, ours second as
	 * the API v2 orders them, tweaked by the tree root, is the
	 * lockup's output key.  */
	auto error = std::string();
	try {
		musigsession = Boltz::MusigSession(tweak, refund_pubkey, signer);
		auto roothash = compute_root_hash(claimScript, refundScript);
		musigsession.apply_xonly_tweak(roothash);
		redeemScript = Bitcoin::pk_to_scriptpk(musigsession.get_output_key());
	} catch (Secp256k1::InvalidPubKey const& e) {
		error = e.what();
	} catch (Musig::InvalidArg const& e) {
		error = e.what();
	}
	if (!error.empty()) {
		co_await loge(error);
		throw End();
	}
}

Ev::Io<void> ClaimTxHandler::confirm_lockup() {
	if (lockupConfirmedHeight == 0) {
		/* The lockup was mined after the swap was created, so
		 * the search starts at the creation height less the
		 * reorganization margin, and resumes that margin below
		 * where the last search stopped.  */
		auto floor = std::uint32_t(1);
		if (createdHeight > search_margin_blocks)
			floor = createdHeight - search_margin_blocks;
		else if (createdHeight == 0
		      && timeoutBlockheight > search_floor_blocks)
			floor = timeoutBlockheight - search_floor_blocks;
		auto from = floor;
		if (lockupSearchedHeight > search_margin_blocks + floor)
			from = lockupSearchedHeight + 1 - search_margin_blocks;

		for (auto h = from; h <= blockheight; ++h) {
			auto hex = co_await env.get_block(h);
			if (hex.empty())
				/* Our node does not have that block yet;
				 * search again at the next block.  */
				break;
			auto block = Bitcoin::Block();
			auto parsed = true;
			auto parse_error = std::string();
			try {
				block = Bitcoin::Block(hex);
			} catch (std::exception const& e) {
				parsed = false;
				parse_error = e.what();
			}
			if (!parsed) {
				co_await loge( std::string("Block ")
					     + std::to_string(h)
					     + " did not parse: "
					     + parse_error
					     );
				break;
			}
			lockupSearchedHeight = h;
			if (block.contains(lockup_txid)) {
				lockupConfirmedHeight = h;
				break;
			}
		}

		auto tx = co_await db.transact();
		tx.query(R"QRY(
		UPDATE "BoltzServiceFactory_rsub"
		   SET lockupSearchedHeight = NULLIF(:searched, 0)
		     , lockupConfirmedHeight = NULLIF(:confirmed, 0)
		 WHERE apiAccess = :apiAccess
		   AND swapId = :swapId
		     ;
		)QRY")
			.bind(":searched", lockupSearchedHeight)
			.bind(":confirmed", lockupConfirmedHeight)
			.bind(":apiAccess", api_endpoint)
			.bind(":swapId", swapId)
			.execute();
		tx.commit();

		if (lockupConfirmedHeight == 0) {
			co_await logd( std::string("Lockup transaction ")
				     + std::string(lockup_txid)
				     + " is not in a block up to height "
				     + std::to_string(lockupSearchedHeight)
				     + "; not claiming yet."
				     );
			throw End();
		}
		co_await logd( std::string("Lockup transaction ")
			     + std::string(lockup_txid)
			     + " is in block "
			     + std::to_string(lockupConfirmedHeight)
			     + "."
			     );
	}

	auto depth = blockheight >= lockupConfirmedHeight
		   ? blockheight + 1 - lockupConfirmedHeight
		   : std::uint32_t(0)
		   ;
	if (depth < lockup_confirmations) {
		co_await logd( std::string("Lockup has ")
			     + std::to_string(depth)
			     + " of "
			     + std::to_string(lockup_confirmations)
			     + " confirmations; not claiming yet."
			     );
		throw End();
	}
}

Ev::Io<std::optional<Sha256::Hash>>
ClaimTxHandler::build_claim( std::uint32_t feerate
			   , ClaimPath path
			   ) {
	auto sighash = Detail::initial_claim_tx( claim_tx
					       , lockupClaimFees
					       , feerate
					       , blockheight
					       , lockup_txid
					       , lockupOut
					       , onchainAmount
					       , redeemScript
					       , destinationAddress
					       , path
					       , claimScript
					       );
	if (claim_tx.outputs[0].amount == Ln::Amount::sat(0)) {
		/* The fee would consume the lockup.  Nothing is
		 * signed; fees may fall before the timeout.  */
		co_await logd( std::string("Claim fee ")
			     + std::string(lockupClaimFees)
			     + " at " + std::to_string(feerate)
			     + " perkw would consume the "
			     + std::string(onchainAmount)
			     + " lockup; waiting for lower fees."
			     );
		co_return std::nullopt;
	}
	claimFeerate = feerate;
	co_return sighash;
}

Ev::Io<void> ClaimTxHandler::persist_reveal() {
	auto tx = co_await db.transact();
	tx.query(R"QRY(
	UPDATE "BoltzServiceFactory_rsub"
	   SET preimageRevealed = 1
	     , lockupTxid = :lockup_txid
	     , lockupOut = :lockupOut
	     , lockupTx = :lockupTx
	 WHERE apiAccess = :apiAccess
	   AND swapId = :swapId
	     ;
	)QRY")
		.bind(":lockup_txid", std::string(lockup_txid))
		.bind(":lockupOut", lockupOut)
		.bind(":lockupTx", std::string(lockup_tx))
		.bind(":apiAccess", api_endpoint)
		.bind(":swapId", swapId)
		.execute();
	tx.commit();
	preimageRevealed = true;
}

Ev::Io<void> ClaimTxHandler::claim( Sha256::Hash keypath_sighash
				  , std::uint32_t feerate
				  ) {
	/* The preimage is recorded as out, so the provider can
	 * settle our payment whatever happens next; the claim must
	 * go through with or without it.  */
	if (!env.cooperative_claims()) {
		co_await logd( "Cooperative claims are off; claiming "
			       "through the script path."
			     );
		co_await script_path_claim(feerate);
		co_return;
	}
	if (co_await cooperative_claim(keypath_sighash))
		co_return;
	co_await script_path_claim(feerate);
}

Ev::Io<bool> ClaimTxHandler::cooperative_claim(Sha256::Hash sighash) {
	/* Now compute the real preimage from the signer key and the
	 * in-database preimage.  */
	real_preimage = Detail::compute_preimage(signer, preimage);

	auto error = std::string();
	try {
		musigsession.load_sighash(sighash);
		/* Create our nonce pair for the musig session.  Nonces
		 * are never stored or reused: every attempt makes new
		 * ones.  */
		local_pubnonce = musigsession.generate_local_nonces(random);
	} catch (Musig::InvalidArg const& e) {
		error = e.what();
	}
	if (!error.empty()) {
		co_await loge(error);
		throw End();
	}

	/* Send the preimage, our public nonce and the unsigned claim
	 * tx to the provider.  The preimage is the real one: its
	 * SHA-256 is the hash the swap was created with and the one
	 * in the provider's invoice; the in-database value is only
	 * the material it is derived from.  */
	auto params = Json::Out()
		.start_object()
			.field("preimage", std::string(real_preimage))
			.field("pubNonce", local_pubnonce)
			.field("transaction", std::string(claim_tx))
			.field("index", 0)
		.end_object()
		;
	auto res = std::unique_ptr<Jsmn::Object>();
	try {
		res = Util::make_unique<Jsmn::Object>(co_await conn.api(
			"/v2/swap/reverse/" + swapId + "/claim"
		      , Util::make_unique<Json::Out>(std::move(params))
		));
	} catch (Boltz::ApiError const& e) {
		error = e.what();
	}
	if (!res) {
		co_await loge( "Claim request failed: " + error
			     + "; claiming through the script path."
			     );
		co_return false;
	}

	/* We receive from the provider their partial sig and public
	 * nonce in reply.  */
	auto boltzpubnonce = std::string();
	auto boltzpartialsig = std::string();
	try {
		boltzpubnonce = (std::string) (*res)["pubNonce"];
		boltzpartialsig = (std::string) (*res)["partialSignature"];
	} catch (Jsmn::TypeError const& e) {
		auto os = std::ostringstream();
		os << "Unexpected result from service: " << *res;
		error = os.str();
	}
	if (error.empty() && !Util::Str::ishex(boltzpubnonce))
		error = "invalid pubnonce from boltz: " + boltzpubnonce;
	if (error.empty() && !Util::Str::ishex(boltzpartialsig))
		error = "invalid partialsig from boltz: " + boltzpartialsig;
	if (!error.empty()) {
		co_await loge( error
			     + "; claiming through the script path."
			     );
		co_return false;
	}

	std::uint8_t aggsig_buffer[64];
	auto aggregatesig = Secp256k1::SchnorrSig();
	try {
		/* Aggregate our public nonce with the provider's.  */
		musigsession.load_pubnonce(boltzpubnonce);
		musigsession.aggregate_pubnonces();
		/* Sign our partial signature and combine into a
		 * single signature.  */
		musigsession.load_partial(boltzpartialsig);
		musigsession.sign_partial();
		musigsession.verify_part_sigs();
		musigsession.aggregate_partials(aggsig_buffer);
		aggregatesig = SchnorrSig::from_buffer(aggsig_buffer);
		if (!aggregatesig.valid( musigsession.get_output_key()
				       , musigsession.get_sighash()
				       ))
			error = "invalid aggsig for given sighash and output key";
	} catch (Musig::InvalidArg const& e) {
		error = e.what();
	} catch (Secp256k1::InvalidPrivKey const& e) {
		error = e.what();
	}
	if (!error.empty()) {
		co_await loge( error
			     + "; claiming through the script path."
			     );
		co_return false;
	}
	affix_aggregated_signature(claim_tx, aggregatesig.to_buffer());
	co_await broadcast_and_record("cooperative");
	co_return true;
}

Ev::Io<void> ClaimTxHandler::script_path_claim(std::uint32_t feerate) {
	/* The claim leaf takes the preimage and a signature from our
	 * key alone.  Its witness is larger than the key path's, so
	 * the claim is rebuilt with the fee for it.  */
	auto built = co_await build_claim(feerate, ClaimPath::ScriptPath);
	if (!built)
		throw End();
	auto sighash = *built;
	real_preimage = Detail::compute_preimage(signer, preimage);

	auto error = std::string();
	auto signature = std::vector<std::uint8_t>();
	auto control_block = std::vector<std::uint8_t>();
	try {
		auto keys = signer.get_keypair_tweak(tweak);
		signature = SchnorrSig::create(keys.priv(), sighash).to_buffer();

		/* The control block: the leaf version with the parity
		 * of the output key, the internal key, and the merkle
		 * path to the root, which for a tree of two leaves is
		 * the other leaf's hash.  */
		std::uint8_t buf[32];
		control_block.push_back(std::uint8_t(
			0xc0 | (musigsession.get_output_key_parity() ? 1 : 0)
		));
		musigsession.get_internal_key().to_buffer(buf);
		control_block.insert(control_block.end(), buf, buf + 32);
		auto sibling = Secp256k1::TapTree::leaf_hash(refundScript);
		control_block.insert( control_block.end()
				    , sibling.begin(), sibling.end()
				    );
	} catch (Musig::InvalidArg const& e) {
		error = e.what();
	} catch (Secp256k1::InvalidPrivKey const& e) {
		error = e.what();
	}
	if (!error.empty()) {
		co_await loge(error);
		throw End();
	}

	std::uint8_t preimage_buf[32];
	real_preimage.to_buffer(preimage_buf);
	affix_script_path_witness
		( claim_tx
		, signature
		, std::vector<std::uint8_t>(preimage_buf, preimage_buf + 32)
		, claimScript
		, control_block
		);
	co_await broadcast_and_record("script path");
}

Ev::Io<void> ClaimTxHandler::broadcast_and_record(std::string const& how) {
	co_await logd( "Broadcasting claim tx (" + how + "): "
		     + std::string(claim_tx)
		     );
	auto success = co_await env.broadcast_tx(claim_tx);
	if (!success) {
		co_await loge( std::string("Broadcast failed! ")
			     + std::string(claim_tx)
			     );
		throw End();
	}

	/* Record the signed claim for rebroadcast until our node
	 * sees it confirmed.  */
	auto tx = co_await db.transact();
	tx.query(R"QRY(
	UPDATE "BoltzServiceFactory_rsub"
	   SET lockedUp = 1
	     , lockupClaimFees = :lockupClaimFees
	     , claimTx = :claimTx
	     , claimFeerate = :claimFeerate
	 WHERE apiAccess = :apiAccess
	   AND swapId = :swapId
	     ;
	)QRY")
		.bind(":lockupClaimFees", lockupClaimFees.to_sat())
		.bind(":claimTx", std::string(claim_tx))
		.bind(":claimFeerate", claimFeerate)
		.bind(":apiAccess", api_endpoint)
		.bind(":swapId", swapId)
		.execute();
	tx.commit();
}

Ev::Io<void> ClaimTxHandler::track_claim() {
	if (claimTxHex.empty()) {
		/* The preimage is out but no claim was ever broadcast:
		 * the provider did not co-sign, or we stopped before
		 * the broadcast.  Ask again, with new nonces.  */
		co_await prepare_keys();
		auto feerate = co_await env.get_feerate();
		auto sighash = co_await build_claim(feerate, ClaimPath::KeyPath);
		if (!sighash)
			throw End();
		co_await claim(*sighash, feerate);
		co_return;
	}

	auto error = std::string();
	auto claim = Bitcoin::Tx();
	try {
		claim = Bitcoin::Tx(claimTxHex);
	} catch (std::exception const& e) {
		error = e.what();
	}
	if (!error.empty()) {
		co_await loge( "Stored claim transaction did not "
			       "parse: " + error
			     );
		throw End();
	}
	auto claim_txid = claim.get_txid();

	/* Done once our own wallet holds the claim's output,
	 * confirmed.  The output is named by the claim's transaction
	 * id: the provider learns the destination address from the
	 * claim request, and a payment of its own to that address
	 * must not pass for the claim.  */
	if (co_await env.has_confirmed_output(claim_txid, 0)) {
		co_await logd( "Claim " + std::string(claim_txid)
			     + " confirmed; swap done."
			     );
		co_await remove_row();
		co_return;
	}

	/* The lockup output can be spent by a claim of ours or, from
	 * the timeout on, by the provider's refund.  A spend in our
	 * blocks that pays our address is our claim, whichever
	 * version of it got mined; any other spend is the loss of
	 * the swap.  Either way the swap is over.  */
	auto spend = co_await find_lockup_spend();
	if (spend) {
		auto ours = Bitcoin::addr_to_scriptPubKey(destinationAddress);
		auto pays_us = false;
		for (auto const& out : spend->tx.outputs)
			if (out.scriptPubKey == ours)
				pays_us = true;
		auto spend_txid = std::string(spend->tx.get_txid());
		if (pays_us)
			co_await logd( "Claim " + spend_txid
				     + " confirmed in block "
				     + std::to_string(spend->height)
				     + "; swap done."
				     );
		else
			co_await loge( "Lockup " + std::string(lockup_txid)
				     + ":" + std::to_string(lockupOut)
				     + " was spent in block "
				     + std::to_string(spend->height)
				     + " by " + spend_txid
				     + ", which does not pay our address; "
				       "the swap's funds are lost."
				     );
		co_await remove_row();
		co_return;
	}

	/* From the timeout on the provider can take the lockup back,
	 * but until it does our claim stays valid: the claim leaf
	 * has no timelock.  So the swap goes on past its timeout,
	 * and is given up only when nothing has spent the lockup
	 * long after.  */
	if (blockheight == timeoutBlockheight)
		co_await loge( "Reached the timeout with the claim "
			       "unconfirmed; the provider can refund the "
			       "lockup from here.  Still claiming."
			     );
	if (blockheight >= timeoutBlockheight + give_up_blocks) {
		co_await loge( std::to_string(give_up_blocks)
			     + " blocks past the timeout with the lockup "
			       "unspent and no claim confirmed; giving up "
			       "on the swap."
			     );
		co_await remove_row();
		co_return;
	}

	/* A claim stuck below the going feerate is replaced by one
	 * at that feerate; otherwise the signed claim is kept in
	 * mempools until it confirms.  A rejection of the rebroadcast
	 * is usual: the node already has it.  */
	auto feerate = co_await env.get_feerate();
	if (feerate > claimFeerate && co_await bump_claim(feerate))
		co_return;
	auto accepted = co_await env.broadcast_tx(claim);
	co_await logd( std::string("Claim ") + std::string(claim_txid)
		     + " not yet confirmed; rebroadcast "
		     + (accepted ? "accepted." : "not accepted.")
		     );
}

Ev::Io<bool> ClaimTxHandler::bump_claim(std::uint32_t feerate) {
	auto const old_fee = lockupClaimFees;
	auto const old_feerate = claimFeerate;
	co_await prepare_keys();

	/* A replacement must pay more than what it replaces by at
	 * least one satoshi per vbyte of itself, or mempools keep
	 * the old one.  A key-path claim is smaller than a
	 * script-path one, so at a feerate only a little higher it
	 * can pay less in all; the feerate is raised until the
	 * key-path claim, the smaller of the two, pays enough.  The
	 * first build gives the weight.  */
	auto sighash = std::optional<Sha256::Hash>();
	for (auto attempt = 0; attempt < 2; ++attempt) {
		sighash = co_await build_claim(feerate, ClaimPath::KeyPath);
		if (!sighash) {
			co_await logd( "Keeping the claim at "
				     + std::to_string(old_feerate)
				     + " perkw."
				     );
			claimFeerate = old_feerate;
			lockupClaimFees = old_fee;
			co_return false;
		}
		auto weight = lockupClaimFees.to_sat() * 1000 / feerate;
		auto needed = old_fee + Ln::Amount::sat(weight / 4 + 1);
		if (lockupClaimFees >= needed)
			break;
		feerate = std::uint32_t((needed.to_sat() * 1000 + weight - 1)
				       / weight) + 1;
	}

	co_await logd( "Replacing the claim at "
		     + std::to_string(old_feerate) + " perkw, fee "
		     + std::string(old_fee) + ", with one at "
		     + std::to_string(feerate) + " perkw."
		     );
	co_await claim(*sighash, feerate);
	co_return true;
}

Ev::Io<std::unique_ptr<ClaimTxHandler::LockupSpend>>
ClaimTxHandler::find_lockup_spend() {
	/* A spend can be in the lockup's own block at the earliest.
	 * Blocks already searched are not searched again.  */
	auto from = lockupConfirmedHeight;
	if (from == 0)
		from = createdHeight;
	if (from == 0)
		from = blockheight;
	if (spendSearchedHeight >= from)
		from = spendSearchedHeight + 1;

	auto found = std::unique_ptr<LockupSpend>();
	auto searched = spendSearchedHeight;
	for (auto h = from; h <= blockheight; ++h) {
		auto hex = co_await env.get_block(h);
		if (hex.empty())
			/* Our node does not have that block yet.  */
			break;
		auto block = Bitcoin::Block();
		auto parse_error = std::string();
		try {
			block = Bitcoin::Block(hex);
		} catch (std::exception const& e) {
			parse_error = e.what();
		}
		if (!parse_error.empty()) {
			co_await loge( "Block " + std::to_string(h)
				     + " did not parse: " + parse_error
				     );
			break;
		}
		searched = h;
		auto tx = block.spender(lockup_txid, std::uint32_t(lockupOut));
		if (tx) {
			found = Util::make_unique<LockupSpend>();
			found->height = h;
			found->tx = *tx;
			break;
		}
	}

	if (searched != spendSearchedHeight) {
		auto tx = co_await db.transact();
		tx.query(R"QRY(
		UPDATE "BoltzServiceFactory_rsub"
		   SET spendSearchedHeight = :searched
		 WHERE apiAccess = :apiAccess
		   AND swapId = :swapId
		     ;
		)QRY")
			.bind(":searched", searched)
			.bind(":apiAccess", api_endpoint)
			.bind(":swapId", swapId)
			.execute();
		tx.commit();
		spendSearchedHeight = searched;
	}
	co_return found;
}

Ev::Io<void> ClaimTxHandler::remove_row() {
	auto tx = co_await db.transact();
	tx.query(R"QRY(
	DELETE FROM "BoltzServiceFactory_rsub"
	 WHERE apiAccess = :apiAccess
	   AND swapId = :swapId
	     ;
	)QRY")
		.bind(":apiAccess", api_endpoint)
		.bind(":swapId", swapId)
		.execute();
	tx.commit();
}

}}
