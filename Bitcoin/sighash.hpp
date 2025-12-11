#ifndef BITCOIN_SIGHASH_HPP
#define BITCOIN_SIGHASH_HPP

#include<cstddef>
#include<cstdint>
#include<stdexcept>
#include<vector>

namespace Bitcoin { class Tx; }
namespace Ln { class Amount; }
namespace Sha256 { class Hash; }

namespace Bitcoin {

enum SighashFlags
{ SIGHASH_DEFAULT = 0 /* only for taproot/segwit v1 inputs */
, SIGHASH_ALL = 1
, SIGHASH_NONE = 2
, SIGHASH_SINGLE = 3
, SIGHASH_ANYONECANPAY = 0x80
};

enum p2trSpendType
{ KEYPATH = 0
, SCRIPTPATH = 1
};

struct InvalidSighash : public Util::BacktraceException<std::invalid_argument> {
	InvalidSighash() =delete;
	InvalidSighash(std::string const& msg)
		: Util::BacktraceException<std::invalid_argument>(
			std::string("Bitcoin::InvalidSighash: ") + msg
		  ) { }
};

/** Bitcoin::sighash
 *
 * @brief computes the sighash for the
 * given transaction, being signed with
 * the given flags, and signing for the
 * input nIn.
 *
 * @desc throws `Bitcoin::InvalidSighash`
 * if the given `flags` is not recognized,
 * or if `nIn` is out-of-range.
 *
 * The given `scriptCode` should consider
 * `OP_CODESEPERATOR`.
 * For P2WPKH it should be the constructed
 * script "1976a914{20-byte-pubkey-hash}88ac".
 * For P2WSH it should be the `witnessScript`
 * whose hash is what is committed in the
 * `scriptPubKey`, and which is the top of the
 * witness stack.
 *
 * This is strictly for SegWit sighash
 * algorithm.
 */
Sha256::Hash
sighash( Bitcoin::Tx const& tx
       , SighashFlags flags
       , std::size_t nIn
       , Ln::Amount amount
       , std::vector<std::uint8_t> const& scriptCode
       );

/** Bitcoin::p2tr_sighash
 *
 * @brief computes a bip341 spec sighash
 *
 * @desc taproot/schnorr spends require an
 * altogether different sighash type, specified
 * in bip 341. `spend_type` values higher than
 * 127 throw `Bitcoin::InvalidSighash`.
 *
 * bip341 signature digest:
 *
 * - all sha256 invocations are single only
 * - the final sha256 hash over all the data
 *   must be a tagged hash as described in
 *   bip340 (tag: "TapSighash")
 * - unconditionally present:
 *     hashtype
 *     nVersion
 *     nLocktime
 *     spend_type
 * - [number] indicates byte quantity
 *
 * [1] hashtype
 * [4] nVersion
 * [4] nLocktime
 * if (hashtype & 0x80 != SIGHASH_ANYONECANPAY)
 *		[32] sha256(prevouts)
 *		[32] sha256(amounts)
 *		[32] sha256(scriptpubkeys)
 * 		[32] sha256(nSequences)
 *
 * if (hashtype & 0x03 != SIGHASH_SINGLE || hashtype & 0x03 != SIGHASH_ANYONECANPAY)
 *		[32] sha256(outputs)
 *
 * [1] spendtype (bip341 ext_flag * 2 + annex flag)
 *
 * if (hashtype & 0x80 == SIGHASH_ANYONECANPAY)
 *		[36] outpoint (32 byte txid + 4 byte output index)
 *		[8]  amount
 *		[35] scriptpubkey
 *		[4]  nSequence
 * else
 *		[4] inputindex
 *
 * if (spendtype & 0x01)
 *		[32] sha256(compactsize(annex) + 0x50 + annex)
 *
 * if (hashtype & 0x03 == SIGHASH_SINGLE)
 *		[32] sha256(nominatedoutput)
 */
Sha256::Hash
p2tr_sighash( Bitcoin::Tx const& tx
			, SighashFlags flags
			, std::uint32_t nIn
			, std::vector<Ln::Amount> in_amounts
			, std::vector<std::vector<std::uint8_t>> const& scriptPubKeys
			, p2trSpendType spendtype
			);
}

#endif /* !defined(BITCOIN_SIGHASH_HPP) */
