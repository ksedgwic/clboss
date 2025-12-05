#ifndef SECP256K1_SIGNERIF_HPP
#define SECP256K1_SIGNERIF_HPP

#include<vector>
#include<cstdint>

namespace Secp256k1 { class PrivKey; }
namespace Secp256k1 { class PubKey; }
namespace Secp256k1 { class KeyPair; }
namespace Secp256k1 { class Signature; }
namespace Sha256 { class Hash; }

namespace Secp256k1 {

/** class Secp256k1::SignerIF
 *
 * @brief abstract class representing an
 * object that returns signatures.
 *
 * @desc Notice that a tweak is multiplied
 * with the privkey.
 * The intent is that users will own the
 * actual private key in some separated
 * file, while database data might need to
 * be sent to developers for debugging.
 * THe database data would contains the
 * tweak, but not the actual private key,
 * thus developers will still not be able
 * to steal possibly-valuable keys.
 */
class SignerIF {
public:
	virtual ~SignerIF() { }

	/* Get the public key of the corresponding tweak.  */
	virtual
	Secp256k1::PubKey
	get_pubkey_tweak(Secp256k1::PrivKey const& tweak) =0;

	/* Get a signature using the signer privkey times
	 * the given tweak.
	 */
	virtual
	Secp256k1::Signature
	get_signature_tweak( Secp256k1::PrivKey const& tweak
			   , Sha256::Hash const& m
			   ) =0;

	/* Combine the given salt with the signer privkey
	 * to get a hash.
	 */
	virtual
	Sha256::Hash
	get_privkey_salted_hash( std::uint8_t salt[32]
			       ) =0;

	/* Get the keypair corresponding to a given tweak.  */
	virtual
	Secp256k1::KeyPair
	get_keypair_tweak(Secp256k1::PrivKey const& tweak) =0;

};

}

#endif /* !defined(SECP256K1_SIGNERIF_HPP) */
