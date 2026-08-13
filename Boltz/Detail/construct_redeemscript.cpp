#include"Boltz/Detail/construct_redeemscript.hpp"
#include"Ripemd160/Hash.hpp"
#include"Secp256k1/PubKey.hpp"

namespace {

/* Bitcoin Script opcodes for the reverse submarine swap HTLC template.
 * Same template as Electrum WITNESS_TEMPLATE_SWAP and
 * boltz-core swapScript().
 */
constexpr std::uint8_t OP_SIZE                = 0x82;
constexpr std::uint8_t OP_EQUAL               = 0x87;
constexpr std::uint8_t OP_IF                  = 0x63;
constexpr std::uint8_t OP_HASH160             = 0xa9;
constexpr std::uint8_t OP_EQUALVERIFY         = 0x88;
constexpr std::uint8_t OP_ELSE                = 0x67;
constexpr std::uint8_t OP_DROP                = 0x75;
constexpr std::uint8_t OP_CHECKLOCKTIMEVERIFY = 0xb1;
constexpr std::uint8_t OP_ENDIF               = 0x68;
constexpr std::uint8_t OP_CHECKSIG            = 0xac;
constexpr std::uint8_t PUSHBYTE_1             = 0x01;
constexpr std::uint8_t PUSHBYTE_3             = 0x03;
constexpr std::uint8_t PUSHBYTE_20            = 0x14;
constexpr std::uint8_t PUSHBYTE_32            = 0x20;
constexpr std::uint8_t PUSHBYTE_33            = 0x21;

}

namespace Boltz { namespace Detail {

std::vector<std::uint8_t>
construct_redeemscript( Ripemd160::Hash const& hash160
		      , Secp256k1::PubKey const& pubkey_hash
		      , std::uint32_t locktime
		      , Secp256k1::PubKey const& pubkey_locktime
		      ) {
	auto expected = std::vector<std::uint8_t>();
	expected.push_back(OP_SIZE);
	expected.push_back(PUSHBYTE_1);
	expected.push_back(PUSHBYTE_32);
	expected.push_back(OP_EQUAL);
	expected.push_back(OP_IF);
	expected.push_back(OP_HASH160);
	expected.push_back(PUSHBYTE_20);
	{
		std::uint8_t buf[20];
		hash160.to_buffer(buf);
		expected.insert(expected.end(), buf, buf + 20);
	}
	expected.push_back(OP_EQUALVERIFY);
	expected.push_back(PUSHBYTE_33);
	{
		std::uint8_t buf[33];
		pubkey_hash.to_buffer(buf);
		expected.insert(expected.end(), buf, buf + 33);
	}
	expected.push_back(OP_ELSE);
	expected.push_back(OP_DROP);
	expected.push_back(PUSHBYTE_3);
	expected.push_back(std::uint8_t(locktime & 0xFF));
	expected.push_back(std::uint8_t((locktime >> 8) & 0xFF));
	expected.push_back(std::uint8_t((locktime >> 16) & 0xFF));
	expected.push_back(OP_CHECKLOCKTIMEVERIFY);
	expected.push_back(OP_DROP);
	expected.push_back(PUSHBYTE_33);
	{
		std::uint8_t buf[33];
		pubkey_locktime.to_buffer(buf);
		expected.insert(expected.end(), buf, buf + 33);
	}
	expected.push_back(OP_ENDIF);
	expected.push_back(OP_CHECKSIG);
	return expected;
}

}}
