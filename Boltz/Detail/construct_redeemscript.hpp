#ifndef BOLTZ_DETAIL_CONSTRUCT_REDEEMSCRIPT_HPP
#define BOLTZ_DETAIL_CONSTRUCT_REDEEMSCRIPT_HPP

#include<cstdint>
#include<vector>

namespace Ripemd160 { class Hash; }
namespace Secp256k1 { class PubKey; }

namespace Boltz { namespace Detail {

/** Boltz::Detail::construct_redeemscript
 *
 * @brief Rebuilds the expected reverse submarine swap
 * HTLC redeemScript from the known swap parameters.
 *
 * Used by SwapSetupHandler to byte-compare against the
 * server-provided redeemScript, following Electrum's
 * _construct_swap_scriptcode pattern.
 *
 * The script template is:
 *   OP_SIZE PUSH(32) OP_EQUAL OP_IF
 *     OP_HASH160 PUSH(20) <hash160> OP_EQUALVERIFY
 *     PUSH(33) <pubkey_hash>
 *   OP_ELSE
 *     OP_DROP PUSH(3) <locktime_le3> OP_CHECKLOCKTIMEVERIFY OP_DROP
 *     PUSH(33) <pubkey_locktime>
 *   OP_ENDIF
 *   OP_CHECKSIG
 */
std::vector<std::uint8_t>
construct_redeemscript( Ripemd160::Hash const& hash160
		      , Secp256k1::PubKey const& pubkey_hash
		      , std::uint32_t locktime
		      , Secp256k1::PubKey const& pubkey_locktime
		      );

}}

#endif /* !defined(BOLTZ_DETAIL_CONSTRUCT_REDEEMSCRIPT_HPP) */
