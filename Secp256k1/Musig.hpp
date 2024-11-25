#ifndef SECP256K1_MUSIG_HPP
#define SECP256K1_MUSIG_HPP

#include"Sha256/Hash.hpp"
#include"Secp256k1/KeyPair.hpp"
#include<memory>
#include<vector>
#include<stdexcept>
#include<string>

namespace Secp256k1 { class PrivKey; }
namespace Secp256k1 { class PubKey; }
namespace Secp256k1 { class Random; }

namespace Secp256k1 { namespace Musig {

/* Thrown in case of being fed an invalid argument.  */
class InvalidArg : public std::invalid_argument {
public:
	InvalidArg(std::string arg)
		: std::invalid_argument("Invalid argument: " +arg) { }
};

/** musig dance
 *    - order of the steps is partly flexible (e.g. steps 4-5 and 6-7 can
 *      happen in any order)
 *    - steps 1:a and 1:b are for bip341 script trees only
 *
 * 1) aggregate pubkeys of all participants
 *    :a) compute script tree root hash (comprising bip340 tagged hashes of
 *        all script leaves and branches)
 *    :b) apply script tree root hash as tweak to internal key (which is
 *        the aggregate key produced in step 1)
 * 2) compute sighash according to bip341
 * 3) create local sec+pub nonces
 * 4) send local pub nonce to all participants
 * 5) receive remote pubnonces, aggregate all into a single pubnonce
 * 6) sign local part sig
 * 7) receive partial sigs from all participants
 * 8) aggregate all partial sigs into a single schnorr sig
 */

class Session {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

protected:
	Session( Secp256k1::KeyPair const& signing_keys
		   , std::vector<Secp256k1::PubKey> const& pkvec
		   );

	Session();
	Session(Session&&);
	Session& operator= (Session&&);

	/* disallow copys to prevent the secnonce proliferating.  */
	Session(Session const&) =delete;
	Session& operator=(Session const&) =delete;

	void load_partial_at(std::string const&, PubKey const&);
	void load_pubnonce_at(std::string const&, PubKey const&);

public:
	virtual ~Session();

	static
	XonlyPubKey pubkey_aggregate(std::vector<PubKey> const&);

	void apply_xonly_tweak(std::vector<std::uint8_t> const& scripthash);
	void load_sighash(Sha256::Hash const&);
	std::string generate_local_nonces(Secp256k1::Random&);
	void aggregate_pubnonces();
	void verify_part_sigs();
	void sign_partial();
	std::string serialize_partial();
	void aggregate_partials(std::uint8_t aggsigbuf[64]);
	Sha256::Hash get_sighash();
	XonlyPubKey get_internal_key();
	XonlyPubKey const& get_output_key();
};

} }

#endif /* SECP256K1_MUSIG_HPP */
