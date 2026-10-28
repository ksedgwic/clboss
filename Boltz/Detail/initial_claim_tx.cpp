#include"Bitcoin/Tx.hpp"
#include"Bitcoin/TxId.hpp"
#include"Bitcoin/addr_to_scriptPubKey.hpp"
#include"Bitcoin/sighash.hpp"
#include"Boltz/Detail/initial_claim_tx.hpp"
#include"Ln/Amount.hpp"
#include"Secp256k1/Musig.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/TapscriptTree.hpp"
#include<sstream>

namespace {

/* Gets the non-witness weight of the transaction, in sipa.  */
std::size_t get_nonwitness_weight(Bitcoin::Tx const& tx) {
	auto os = std::ostringstream();
	os << tx;
	return os.str().size() * 4;
}

auto const dust_limit = Ln::Amount::sat(547);

/* Weight of one witness item: its length prefix and its bytes.  */
std::size_t witness_item_weight(std::size_t len) {
	return (len < 253 ? 1 : 3) + len;
}

}

using namespace Secp256k1;

namespace Boltz { namespace Detail {

Sha256::Hash
initial_claim_tx( Bitcoin::Tx& claim_tx /* written by function */
		, Ln::Amount& claim_tx_fees /* written by function */

		/* Arguments to the function.  */

		/* Feerate.  */
		, std::uint32_t feerate_perkw
		, std::uint32_t blockheight

		/* Details of the input.  */
		, Bitcoin::TxId const& lockup_txid
		, std::uint32_t lockup_outnum
		, Ln::Amount lockup_amount

		/* scriptPubKey of the lockup output.  */
		, std::vector<std::uint8_t> const& lockup_scriptPubKey

		/* Output address.  */
		, std::string const& output_addr

		/* The spend made.  */
		, ClaimPath path
		, std::vector<std::uint8_t> const& claim_script
		) {
	/* Single-input single-output.  */
	claim_tx.inputs.resize(1);
	claim_tx.outputs.resize(1);
	/* Replaceable, so that a claim rebuilt at a higher feerate
	 * takes this one's place in every mempool, signalling or
	 * not.  A non-final sequence enforces the locktime, which
	 * is the current height: the claim is valid from the next
	 * block.  */
	claim_tx.nLockTime = blockheight;
	claim_tx.inputs[0].nSequence = 0xFFFFFFFD;

	/* Set up input.  */
	claim_tx.inputs[0].prevTxid = lockup_txid;
	claim_tx.inputs[0].prevOut = lockup_outnum;
	claim_tx.inputs[0].scriptSig.resize(0);
	/* The key path's witness is the aggregated signature alone;
	 * the script path's is signature, preimage, leaf and control
	 * block.  */
	claim_tx.inputs[0].witness.witnesses.resize(
		path == ClaimPath::KeyPath ? 1 : 4
	);

	/* Set up output.  */
	claim_tx.outputs[0].scriptPubKey = Bitcoin::addr_to_scriptPubKey(
		output_addr
	);

	/* Measure weight.  */
	auto nonwitness_weight = get_nonwitness_weight(claim_tx);
	auto witness_weight = 1 /* witness item count */
			    + witness_item_weight(64) /* schnorr signature, SIGHASH_DEFAULT */
			    + 2 /* marker+flag */
			    ;
	if (path == ClaimPath::ScriptPath)
		witness_weight += witness_item_weight(32) /* preimage */
			       + witness_item_weight(claim_script.size())
			       + witness_item_weight(65) /* control block */
			       ;
	auto weight = nonwitness_weight + witness_weight
		    + 12 /* Some extra, in case we screwed up the weight.  */
		    ;

	/* Determine fee.  */
	claim_tx_fees = Ln::Amount::sat((weight * feerate_perkw) / 1000);
	/* Is the fee too large?  */
	if (claim_tx_fees + dust_limit > lockup_amount) {
		/* The fee would consume the lockup.  An output of
		 * nothing tells the caller, which does not sign this;
		 * revealing the preimage for a transaction that pays
		 * us nothing would only settle our payment.  */
		claim_tx_fees = lockup_amount;
		claim_tx.outputs[0].amount = Ln::Amount::sat(0);
	} else {
		/* Set amount.  */
		claim_tx.outputs[0].amount = lockup_amount - claim_tx_fees;
	}

	/* The BIP341 sighash covers each spent scriptPubKey with
	 * its CompactSize length prefix, the way a TxOut serializes
	 * it; the lockup scriptPubKey is 34 bytes, so one byte.  */
	auto prefixed = std::vector<std::uint8_t>(lockup_scriptPubKey.size() + 1);
	prefixed[0] = std::uint8_t(lockup_scriptPubKey.size());
	std::copy( lockup_scriptPubKey.begin(), lockup_scriptPubKey.end()
		 , prefixed.begin() + 1
		 );

	if (path == ClaimPath::KeyPath)
		return Bitcoin::p2tr_sighash
			( claim_tx
			, Bitcoin::SIGHASH_DEFAULT
			, 0
			, std::vector<Ln::Amount>{lockup_amount}
			, std::vector<std::vector<std::uint8_t>>{prefixed}
			);

	/* The script path signs for the leaf it runs.  */
	auto extension = Bitcoin::TapscriptExtension();
	auto leaf = Secp256k1::TapTree::leaf_hash(claim_script);
	extension.leaf_hash.from_buffer(leaf.data());
	return Bitcoin::p2tr_sighash
		( claim_tx
		, Bitcoin::SIGHASH_DEFAULT
		, 0
		, std::vector<Ln::Amount>{lockup_amount}
		, std::vector<std::vector<std::uint8_t>>{prefixed}
		, extension
		);
}

void
affix_aggregated_signature( Bitcoin::Tx& claim_tx
		, std::vector<std::uint8_t> const& aggregatesig
		) {
	/* BIP341 key path: one witness item, the 64-byte signature.
	 * SIGHASH_DEFAULT is implied by the length; a 65th byte of
	 * 0x00 would make the input invalid.  */
	claim_tx.inputs[0].witness.witnesses[0] = aggregatesig;
}

void
affix_script_path_witness( Bitcoin::Tx& claim_tx
			 , std::vector<std::uint8_t> const& signature
			 , std::vector<std::uint8_t> const& preimage
			 , std::vector<std::uint8_t> const& claim_script
			 , std::vector<std::uint8_t> const& control_block
			 ) {
	/* The leaf pops the preimage first, then the signature, so
	 * the signature is the bottom item; the leaf and its control
	 * block end the stack, as BIP341 lays out a script-path
	 * witness.  */
	auto& witness = claim_tx.inputs[0].witness.witnesses;
	witness.resize(4);
	witness[0] = signature;
	witness[1] = preimage;
	witness[2] = claim_script;
	witness[3] = control_block;
}

}}
