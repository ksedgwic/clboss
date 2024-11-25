#include<assert.h>
#include<secp256k1.h>
#include<secp256k1_schnorrsig.h>
#include<string.h>
#include"Secp256k1/Detail/context.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Sha256/Hash.hpp"
#include"Util/Str.hpp"

using Secp256k1::Detail::context;

namespace Secp256k1 {

Signature::Signature(std::uint8_t const buffer[64]) {
	auto res = secp256k1_ecdsa_signature_parse_compact
		( context.get()
		, reinterpret_cast<secp256k1_ecdsa_signature*>(data)
		, reinterpret_cast<const unsigned char*>(buffer)
		);
	if (res == 0)
		throw BadSignatureEncoding();
	secp256k1_ecdsa_signature_normalize
		( context.get()
		, reinterpret_cast<secp256k1_ecdsa_signature*>(data)
		, reinterpret_cast<secp256k1_ecdsa_signature*>(data)
		);
}
Signature::Signature( Secp256k1::PrivKey const& sk
		    , Sha256::Hash const& m
		    ) {
	std::uint8_t mbuf[32];
	m.to_buffer(mbuf);
	unsigned char extra_entropy[32] = { 0 };

	do {
		auto res = secp256k1_ecdsa_sign
			( context.get()
			, reinterpret_cast<secp256k1_ecdsa_signature*>(data)
			, reinterpret_cast<const unsigned char*>(mbuf)
			, reinterpret_cast<const unsigned char*>(sk.key)
			, nullptr
			, extra_entropy
			);
		if (res == 0)
			/* Extremely unlikely to happen.
			 * TODO: backtrace-capturing.
			 */
			throw Util::BacktraceException<std::runtime_error>("Nonce generation for signing failed.");
		++(*reinterpret_cast<std::uint64_t*>(extra_entropy));
	} while (!sig_has_low_r());
}

bool Signature::sig_has_low_r() const {
	/* As policy, Bitcoin requires signatures to have low-R for
	 * compactness, and would fail to propgate if not.
	 * Below was cribbed form lightningd.
	 */
	unsigned char compact_sig[64];
	secp256k1_ecdsa_signature_serialize_compact
		( context.get()
		, compact_sig
		, reinterpret_cast<secp256k1_ecdsa_signature const*>(data)
		);
	return compact_sig[0] < 0x80;
}

Signature::Signature() {
	unsigned char buffer[64];
	memset(buffer, 0, 64);
	auto res = secp256k1_ecdsa_signature_parse_compact
		( context.get()
		, reinterpret_cast<secp256k1_ecdsa_signature*>(data)
		, buffer
		);
	if (res)
		return;
	else
		return;
}

Signature::Signature(std::string const& s) {
	auto buf = Util::Str::hexread(s);
	if (buf.size() != 64)
		throw BadSignatureEncoding();
	auto res = secp256k1_ecdsa_signature_parse_compact
		( context.get()
		, reinterpret_cast<secp256k1_ecdsa_signature*>(data)
		, reinterpret_cast<const unsigned char*>(&buf[0])
		);
	if (res == 0)
		throw BadSignatureEncoding();
	secp256k1_ecdsa_signature_normalize
		( context.get()
		, reinterpret_cast<secp256k1_ecdsa_signature*>(data)
		, reinterpret_cast<secp256k1_ecdsa_signature*>(data)
		);
}

void Signature::to_buffer(std::uint8_t buffer[64]) const {
	auto res = secp256k1_ecdsa_signature_serialize_compact
		( context.get()
		, reinterpret_cast<unsigned char*>(buffer)
		, reinterpret_cast<const secp256k1_ecdsa_signature*>(data)
		);
	assert(res == 1);
}

bool Signature::valid( Secp256k1::PubKey const& pk
		     , Sha256::Hash const& m
		     ) const {
	std::uint8_t mbuf[32];
	m.to_buffer(mbuf);

	auto res = secp256k1_ecdsa_verify
		( context.get()
		, reinterpret_cast<const secp256k1_ecdsa_signature*>(data)
		, reinterpret_cast<const unsigned char*>(mbuf)
		, reinterpret_cast<const secp256k1_pubkey*>(pk.get_key())
		);
	return res != 0;
}

std::vector<std::uint8_t>
Signature::der_encode() const {
	auto dummy = (unsigned char)(0);
	/* Determine length first.  */
	auto len = size_t();
	auto res = secp256k1_ecdsa_signature_serialize_der
		( context.get()
		, &dummy
		, &len
		, reinterpret_cast<const secp256k1_ecdsa_signature*>(data)
		);
	assert(res == 0);

	/* Now serialize.  */
	auto rv = std::vector<std::uint8_t>(len);
	res = secp256k1_ecdsa_signature_serialize_der
		( context.get()
		, &rv[0]
		, &len
		, reinterpret_cast<const secp256k1_ecdsa_signature*>(data)
		);
	assert(res == 1);
	assert(len == rv.size());

	return rv;
}

Signature
Signature::der_decode(std::vector<std::uint8_t> const& d) {
	auto rv = Signature();
	auto res = secp256k1_ecdsa_signature_parse_der
		( context.get()
		, reinterpret_cast<secp256k1_ecdsa_signature*>(rv.data)
		, reinterpret_cast<unsigned char const*>(&d[0])
		, d.size()
		);
	/* In case of parse failure, the signature will never be valid
	 * for any pubkey+message.
	 */
	if (res)
		return rv;
	else
		return rv;
}

SchnorrSig::SchnorrSig(std::uint8_t const buffer[64]) {
	memcpy(&data[0], &buffer[0], 64);
}

SchnorrSig::SchnorrSig( Secp256k1::PrivKey const& sk
		    , Sha256::Hash const& m
		    ) {
	std::uint8_t mbuf[32];
	m.to_buffer(mbuf);
	std::uint8_t skbuf[32];
	sk.to_buffer(skbuf);

	secp256k1_keypair kp;
	auto res = secp256k1_keypair_create
		( context.get()
		, &kp
		, skbuf );

	if (!res)
		throw InvalidPrivKey();

	res = secp256k1_schnorrsig_sign32
			( context.get()
			, data
			, mbuf
			, &kp
			, nullptr ); //FIXME 32 random bytes
	if (!res)
		throw InvalidPrivKey();
}

SchnorrSig::SchnorrSig() {
	memset(data, 0, 64);
}

bool SchnorrSig::valid( Secp256k1::XonlyPubKey const& pk
		     , Sha256::Hash const& m
		     ) const {
	std::uint8_t mbuf[32];
	m.to_buffer(mbuf);
	size_t msglen = sizeof(mbuf);

	auto res = secp256k1_schnorrsig_verify
		( context.get()
		, reinterpret_cast<const unsigned char*>(data)
		, reinterpret_cast<const unsigned char*>(mbuf)
		, msglen
		, reinterpret_cast<const secp256k1_xonly_pubkey*>(pk.get_key())
		);
	return res != 0;
}

void SchnorrSig::to_buffer(std::uint8_t buffer[64]) const {
	memcpy(&buffer[0], &data[0], 64);
}
std::vector<std::uint8_t> SchnorrSig::to_buffer() const {
	auto buf = std::vector<std::uint8_t>(64);
	for (auto i{0}; i < 64; ++i)
		buf[i] = data[i];

	return buf;
}

}
