#ifndef BOLTZ_DETAIL_INITIAL_CLAIM_TX_HPP
#define BOLTZ_DETAIL_INITIAL_CLAIM_TX_HPP

#include"Sha256/Hash.hpp"
#include<cstdint>
#include<string>
#include<vector>

namespace Bitcoin { class Tx; }
namespace Bitcoin { class TxId; }
namespace Ln { class Amount; }

namespace Boltz { namespace Detail {

/** enum Boltz::Detail::ClaimPath
 *
 * @brief which spend of the lockup the claim
 * makes.  The key path carries one aggregate
 * signature.  The script path runs the claim
 * leaf, so it carries our signature, the preimage,
 * the leaf and its control block, and pays more.
 */
enum class ClaimPath { KeyPath, ScriptPath };

/** Boltz::Detail::initial_claim_tx
 *
 * @brief creates the initial claim transaction
 * for a reverse submarine (offchain-to-onchain)
 * swap, with the fee for the given path, and
 * returns the sighash that path signs.
 *
 * @desc the transaction is replaceable and valid
 * from the block after `blockheight`.  If the fee
 * at the given feerate would consume the lockup,
 * the output amount is zero and the transaction
 * must not be signed.
 */
Sha256::Hash
initial_claim_tx( Bitcoin::Tx& claim_tx /* written by function */
		, Ln::Amount& claim_tx_fees /* written by function */

		/* Arguments to the function.  */

		/* Common: feerate and blockheight.  */
		, std::uint32_t feerate_perkw
		, std::uint32_t blockheight

		/* Details of the input.  */
		, Bitcoin::TxId const& lockup_txid
		, std::uint32_t lockup_outnum
		, Ln::Amount lockup_amount

		/* scriptPubKey of the lockup output, for the sighash.  */
		, std::vector<std::uint8_t> const& lockup_scriptPubKey

		/* Output address.  */
		, std::string const& output_addr

		/* The spend made, and the claim leaf the script
		 * path runs (unused for the key path).  */
		, ClaimPath path
		, std::vector<std::uint8_t> const& claim_script
		);

/** Boltz::Detail::affix_aggregated_signature
 *
 * @brief completes the claim transaction from
 * initial_claim_tx with the MuSig2 aggregated
 * signature over its sighash (a key-path spend).
 */
void
affix_aggregated_signature(Bitcoin::Tx& claim_tx /* written by function */
		, std::vector<std::uint8_t> const& aggregatesig
		);

/** Boltz::Detail::affix_script_path_witness
 *
 * @brief completes the claim transaction from
 * initial_claim_tx for the script path: our
 * signature, the preimage, the claim leaf and
 * its control block, in the order the leaf
 * consumes them.
 */
void
affix_script_path_witness( Bitcoin::Tx& claim_tx /* written by function */
			 , std::vector<std::uint8_t> const& signature
			 , std::vector<std::uint8_t> const& preimage
			 , std::vector<std::uint8_t> const& claim_script
			 , std::vector<std::uint8_t> const& control_block
			 );
}}

#endif /* !defined(BOLTZ_DETAIL_INITIAL_CLAIM_TX_HPP) */
