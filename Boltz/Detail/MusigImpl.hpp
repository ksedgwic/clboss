#ifndef BOLTZ_MUSIG_IMPL_HPP
#define BOLTZ_MUSIG_IMPL_HPP

#include"Secp256k1/Musig.hpp"
#include"Secp256k1/SignerIF.hpp"

using namespace Secp256k1;

namespace Boltz {

class MusigSession : public Secp256k1::Musig::Session {
public:
	Secp256k1::PubKey boltzpub;

	MusigSession() : Secp256k1::Musig::Session() { }

	MusigSession(MusigSession&&) =default;
	MusigSession& operator= (MusigSession&&) =default;

	/* disallow copys to prevent the secnonce proliferating.  */
	MusigSession(MusigSession const&) =delete;
	MusigSession& operator=(MusigSession const&) =delete;

	/* Boltz API v2 employs role-based sorting for pubkey
	 * aggregation: their key first, client key second.
	 * We implement Musig::Session ctor and the load_partial
	 * function to reflect that.  */
	MusigSession( Secp256k1::PrivKey const& tweak
				, Secp256k1::PubKey const& boltzpub_
				, Secp256k1::SignerIF& signer)
		: Musig::Session( signer.get_keypair_tweak(tweak)
						, {boltzpub_, signer.get_pubkey_tweak(tweak)}
						)
		, boltzpub(boltzpub_)
	{ }

	void load_partial(std::string const& psig) {
		load_partial_at(psig, boltzpub);
	}
	void load_pubnonce(std::string const& pnonce) {
		load_partial_at(pnonce, boltzpub);
	}
};

}

#endif /* !defined(BOLTZ_MUSIG_IMPL_HPP) */
