#ifndef SECP256K1_PUBKEY_HPP
#define SECP256K1_PUBKEY_HPP

#include"Util/BacktraceException.hpp"
#include<cstdint>
#include<functional>
#include<istream>
#include<memory>
#include<ostream>
#include<stdexcept>
#include<string>
#include<utility>
#include<vector>

extern "C" {
struct secp256k1_context_struct;
}

namespace Secp256k1 { class PrivKey; }
namespace Secp256k1 { class PubKey; }
namespace Secp256k1 { class XonlyPubKey; }
namespace Secp256k1 { class Signature; }
namespace Secp256k1 { class SchnorrSig; }
namespace Secp256k1 { namespace Musig { class Session; } }

std::ostream& operator<<(std::ostream&, Secp256k1::PubKey const&);

namespace Secp256k1 {

/* Thrown in case of being fed an invalid public key.  */
class InvalidPubKey : public Util::BacktraceException<std::invalid_argument> {
public:
	InvalidPubKey() : Util::BacktraceException<std::invalid_argument>("Invalid public key.") { }
};

class PubKey {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

	explicit PubKey(secp256k1_context_struct *, std::uint8_t buffer[33]);
	explicit PubKey(std::uint8_t const buffer[33]);

	/* Used by Signature::valid.  */
	void const* get_key() const;

public:
	/* Get G (with prepended tie-breaker byte).  */
	PubKey();
	/* Load public key from a hex-encoded string.  */
	explicit PubKey(std::string const&);
	/* Create hex-encoded string.  */
	explicit operator std::string() const;
	/* Get the public key behind the given private key.  */
	explicit PubKey(Secp256k1::PrivKey const&);

	/* Copy an existing public key.  */
	PubKey(PubKey const&);
	PubKey(PubKey&&);

	~PubKey();

	PubKey& operator=(PubKey const& o) {
		auto tmp = PubKey(o);
		tmp.pimpl.swap(pimpl);
		return *this;
	}
	PubKey& operator=(PubKey&& o) {
		auto tmp = PubKey(std::move(o));
		tmp.pimpl.swap(pimpl);
		return *this;
	}

	void negate();
	PubKey operator-() const {
		auto tmp = *this;
		tmp.negate();
		return tmp;
	}

	PubKey& operator+=(PubKey const&);
	PubKey operator+(PubKey const& o) const {
		auto tmp = *this;
		tmp += o;
		return tmp;
	}

	PubKey& operator-=(PubKey const& o) {
		return (*this += -o);
	}
	PubKey operator-(PubKey const& o) const {
		auto tmp = o;
		tmp.negate();
		tmp += *this;
		return tmp;
	}

	PubKey& operator*=(PrivKey const&);
	PubKey operator*(PrivKey const& o) const {
		auto tmp = *this;
		tmp *= o;
		return tmp;
	}

	bool operator==(PubKey const&) const;
	bool operator!=(PubKey const& o) const {
		return !(*this == o);
	}

	friend std::ostream& ::operator<<(std::ostream&, PubKey const&);

	static PubKey xonly_tweak( XonlyPubKey const&
							 , const unsigned char tweak[32] );

	static PubKey from_buffer(std::uint8_t const buffer[33]) {
		return PubKey(buffer);
	}
	/* Needed for the generator point.  */
	static PubKey from_buffer_with_context( secp256k1_context_struct *ctx
					      , std::uint8_t buffer[33]
					      ) {
		return PubKey(ctx, buffer);
	}

	void to_buffer(std::uint8_t buffer[33]) const;

	friend class Secp256k1::Signature;
	friend class Secp256k1::XonlyPubKey;
	friend class Secp256k1::Musig::Session;
};

inline
PubKey operator*(PrivKey const& a, PubKey const& B) {
	return B * a;
}

// TODO class template?
class XonlyPubKey {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

	explicit XonlyPubKey( secp256k1_context_struct *
						, std::uint8_t buffer[32]);
	explicit XonlyPubKey(std::uint8_t const buffer[32]);

	/* Used by SchnorrSig::valid.  */
	void const* get_key() const;

public:
	/* Get G (the 32 byte x coordinate).  */
	XonlyPubKey();
	/* Load public key from a hex-encoded string.  */
	explicit XonlyPubKey(std::string const&);

	/* Copy an existing public key.  */
	XonlyPubKey(XonlyPubKey const&);
	XonlyPubKey(XonlyPubKey&&);

	~XonlyPubKey();

	XonlyPubKey& operator=(XonlyPubKey const& o) {
		auto tmp = XonlyPubKey(o);
		tmp.pimpl.swap(pimpl);
		return *this;
	}
	XonlyPubKey& operator=(XonlyPubKey&& o) {
		auto tmp = XonlyPubKey(std::move(o));
		tmp.pimpl.swap(pimpl);
		return *this;
	}

	bool operator==(XonlyPubKey const&) const;
	bool operator!=(XonlyPubKey const& o) const {
		return !(*this == o);
	}

	void to_buffer(std::uint8_t buffer[32]) const;

	/* find the x-only equivalent key from a 33 byte ecdsa key. */
	static XonlyPubKey from_ecdsa_pk(PubKey const&);
	static XonlyPubKey from_ecdsa_pk(std::uint8_t const buffer[33]);

	static XonlyPubKey from_buffer(std::uint8_t const buffer[32]) {
		return XonlyPubKey(buffer);
	}
	/* Needed for the generator point.  */
	static XonlyPubKey from_buffer_with_context( secp256k1_context_struct *ctx
					      , std::uint8_t buffer[32]
					      ) {
		return XonlyPubKey(ctx, buffer);
	}

	friend class Secp256k1::PubKey;
	friend class Secp256k1::SchnorrSig;
};

}

#endif /* SECP256K1_PUBKEY_HPP */
