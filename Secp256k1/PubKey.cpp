#include<assert.h>
#include<basicsecure.h>
#include<iomanip>
#include<secp256k1.h>
#include<secp256k1_extrakeys.h>
#include<sstream>
#include<string>
#include<string.h>
#include<utility>
#include"Secp256k1/Detail/context.hpp"
#include"Secp256k1/G.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"

using Secp256k1::Detail::context;

namespace Secp256k1 {

class PubKey::Impl {
public:
	secp256k1_pubkey key;

	explicit Impl(Secp256k1::PrivKey const& sk) {
		auto res = secp256k1_ec_pubkey_create( context.get()
						     , &key
						     , sk.key
						     );
		/* The private key should have been verified.  */
		assert(res == 1);
	}
	Impl(secp256k1_context_struct *ctx, std::uint8_t buffer[33]) {
		auto res = secp256k1_ec_pubkey_parse( ctx
						    , &key
						    , buffer
						    , 33
						    );
		if (!res)
			throw InvalidPubKey();
	}
	Impl(std::uint8_t const buffer[33]) {
		auto res = secp256k1_ec_pubkey_parse( context.get()
						    , &key
						    , buffer
						    , 33
						    );
		if (!res)
			throw InvalidPubKey();
	}
	Impl() { }
	Impl(Impl const& o) {
		key = o.key;
	}

	void negate() {
		auto res = secp256k1_ec_pubkey_negate(context.get(), &key);
		assert(res == 1);
	}
	void add(Impl const& o) {
		secp256k1_pubkey tmp;

		secp256k1_pubkey const* keys[2] = {&o.key, &key};
		auto res = secp256k1_ec_pubkey_combine( context.get()
						      , &tmp
						      , keys
						      , 2
						      );
		/* FIXME: use a backtrace-prserving exception.  */
		if (!res)
			throw Util::BacktraceException<std::out_of_range>(
				"Secp256k1::PubKey::operatoor+=: "
				"result of adding PubKey out-of-range"
			);

		key = tmp;
	}
	void mul(Secp256k1::PrivKey const& sk) {
		/* Construct temporary. */
		auto tmp = key;

		auto res = secp256k1_ec_pubkey_tweak_mul( context.get()
							, &tmp
							, sk.key
							);
		if (!res)
			throw Util::BacktraceException<std::out_of_range>(
				"Secp256k1::PubKey::operatoor*=: "
				"result of multiplying PrivKey out-of-range"
			);
		/* swap. */
		key = tmp;
	}
	void tweakadd(const unsigned char tweak[32]) {
		auto res = secp256k1_ec_pubkey_tweak_add( context.get()
						      , &key
						      , tweak
						      );
		/* FIXME: use a backtrace-prserving exception.  */
		if (!res)
			throw std::out_of_range(
				"Secp256k1::PubKey::operator+=: "
				"result of tweak-adding PubKey out-of-range"
			);
	}

	void xonly_tweak( const unsigned char x_key[32]
					, const unsigned char tweak[32]
					) {
		int ret = secp256k1_xonly_pubkey_tweak_add
				( context.get()
				, &key
				, reinterpret_cast<const secp256k1_xonly_pubkey*>(x_key)
				, tweak
				);
		if (!ret)
			throw InvalidPubKey();
	}

	bool equal(Impl const& o) const {
		std::uint8_t a[33];
		std::uint8_t b[33];
		size_t asize = sizeof(a);
		size_t bsize = sizeof(b);

		auto resa = secp256k1_ec_pubkey_serialize( context.get()
							 , a
							 , &asize
							 , &key
							 , SECP256K1_EC_COMPRESSED
							 );
		assert(resa == 1);
		assert(asize == sizeof(a));

		auto resb = secp256k1_ec_pubkey_serialize( context.get()
							 , b
							 , &bsize
							 , &o.key
							 , SECP256K1_EC_COMPRESSED
							 );
		assert(resb == 1);
		assert(bsize == sizeof(b));

		return basicsecure_eq(a, b, sizeof(a));
	}

	void dump(std::ostream& os) {
		std::uint8_t a[33];
		size_t asize = sizeof(a);

		auto resa = secp256k1_ec_pubkey_serialize( context.get()
							 , a
							 , &asize
							 , &key
							 , SECP256K1_EC_COMPRESSED
							 );
		assert(resa == 1);
		assert(asize == sizeof(a));

		for (auto i = 0; i < 33; ++i) {
			os << Util::Str::hexbyte(a[i]);
		}
	}

	void to_buffer(std::uint8_t buffer[33]) const {
		size_t size = 33;
		auto resa = secp256k1_ec_pubkey_serialize( context.get()
							 , buffer
							 , &size
							 , &key
							 , SECP256K1_EC_COMPRESSED
							 );
		assert(resa == 1);
		assert(size == 33);
	}
};

/* Get G (with prepended tie-breaker byte).  */
PubKey::PubKey()
	: pimpl(Util::make_unique<Impl>(*G_tied.pimpl)) { }

PubKey::PubKey(secp256k1_context_struct *ctx, std::uint8_t buffer[33])
	: pimpl(Util::make_unique<Impl>(ctx, buffer)) {}
PubKey::PubKey(std::uint8_t const buffer[33])
	: pimpl(Util::make_unique<Impl>(buffer)) {}

PubKey::PubKey(std::string const& s) {
	auto buf = Util::Str::hexread(s);
	if (buf.size() != 33)
		/* FIXME: Use a specific invalid-public-key failure.  */
		throw InvalidPubKey();
	pimpl = Util::make_unique<Impl>(&buf[0]);
}
PubKey::operator std::string() const {
	auto os = std::ostringstream();
	pimpl->dump(os);
	return os.str();
}

PubKey::PubKey(Secp256k1::PrivKey const& sk)
	: pimpl(Util::make_unique<Impl>(sk)) {}

PubKey::PubKey(PubKey const& o)
	: pimpl(Util::make_unique<Impl>(*o.pimpl)) { }
PubKey::PubKey(PubKey&& o) {
	auto mine = Util::make_unique<Impl>();
	std::swap(pimpl, mine);
	std::swap(pimpl, o.pimpl);
}
PubKey::~PubKey() { }

void const* PubKey::get_key() const {
	return &pimpl->key;
}

void PubKey::negate() {
	pimpl->negate();
}

PubKey& PubKey::operator+=(PubKey const& o) {
	pimpl->add(*o.pimpl);
	return *this;
}

PubKey& PubKey::operator*=(PrivKey const& o) {
	pimpl->mul(o);
	return *this;
}

bool PubKey::operator==(PubKey const& o) const {
	return pimpl->equal(*o.pimpl);
}

PubKey PubKey::xonly_tweak( XonlyPubKey const& x_key
						  , const unsigned char tweak[32] ) {
	auto rv = PubKey();
	rv.pimpl->xonly_tweak( reinterpret_cast<const unsigned char*>(x_key.get_key())
						 , tweak );
	return rv;
}

void PubKey::to_buffer(std::uint8_t buffer[33]) const {
	pimpl->to_buffer(buffer);
}

}

std::ostream& operator<<(std::ostream& os, Secp256k1::PubKey const& pk) {
	pk.pimpl->dump(os);
	return os;
}

namespace Secp256k1 {

class XonlyPubKey::Impl {
public:
	secp256k1_xonly_pubkey key;

	Impl(std::uint8_t const buffer[32]) {
		auto res = secp256k1_xonly_pubkey_parse( context.get()
						    , &key
						    , buffer
						    );
		if (!res)
			throw InvalidPubKey();
	}
	Impl(secp256k1_context_struct *ctx, std::uint8_t buffer[32]) {
		auto res = secp256k1_xonly_pubkey_parse( ctx
						    , &key
						    , buffer
						    );
		if (!res)
			throw InvalidPubKey();
	}

	Impl() { }

	bool equal(Impl const& o) const {
		std::uint8_t a[32];
		std::uint8_t b[32];

		auto resa = secp256k1_xonly_pubkey_serialize( context.get()
							 , a
							 , &key
							 );
		assert(resa == 1);

		auto resb = secp256k1_xonly_pubkey_serialize( context.get()
							 , b
							 , &o.key
							 );
		assert(resb == 1);

		return basicsecure_eq(a, b, sizeof(a));
	}
};

/* Get G (the 32 byte x coordinate).  */
XonlyPubKey::XonlyPubKey()
	: pimpl(Util::make_unique<Impl>(*G_xcoord.pimpl)) { }

XonlyPubKey::XonlyPubKey(std::string const& s) {
	auto buf = Util::Str::hexread(s);
	if (buf.size() != 32)
		throw InvalidPubKey();
	pimpl = Util::make_unique<Impl>(&buf[0]);
}
XonlyPubKey::XonlyPubKey( secp256k1_context_struct *ctx
						, std::uint8_t buffer[32])
	: pimpl(Util::make_unique<Impl>(ctx, buffer)) {}
XonlyPubKey::XonlyPubKey(std::uint8_t const buffer[32])
	: pimpl(Util::make_unique<Impl>(buffer)) {}


XonlyPubKey::XonlyPubKey(XonlyPubKey const& o)
	: pimpl(Util::make_unique<Impl>(*o.pimpl)) { }

XonlyPubKey::XonlyPubKey(XonlyPubKey&& o) {
	auto mine = Util::make_unique<Impl>();
	std::swap(pimpl, mine);
	std::swap(pimpl, o.pimpl);
}

XonlyPubKey::~XonlyPubKey() { }

void const* XonlyPubKey::get_key() const {
	return &pimpl->key;
}

bool XonlyPubKey::operator==(XonlyPubKey const& o) const {
	return pimpl->equal(*o.pimpl);
}

void
XonlyPubKey::to_buffer(std::uint8_t buffer[32]) const {
	auto res = secp256k1_xonly_pubkey_serialize
		( context.get()
		, reinterpret_cast<unsigned char*>(buffer)
		, &(pimpl->key) );
	assert(res == 1);
}

XonlyPubKey
XonlyPubKey::from_ecdsa_pk(PubKey const& ecpk) {
	return from_ecdsa_pk(reinterpret_cast<std::uint8_t const*>(ecpk.get_key()));
}

XonlyPubKey
XonlyPubKey::from_ecdsa_pk(std::uint8_t const ecpk_buf[33]) {
	auto rv = XonlyPubKey();
	int parity;
	auto res = secp256k1_xonly_pubkey_from_pubkey
				( context.get()
				, (secp256k1_xonly_pubkey*) &(rv.pimpl->key)
				, &parity
				, (const secp256k1_pubkey*) ecpk_buf
				);
	assert(res == 1);
	return rv;
}

}

