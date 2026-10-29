#ifndef BITCOIN_PUBKEY_TO_SCRIPTPUBKEY_HPP
#define BITCOIN_PUBKEY_TO_SCRIPTPUBKEY_HPP

#include<cstdint>
#include<string>
#include<vector>

namespace Secp256k1 { class XonlyPubKey; }

namespace Bitcoin {

/* The P2TR scriptPubKey paying to the given output key.  */
std::vector<uint8_t> pk_to_scriptpk(Secp256k1::XonlyPubKey const&);

}

#endif /* !defined(BITCOIN_PUBKEY_TO_SCRIPTPUBKEY_HPP) */
