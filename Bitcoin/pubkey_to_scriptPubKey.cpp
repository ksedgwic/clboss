#include"Bitcoin/pubkey_to_scriptPubKey.hpp"
#include"Secp256k1/PubKey.hpp"

namespace Bitcoin {
#if 0
std::vector<uint8_t>
pk_to_scriptpk(Secp256k1::PubKey const& key) {
	auto rv = std::vector<std::uint8_t>(21);
	rv[0] = 0x00; /* OP_0, denoting segwit v0 */
	rv[1] = 0x14; /* varint for P2WPKH */
	key.to_buffer(&rv[2]);
	return rv;
}
#endif
std::vector<uint8_t>
pk_to_scriptpk(Secp256k1::PubKey const& key) {
	auto rv = std::vector<std::uint8_t>(34);
	rv[0] = 0x00; /* OP_0, denoting segwit v0 */
	rv[1] = 0x20; /* varint for P2WSH pubkey */
	key.to_buffer(&rv[2]);
	return rv;
}

std::vector<uint8_t>
pk_to_scriptpk(Secp256k1::XonlyPubKey const& key) {
	auto rv = std::vector<std::uint8_t>(34);
	rv[0] = 0x51; /* OP_1, denoting segwit v1 */
	rv[1] = 0x20; /* varint for taproot pubkey */
	key.to_buffer(&rv[2]);
	return rv;
}

}
