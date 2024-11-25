#include"Secp256k1/Musig.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Random.hpp"
#include"Secp256k1/Detail/context.hpp"
#include"Secp256k1/tagged_hashes.hpp"
#include"Util/make_unique.hpp"
#include"Util/Str.hpp"
#include<basicsecure.h>
#include<secp256k1.h>
#include<secp256k1_musig.h>
#include<assert.h>
#include<stdlib.h>
#include<string.h>
#include<sstream>

using Secp256k1::Detail::context;

namespace Secp256k1 { namespace Musig {

namespace {

struct Signatory {
	Secp256k1::PubKey public_key;
	std::uint8_t pubnonce[132]{}; /* secp256k1_musig_pubnonce */
	std::uint8_t part_sig[36]{}; /* secp256k1_musig_partial_sig */
	bool pubnonce_loaded{false};
	bool partial_loaded{false};
};

}

class Session::Impl {
private:
	std::vector<Signatory> signers;
	Signatory& local;
	std::uint8_t signing_key[32];
	XonlyPubKey aggregate_pk;
	secp256k1_musig_keyagg_cache aggkey_cache{};
	XonlyPubKey output_key;
	std::uint8_t sighash[32];
	// TODO should we mlock the memory allocated for secnonce? does mlock
	// port to other unix clones?
	secp256k1_musig_secnonce secnonce{};
	secp256k1_musig_aggnonce aggnonce{};
	secp256k1_musig_session session{};
	bool sighash_loaded{false};

public:
	Impl(Impl&&) =default;
	Impl& operator=(Impl&&) =default;

	Impl( std::vector<Signatory> signers_
		, Signatory& local_
		, Secp256k1::KeyPair const& keys
		) : signers(std::move(signers_))
		  , local(local_)
	{
		/* copy signing key.  */
		keys.priv().to_buffer(signing_key);

		const secp256k1_pubkey *pkarr[signers.size()];

		for (size_t i{0}; i < signers.size(); ++i)
			pkarr[i] = reinterpret_cast<const secp256k1_pubkey*>(signers[i].public_key.get_key());

		aggregate_pk = pubkey_aggregate( &aggkey_cache
									   , pkarr
									   , signers.size()
									   );
	}

	/* aggregate local/external pubkeys, and (if not nullptr),
	 * initialize given secp256k1 musig aggkey cache.  */
	static
	XonlyPubKey pubkey_aggregate( secp256k1_musig_keyagg_cache *agg_cache
								, const secp256k1_pubkey **pkarr
								, size_t nKeys
								) {
		std::uint8_t xkey_buf[64];
		auto res = secp256k1_musig_pubkey_agg
			( context.get()
			, reinterpret_cast<secp256k1_xonly_pubkey*>(xkey_buf)
			, agg_cache
			, pkarr
			, nKeys
			);

		if (!res)
			throw InvalidArg("Session::Impl::pubkey_aggregate secp256k1_musig_pubkey_agg");

		std::uint8_t serialized_xkey[32];
		res = secp256k1_xonly_pubkey_serialize
			( context.get()
			, reinterpret_cast<unsigned char*>(serialized_xkey)
			, reinterpret_cast<const secp256k1_xonly_pubkey*>(xkey_buf)
			);

		return XonlyPubKey::from_buffer(serialized_xkey);
	}

	void serialize_aggkey(std::uint8_t buf[32]) {
		aggregate_pk.to_buffer(buf);
	}

	void
	xonly_tweak( std::uint8_t aggkey_serialized[32]
			   , std::vector<std::uint8_t> const& scripthash ) {
		unsigned char msg[64];
		memcpy(&msg[0], &aggkey_serialized[0], 32);
		memcpy(&msg[32], scripthash.data(), 32);

		unsigned char tweak[32];
		Secp256k1::tagged_hash( tweak
							  , msg
							  , std::size_t{64}
							  , Tag::TWEAK );

		secp256k1_pubkey ecpk;
		int res = secp256k1_musig_pubkey_xonly_tweak_add
				( context.get()
				, &ecpk
				, &aggkey_cache
				, reinterpret_cast<const unsigned char*>(tweak)
				);
		if (!res)
			throw InvalidArg("Tweak::Impl::tweak_musig secp256k1_musig_pubkey_xonly_tweak_add");

		output_key = XonlyPubKey::from_ecdsa_pk(reinterpret_cast<const std::uint8_t*>(&ecpk));
	}

	void load_sighash(Sha256::Hash const& sh) {
		sh.to_buffer(sighash);
		sighash_loaded = true;
	}

	std::string
	generate_local_nonces(Secp256k1::Random& random) {
		if (!sighash_loaded)
			throw InvalidArg("Session::generate_local_nonces sighash not loaded");

		unsigned char secrand[32];
		for (auto i{0}; i < 32; ++i)
			secrand[i] = random.get();

		unsigned char extra32[32];
		for (auto i{0}; i < 32; ++i)
			extra32[i] = random.get();

		auto *p_nonce = reinterpret_cast<secp256k1_musig_pubnonce*>(&(local.pubnonce));
		auto res = secp256k1_musig_nonce_gen
			( context.get()
			// TODO should we mlock the memory allocated for secnonce? does mlock
			// port to other unix clones?
			, &secnonce
			, p_nonce
			, secrand
			, signing_key
			, reinterpret_cast<const secp256k1_pubkey*>(local.public_key.get_key())
			, reinterpret_cast<const unsigned char*>(sighash) /* msg32 */
			, reinterpret_cast<const secp256k1_musig_keyagg_cache*>(&aggkey_cache)
			// FIXME unclear on what's best practice, but if the rng is deficient, why use it twice?
			, reinterpret_cast<const unsigned char*>(extra32)	/* extra32 */
			);

		if (!res)
			throw InvalidArg("Session::generate_local_nonces secp256k1_musig_nonce_gen");

		local.pubnonce_loaded = true;
		return dump_pubnonce(p_nonce);
	}

	void load_pubnonce_at( std::string const& pubnonce
						 , Signatory& signer ) {
		auto buf = Util::Str::hexread(pubnonce);
		if (buf.size() != 66) /* <-- size of a serialized pubnonce.  */
			throw InvalidArg("bad size secp256k1_musig_pubnonce");

		auto res = secp256k1_musig_pubnonce_parse
			( context.get()
			, reinterpret_cast<secp256k1_musig_pubnonce*>(&(signer.pubnonce))
			, buf.data()
			);

		if (res != 1)
			throw InvalidArg("Session::Impl::Impl secp256k1_musig_pubnonce_parse");

		signer.pubnonce_loaded = true;
	}

	/* aggregate our public nonce with the counterpartys' public nonce */
	void aggregate_pubnonces() {
		if (!sighash_loaded)
			throw InvalidArg("Session::aggregate_nonces sighash not loaded");

		secp256k1_musig_pubnonce *pubnoncearr[signers.size()];

		size_t i = 0;
		for (auto it = signers.begin(); it != signers.end(); ++it, ++i) {
			if (!it->pubnonce_loaded) //TODO pubkey of the offender in the error msg?
				throw InvalidArg("Session::aggregate_pubnonces missing or invalid pubnonce(s)");

			pubnoncearr[i] = reinterpret_cast<secp256k1_musig_pubnonce*>(it->pubnonce);
		}

		auto res = secp256k1_musig_nonce_agg
			( context.get()
			, &aggnonce
			, pubnoncearr
			, signers.size() );

		if (!res)
			throw InvalidArg("Session::Impl::Impl secp256k1_musig_nonce_agg");

		/* construct session */
		res = secp256k1_musig_nonce_process
			( context.get()
			, &session
			, &aggnonce
			, sighash
			, reinterpret_cast<const secp256k1_musig_keyagg_cache*>(&aggkey_cache)
			);

		if (!res)
			throw InvalidArg("Session::Impl::Impl secp256k1_musig_nonce_process");
	}

	void load_partial_at( std::string const& part_sig
						, Signatory& signer ) {
		auto buf = Util::Str::hexread(part_sig);
		if (buf.size() != 32) /* <-- size of a serialized partial sig.  */
			throw InvalidArg("bad size PartialSig");

		auto res = secp256k1_musig_partial_sig_parse
			( context.get()
			, reinterpret_cast<secp256k1_musig_partial_sig*>(&(signer.part_sig))
			, reinterpret_cast<const unsigned char*>(&buf[0])
			);
		if (res == 0)
			throw InvalidArg("secp256k1_musig_partial_sig_parse");

		signer.partial_loaded = true;
	}

	void verify_part_sigs() {
		auto res = 0;
		for (auto it = signers.begin(); it != signers.end(); ++it) {
			if (!it->partial_loaded) //TODO pubkey of the offender in the error msg?
				throw InvalidArg("Session::verify_part_sigs missing or invalid partial sig(s)");

			res = secp256k1_musig_partial_sig_verify
				( context.get()
				, reinterpret_cast<const secp256k1_musig_partial_sig*>(&(it->part_sig))
				, reinterpret_cast<const secp256k1_musig_pubnonce*>(&(it->pubnonce))
				, reinterpret_cast<const secp256k1_pubkey*>(it->public_key.get_key())
				, reinterpret_cast<const secp256k1_musig_keyagg_cache*>(&aggkey_cache)
				, &session
				);

			if (res == 0)
				throw InvalidArg("secp256k1_musig_partial_sig_verify");
		}
	}

	void sign_partial() {
		secp256k1_keypair kp;
		auto res = secp256k1_keypair_create
			( context.get()
			, &kp
			, signing_key );

		if (!res)
			throw InvalidArg("Session::sign_partial secp256k1_keypair_create");

		res = secp256k1_musig_partial_sign
			( context.get()
			, reinterpret_cast<secp256k1_musig_partial_sig*>(&(local.part_sig))
			, &secnonce
			, reinterpret_cast<const secp256k1_keypair*>(&kp)
			, reinterpret_cast<const secp256k1_musig_keyagg_cache*>(&aggkey_cache)
			, &session
			);

		if (!res)
			throw InvalidArg("Session::sign_partial secp256k1_musig_partial_sign");

		local.partial_loaded = true;
	}

	std::string serialize_partial() {
		std::uint8_t buf[32];
		auto res = secp256k1_musig_partial_sig_serialize
			( context.get()
			, reinterpret_cast<unsigned char*>(&buf[0])
			, reinterpret_cast<const secp256k1_musig_partial_sig*>(&(local.part_sig))
			);
		if (res == 0)
			throw InvalidArg("secp256k1_musig_partial_sig_serialize");

		auto os = std::ostringstream();
		for (auto i{0}; i < 32; ++i)
			os << Util::Str::hexbyte(buf[i]);

		return os.str();
	}

	void /* aggregate the partial sigs */
	aggregate_partials(std::uint8_t aggsigbuf[64]) {
		const secp256k1_musig_partial_sig *sig_array[signers.size()];

		auto i = 0;
		for (auto it = signers.begin(); it != signers.end(); ++it, ++i)
			sig_array[i] = reinterpret_cast<const secp256k1_musig_partial_sig*>(&(it->part_sig));

		auto res = secp256k1_musig_partial_sig_agg
			( context.get()
			, aggsigbuf
			, &session
			, reinterpret_cast<const secp256k1_musig_partial_sig* const*>(sig_array)
			, signers.size()
			);

		if (!res)
			throw InvalidArg("Session::aggregate_partials secp256k1_partial_sig_agg");
	}

	std::string dump_pubnonce(secp256k1_musig_pubnonce *p_nonce) {
		std::uint8_t buf[66];

		auto res = secp256k1_musig_pubnonce_serialize
					( context.get()
					, buf
					, p_nonce
					);
		assert(res == 1);

		auto os = std::ostringstream();
		for (auto i{0}; i < 66; ++i)
			os << Util::Str::hexbyte(buf[i]);

		return os.str();
	}

	Signatory& find_signer(PubKey const& pk) {
		auto it = signers.begin();
		for ( ; it != signers.end(); ++it)
			if (pk == it->public_key) break;

		if (it == signers.end()) //TODO throw out of range?
			throw InvalidArg("Musig::Session::Impl find_signer");

		return *it;
	}

	Sha256::Hash get_sighash() {
		Sha256::Hash rv;
		rv.from_buffer(sighash);
		return rv;
	}

	XonlyPubKey internal_key() {
		return aggregate_pk;
	}

	XonlyPubKey const& get_output_key() {
		return output_key;
	}
};

Session::Session( Secp256k1::KeyPair const& keys
				, std::vector<Secp256k1::PubKey> const& pkvec
				)
{
	auto signatory = Signatory();
	auto signers = std::vector<Signatory>(pkvec.size());
	auto it_local = signers.end();

	for (size_t i{0}; i < pkvec.size(); ++i) {
		signatory.public_key = pkvec[i];
		signers[i] = std::move(signatory);

		if (keys.pub() == pkvec[i])
			it_local = signers.begin() +i;
	}

	if (it_local == signers.end())
		throw InvalidArg("Musig::Session::Session local pubkey arg missing");

	pimpl = std::make_unique<Impl>
		( std::move(signers)
		, *it_local
		, keys
		);
}

/* useful only for class or namespace level Session objects that do not
 * have the data available to fully construct when they are instantiated.  */
Session::Session() : pimpl(nullptr) { }

Session::~Session() =default;
Session::Session(Session&&) =default;
Session& Session::operator= (Session&&) =default;

XonlyPubKey
Session::pubkey_aggregate(std::vector<PubKey> const& signers) {
	const secp256k1_pubkey *pkarr[signers.size()];

	for (size_t i{0}; i < signers.size(); ++i)
		pkarr[i] = reinterpret_cast<const secp256k1_pubkey*>(signers[i].get_key());

	return Impl::pubkey_aggregate(nullptr, pkarr, signers.size());
}

void
Session::apply_xonly_tweak(std::vector<std::uint8_t> const& scripthash) {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	std::uint8_t buf[32];
	pimpl->serialize_aggkey(buf);
	pimpl->xonly_tweak(buf, scripthash);
}

void Session::load_sighash(Sha256::Hash const& sh) {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	return pimpl->load_sighash(sh);
}

std::string
Session::generate_local_nonces(Secp256k1::Random& random) {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	return pimpl->generate_local_nonces(random);
}

void Session::load_pubnonce_at( std::string const& pn
							  , PubKey const& pk ) {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	auto& signer = pimpl->find_signer(pk);
	pimpl->load_pubnonce_at(pn, signer);
}

void Session::aggregate_pubnonces() {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	pimpl->aggregate_pubnonces();
}

void Session::load_partial_at( std::string const& ps
							 , PubKey const& pk ) {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	auto& signer = pimpl->find_signer(pk);
	pimpl->load_partial_at(ps, signer);
}

void Session::verify_part_sigs() {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	pimpl->verify_part_sigs();
}

void Session::sign_partial() {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	return pimpl->sign_partial();
}

std::string Session::serialize_partial() {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	return pimpl->serialize_partial();
}

void
Session::aggregate_partials(std::uint8_t buffer[64]) {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	pimpl->aggregate_partials(buffer);
}

Sha256::Hash Session::get_sighash() {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	return pimpl->get_sighash();
}

XonlyPubKey Session::get_internal_key() {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	return pimpl->internal_key();
}

XonlyPubKey const& Session::get_output_key() {
	if (!pimpl)
		throw InvalidArg("Musig::Session object uninitialized");

	return pimpl->get_output_key();
}

} }
