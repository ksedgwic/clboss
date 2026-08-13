#undef NDEBUG
#include"Boltz/Detail/construct_redeemscript.hpp"
#include"Boltz/Detail/match_lockscript.hpp"
#include"Ripemd160/Hash.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Util/Str.hpp"
#include<assert.h>

/* The valid script from test_match_lockscript.cpp (locktime 649365 = 0x09E803
 * little-endian: 03 E8 09).
 */
static auto const valid_hex = std::string(
	"8201208763a9142c2d5441ef4a4469eae063941463e3c65ee926c088"
	"210207262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779"
	"67750395e809b17521"
	"030a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529e"
	"68ac"
);

int main() {
	auto const hash160 = Ripemd160::Hash(
		"2c2d5441ef4a4469eae063941463e3c65ee926c0"
	);
	auto const pubkey_hash = Secp256k1::PubKey(
		"0207262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779"
	);
	auto const locktime = std::uint32_t(649365);
	auto const pubkey_locktime = Secp256k1::PubKey(
		"030a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529e"
	);

	/* construct_redeemscript must reproduce the exact bytes of the
	 * known-valid script that match_lockscript also accepts.
	 */
	auto script = Boltz::Detail::construct_redeemscript
		( hash160
		, pubkey_hash
		, locktime
		, pubkey_locktime
		);
	auto expected = Util::Str::hexread(valid_hex);
	assert(script == expected);

	/* Round-trip: match_lockscript must accept the script we just built.  */
	auto rt_hash = Ripemd160::Hash();
	auto rt_pkh = Secp256k1::PubKey();
	auto rt_lt = std::uint32_t();
	auto rt_pkt = Secp256k1::PubKey();
	auto ok = Boltz::Detail::match_lockscript
		( rt_hash, rt_pkh, rt_lt, rt_pkt, script );
	assert(ok);
	assert(rt_hash == hash160);
	assert(rt_pkh == pubkey_hash);
	assert(rt_lt == locktime);
	assert(rt_pkt == pubkey_locktime);

	/* Changing any parameter must produce a different script.  */
	auto other_hash = Ripemd160::Hash(
		"0000000000000000000000000000000000000000"
	);
	auto s2 = Boltz::Detail::construct_redeemscript
		( other_hash, pubkey_hash, locktime, pubkey_locktime );
	assert(s2 != script);

	auto other_pk = Secp256k1::PubKey(
		"0307262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779"
	);
	auto s3 = Boltz::Detail::construct_redeemscript
		( hash160, other_pk, locktime, pubkey_locktime );
	assert(s3 != script);

	auto s4 = Boltz::Detail::construct_redeemscript
		( hash160, pubkey_hash, locktime + 1, pubkey_locktime );
	assert(s4 != script);

	auto s5 = Boltz::Detail::construct_redeemscript
		( hash160, pubkey_hash, locktime, other_pk );
	assert(s5 != script);

	assert(script.size() == expected.size());

	return 0;
}
