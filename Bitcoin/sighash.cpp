#include"Bitcoin/Tx.hpp"
#include"Bitcoin/le.hpp"
#include"Bitcoin/sighash.hpp"
#include"Ln/Amount.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/HasherStream.hpp"
#include"Sha256/fun.hpp"
#include"Secp256k1/tagged_hashes.hpp"

namespace {

void feed_hash(std::ostream& hasher, Sha256::Hash const& hash) {
	std::uint8_t buf[32];
	hash.to_buffer(buf);
	for (auto i = std::size_t(0); i < sizeof(buf); ++i)
		hasher.put(buf[i]);
}

using ::Bitcoin::SighashFlags;
using ::Bitcoin::SIGHASH_DEFAULT;
using ::Bitcoin::SIGHASH_ALL;
using ::Bitcoin::SIGHASH_NONE;
using ::Bitcoin::SIGHASH_SINGLE;
using ::Bitcoin::SIGHASH_ANYONECANPAY;
using ::Bitcoin::InvalidSighash;
using ::Bitcoin::p2trSpendType;
using ::Bitcoin::KEYPATH;
using ::Bitcoin::SCRIPTPATH;

void
sighash_core( Bitcoin::Tx const& tx
	    , SighashFlags flags
	    , std::size_t nIn
	    , Ln::Amount amount
	    , std::vector<std::uint8_t> const& scriptCode
	    , std::ostream& hasher
	    ) {

	auto loflags = flags & 0x1F;
	auto hiflags = flags & 0xE0;

	switch (loflags) {
	case SIGHASH_ALL:
	case SIGHASH_NONE:
	case SIGHASH_SINGLE:
		break;
	default:
		throw InvalidSighash("Invalid SIGHASH flag");
	}
	switch (hiflags) {
	case 0:
	case SIGHASH_ANYONECANPAY:
		break;
	default:
		throw InvalidSighash("Invalid SIGHASH flag");
	}

	if (nIn >= tx.inputs.size())
		throw InvalidSighash("nIn out of range");

	auto hashPrevouts = Sha256::Hash();
	if (!(hiflags & SIGHASH_ANYONECANPAY)) {
		Sha256::HasherStream hasher;
		for (auto const& i : tx.inputs)
			hasher << i.prevTxid
			       << Bitcoin::le(i.prevOut)
				;
		hashPrevouts = Sha256::fun(std::move(hasher).finalize());
	}

	auto hashSequence = Sha256::Hash();
	if ( !(hiflags & SIGHASH_ANYONECANPAY)
	  && (loflags != SIGHASH_SINGLE)
	  && (loflags != SIGHASH_NONE)
	   ) {
		Sha256::HasherStream hasher;
		for (auto const& i : tx.inputs)
			hasher << Bitcoin::le(i.nSequence);
		hashSequence = Sha256::fun(std::move(hasher).finalize());
	}

	auto hashOutputs = Sha256::Hash();
	if ( (loflags != SIGHASH_SINGLE)
	  && (loflags != SIGHASH_NONE)
	   ) {
		Sha256::HasherStream hasher;
		for (auto const& o : tx.outputs)
			hasher << o;
		hashOutputs = Sha256::fun(std::move(hasher).finalize());
	} else if ((loflags == SIGHASH_SINGLE) && nIn < tx.outputs.size()) {
		Sha256::HasherStream hasher;
		hasher << tx.outputs[nIn];
		hashOutputs = Sha256::fun(std::move(hasher).finalize());
	}

	/* Hashing sequence.  */
	hasher << Bitcoin::le(tx.nVersion);
	feed_hash(hasher, hashPrevouts);
	feed_hash(hasher, hashSequence);

	/* Input being signed.  */
	hasher << tx.inputs[nIn].prevTxid
	       << Bitcoin::le(tx.inputs[nIn].prevOut)
		;
	for (auto b : scriptCode)
		hasher.put(b);
	hasher << Bitcoin::le(amount)
	       << Bitcoin::le(tx.inputs[nIn].nSequence)
		;

	feed_hash(hasher, hashOutputs);
	hasher << Bitcoin::le(tx.nLockTime)
	       << Bitcoin::le(std::uint32_t(flags))
		;
}

void
p2tr_sighash_core( Bitcoin::Tx const& tx
	    , SighashFlags flags
	    , std::uint32_t nIn
		, std::vector<Ln::Amount> in_amounts
		, std::vector<std::vector<std::uint8_t>> const& scriptPubKeys
		, p2trSpendType spendtype
	    , std::ostream& hasher
	    ) {
	/* spend type cannot exceed 7 bits to conform to bip341.  */
	if (spendtype & 0x80)
		throw InvalidSighash("p2tr SpendType flag exceeds 0x7F");

	auto loflags = flags & 0x1F;
	auto hiflags = flags & 0xE0;

	switch (loflags) {
	case SIGHASH_DEFAULT:
	case SIGHASH_ALL:
	case SIGHASH_NONE:
	case SIGHASH_SINGLE:
		break;
	default:
		throw InvalidSighash("Invalid SIGHASH flag");
	}
	switch (hiflags) {
	case 0:
	case SIGHASH_ANYONECANPAY:
		break;
	default:
		throw InvalidSighash("Invalid SIGHASH flag");
	}

	if (nIn >= tx.inputs.size())
		throw InvalidSighash("nIn out of range");

	auto hashPrevouts = Sha256::Hash();
	if (!(hiflags & SIGHASH_ANYONECANPAY)) {
		Sha256::HasherStream hasher;
		for (auto const& i : tx.inputs)
			hasher << i.prevTxid
			       << Bitcoin::le(i.prevOut)
				;
		hashPrevouts = std::move(hasher).finalize();
	}

	auto hashAmounts = Sha256::Hash();
	if (!(hiflags & SIGHASH_ANYONECANPAY)) {
		Sha256::HasherStream hasher;
		for (auto const& a : in_amounts)
			hasher << Bitcoin::le(a.to_sat());
		hashAmounts = std::move(hasher).finalize();
	}

	auto hashScriptPubkeys = Sha256::Hash();
	if (!(hiflags & SIGHASH_ANYONECANPAY)) {
		Sha256::HasherStream hasher;
		for (auto const& spk : scriptPubKeys)
			for (auto b : spk)
				hasher.put(b);
		hashScriptPubkeys = std::move(hasher).finalize();
	}

	auto hashSequences = Sha256::Hash();
	if (!(hiflags & SIGHASH_ANYONECANPAY)) {
		Sha256::HasherStream hasher;
		for (auto const& i : tx.inputs)
			hasher << Bitcoin::le(i.nSequence);
		hashSequences = std::move(hasher).finalize();
	}

	auto hashOutputs = Sha256::Hash();
	if ( (loflags != SIGHASH_SINGLE)
	  && (loflags != SIGHASH_NONE)
	   ) {
		Sha256::HasherStream hasher;
		for (auto const& o : tx.outputs)
			hasher << o;
		hashOutputs = std::move(hasher).finalize();
	}

	/* Hashing sequence.  */
	/* 1. tx data */
	hasher << std::uint8_t(0x00) /* sighash epoch 	*/
		   << std::uint8_t(flags) /* taproot spend type  */
		   << Bitcoin::le(tx.nVersion)
		   << Bitcoin::le(tx.nLockTime);

	if (!(hiflags & SIGHASH_ANYONECANPAY)) {
		feed_hash(hasher, hashPrevouts);
		feed_hash(hasher, hashAmounts);
		feed_hash(hasher, hashScriptPubkeys);
		feed_hash(hasher, hashSequences);
	}

	if ( (loflags != SIGHASH_SINGLE)
	  && (loflags != SIGHASH_NONE)
	   ) {
		feed_hash(hasher, hashOutputs);
	}

	/* 2. input data */
	/* spend type (bip341 ext_flag + optional annex flag).
	 * NOTE: as of the present Bitcoin Core (v.28.0), there
	 * is no support for interpreting annex data, and so
	 * we cannot either.  */
	hasher << std::uint8_t(spendtype);

	if (hiflags & SIGHASH_ANYONECANPAY) {
		auto const& input = tx.inputs[nIn];
		/* outpoint as COutPoint */
		hasher << input.prevTxid
		       << Bitcoin::le(input.prevOut);
		/* little-endian input amount */
		hasher << Bitcoin::le(in_amounts[nIn].to_sat());
		/* varint(scriptPubKey) + scriptPubKey */
		auto const& spk = scriptPubKeys[nIn];
		for (auto b : spk)
			hasher.put(b);
		/* little-endian nSequence */
		hasher << Bitcoin::le(input.nSequence);
	} else {
		/* little-endian input number */
		hasher << Bitcoin::le(nIn);
	}

	/* NOTE: a 32 byte sha-256 hash of formatted annex data
	 * would be put into the sighash stream at this stage, if
	 * 'annex present' was indicated in the ext_flag byte.
	 * As noted above, Bitcoin Core does not support annex
	 * data use in taproot transactions as of v.28.0 */

	/* 3. output data */
	/* SIGHASH_SINGLE corresponding output.  */
	if ((loflags == SIGHASH_SINGLE) && nIn < tx.outputs.size()) {
		{
			Sha256::HasherStream hasher;
			hasher << tx.outputs[nIn];
			hashOutputs = std::move(hasher).finalize();
		}

		feed_hash(hasher, hashOutputs);
	}
}

}

namespace Bitcoin {

Sha256::Hash
sighash( Bitcoin::Tx const& tx
       , SighashFlags flags
       , std::size_t nIn
       , Ln::Amount amount
       , std::vector<std::uint8_t> const& scriptCode
       ) {
	Sha256::HasherStream hasher;
	sighash_core(tx, flags, nIn, amount, scriptCode, hasher);
	return Sha256::fun(std::move(hasher).finalize());
}

Sha256::Hash
p2tr_sighash( Bitcoin::Tx const& tx
			, SighashFlags flags
			, std::uint32_t nIn
			, std::vector<Ln::Amount> in_amounts
			, std::vector<std::vector<std::uint8_t>> const& scriptPubKeys
			, p2trSpendType spendtype
			) {
	auto hasher = Sha256::HasherStream(Tag::SIGHASH);
	p2tr_sighash_core(tx, flags, nIn, std::move(in_amounts), scriptPubKeys, spendtype, hasher);
	/* bip341 specifies single sha256 over sighash input data.  */
	return std::move(hasher).finalize();
}

}
