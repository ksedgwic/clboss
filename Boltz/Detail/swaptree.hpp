#ifndef BOLTZ_DETAIL_SWAP_TREE_HPP
#define BOLTZ_DETAIL_SWAP_TREE_HPP

#include<cstdint>
#include<vector>

namespace Ripemd160 { class Hash; }
namespace Secp256k1 { class XonlyPubKey; }

namespace Boltz { namespace Detail {

/** Boltz::Detail::claim_leaf
 *
 * @brief the tapscript of a reverse swap's claim
 * leaf, as the Boltz API v2 backend writes it:
 *
 *     OP_SIZE 32 OP_EQUALVERIFY
 *     OP_HASH160 <hash160 of the preimage> OP_EQUALVERIFY
 *     <claim key> OP_CHECKSIG
 *
 * @desc the setup builds this from our own key and
 * preimage hash and accepts the provider's leaf only
 * when the bytes are the same.
 */
std::vector<std::uint8_t> claim_leaf
				( Ripemd160::Hash const& preimage_hash160
				, Secp256k1::XonlyPubKey const& claim_key
				);

/** Boltz::Detail::refund_leaf
 *
 * @brief the tapscript of a reverse swap's refund
 * leaf:
 *
 *     <refund key> OP_CHECKSIGVERIFY
 *     <timeout block height> OP_CHECKLOCKTIMEVERIFY
 *
 * @desc the height is pushed as a minimal script
 * number, the only encoding the backend writes.
 */
std::vector<std::uint8_t> refund_leaf
				( Secp256k1::XonlyPubKey const& refund_key
				, std::uint32_t timeout_blockheight
				);

/** Boltz::Detail::compute_root_hash
 *
 * @brief the merkle root of the swap tree, the two
 * leaves above, which tweaks the aggregate key into
 * the lockup's output key.
 */
std::vector<std::uint8_t> compute_root_hash
				( std::vector<std::uint8_t> const& claimscript
				, std::vector<std::uint8_t> const& refundscript
				);

}}

#endif /* !defined(BOLTZ_DETAIL_SWAP_TREE_HPP) */
