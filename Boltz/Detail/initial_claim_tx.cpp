#include"Bitcoin/Tx.hpp"
#include"Bitcoin/TxId.hpp"
#include"Bitcoin/addr_to_scriptPubKey.hpp"
#include"Bitcoin/sighash.hpp"
#include"Boltz/Detail/initial_claim_tx.hpp"
#include"Ln/Amount.hpp"
#include"Ln/Preimage.hpp"
#include"Secp256k1/Musig.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include<sstream>

namespace {

/* Gets the non-witness weight of the transaction, in sipa.  */
std::size_t get_nonwitness_weight(Bitcoin::Tx const& tx) {
	auto os = std::ostringstream();
	os << tx;
	return os.str().size() * 4;
}

auto const dust_limit = Ln::Amount::sat(547);

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

		/* Witness details.  */
		, std::vector<std::uint8_t> const& witnessScript

		/* Output address.  */
		, std::string const& output_addr
		) {
	/* Single-input single-output.  */
	claim_tx.inputs.resize(1);
	claim_tx.outputs.resize(1);
	/* Set nLockTime and nSequence.  */
	claim_tx.nLockTime = blockheight + 1;
	claim_tx.inputs[0].nSequence = 0xFFFFFFFF; /* Final!  Not RBF!  */ // TODO except we are now in a post Full-RBF world after Bitcoin 28.0...

	/* Set up input.  */
	claim_tx.inputs[0].prevTxid = lockup_txid;
	claim_tx.inputs[0].prevOut = lockup_outnum;
	claim_tx.inputs[0].scriptSig.resize(0);
	/* signature, preimage, witnessScript */
	claim_tx.inputs[0].witness.witnesses.resize(3);

	/* Set up output.  */
	claim_tx.outputs[0].scriptPubKey = Bitcoin::addr_to_scriptPubKey(
		output_addr
	);

	/* Measure weight.  */
	//TODO value of sig varint and size of signature itself must be schnorr/musig based
	auto nonwitness_weight = get_nonwitness_weight(claim_tx);
	auto witness_weight = 1 /* varint of signature */
			    + 64 /* schnorr signature.  */
			    + 1 /* varint of preimage */
			    + 32 /* preimage */
			    + 1 /* varint of witnessScript */
			    + witnessScript.size()
			    + 2 /* marker+flag */
			    ;
	auto weight = nonwitness_weight + witness_weight
		    + 12 /* Some extra, in case we screwed up the weight.  */
		    ;

	/* Determine fee.  */
	claim_tx_fees = Ln::Amount::sat((weight * feerate_perkw) / 1000);
	/* Is the fee too large?  */
	if (claim_tx_fees + dust_limit > lockup_amount) {
		/* Transform to an OP_RETURN with 20 0 bytes, and give the
		 * entire amount to fees.  */
		claim_tx_fees = lockup_amount;
		claim_tx.outputs[0].scriptPubKey.resize(22);
		std::fill( claim_tx.outputs[0].scriptPubKey.begin()
			 , claim_tx.outputs[0].scriptPubKey.end()
			 , 0x00
			 );
		claim_tx.outputs[0].scriptPubKey[0] = 0x6a;
		claim_tx.outputs[0].scriptPubKey[1] = 0x14;
		claim_tx.outputs[0].amount = Ln::Amount::sat(0);
	} else {
		/* Set amount.  */
		claim_tx.outputs[0].amount = lockup_amount - claim_tx_fees;
	}

	/* Sign!  */
	/* FIXME: I am not at all certain exactly what the first byte of
	 * scriptCode is *actually* supposed to be.
	 * It might be a `varint`, or it might be an `OP_PUSHDATA`.
	 * The below works in practice, so it is considered "okay"
	 * for now.
	 * That took several hours of spinning....
	 */
	auto scriptPubkey = std::vector<std::uint8_t>(witnessScript.size() + 1);
	scriptPubkey[0] = std::uint8_t(witnessScript.size());
	std::copy( witnessScript.begin(), witnessScript.end()
		 , scriptPubkey.begin() + 1
		 );

	return Bitcoin::p2tr_sighash
		( claim_tx
		, Bitcoin::SIGHASH_DEFAULT
		, 0
		, std::vector<Ln::Amount>{lockup_amount}
		, std::vector<std::vector<std::uint8_t>>{scriptPubkey}
		, Bitcoin::KEYPATH
		);
}

void
affix_aggregated_signature( Bitcoin::Tx& claim_tx
		/* Witness details.  */
		, Ln::Preimage const& preimage
		, std::vector<std::uint8_t> const& witnessScript
		, std::vector<std::uint8_t> const& aggregatesig
		) {
	auto scriptPubkey = std::vector<std::uint8_t>(witnessScript.size() + 1);
	scriptPubkey[0] = std::uint8_t(witnessScript.size());
	std::copy( witnessScript.begin(), witnessScript.end()
		 , scriptPubkey.begin() + 1
		 );

	/* Load up the witnesses.  */ // TODO structured the same in bip341 ?
	auto& witnesses = claim_tx.inputs[0].witness.witnesses;
	witnesses[0] = aggregatesig;
	witnesses[0].push_back(std::uint8_t(Bitcoin::SIGHASH_DEFAULT));
	witnesses[1].resize(32);
	preimage.to_buffer(&witnesses[1][0]);
	witnesses[2] = witnessScript;
}

}}
