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

/** Boltz::Detail::initial_claim_tx
 *
 * @brief creates the initial claim transaction
 * for a reverse submarine (offchain-to-onchain)
 * swap.
 *
 * @desc if the fee at the given feerate would
 * consume the lockup, the output amount is zero
 * and the transaction must not be signed.
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
}}

#endif /* !defined(BOLTZ_DETAIL_INITIAL_CLAIM_TX_HPP) */
