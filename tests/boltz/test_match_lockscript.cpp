#undef NDEBUG
#include"Boltz/Detail/match_lockscript.hpp"
#include"Ripemd160/Hash.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Util/Str.hpp"
#include<assert.h>

int main() {
	auto res = bool();
	auto hash = Ripemd160::Hash();
	auto claim_pubkey_hash = Secp256k1::XonlyPubKey();

	auto testclaim = [&](std::string const& s) {
		res = Boltz::Detail::match_claimscript
			( hash, claim_pubkey_hash
			, Util::Str::hexread(s)
			);
	};

	auto refund_pubkey_hash = Secp256k1::XonlyPubKey();
	auto locktime = std::uint32_t();

	auto testrefund = [&](std::string const& s) {
		res = Boltz::Detail::match_refundscript
			( locktime, refund_pubkey_hash
			, Util::Str::hexread(s)
			);
	};

	testclaim("82012088a9142c2d5441ef4a4469eae063941463e3c65ee926c0882007262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779ac");
	assert(res);
	assert(hash == Ripemd160::Hash("2c2d5441ef4a4469eae063941463e3c65ee926c0"));
	assert(claim_pubkey_hash == Secp256k1::XonlyPubKey("07262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779"));
	testrefund("200a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529ead0395e809b1");
	assert(res);
	assert(locktime == 649365);
	assert(refund_pubkey_hash == Secp256k1::XonlyPubKey("0a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529e"));

	testclaim("82012188a9142c2d5441ef4a4469eae063941463e3c65ee926c0882007262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779ac");
	assert(!res);
	testrefund("210a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529ead0395e809b1");
	assert(!res);

	// FIXME this test seems to test the tie-breaker byte at the start of the public key, meaningless with x-only keys...
	/*test("8201208763a9142c2d5441ef4a4469eae063941463e3c65ee926c08821ff07262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd77967750395e809b17521030a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529e68ac");
	assert(!res);*/

	testrefund("200a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529ead0295e8b1");
	assert(!res);

	// FIXME this test seems to test the tie-breaker byte at the start of the public key, meaningless with x-only keys...
	/*test("8201208763a9142c2d5441ef4a4469eae063941463e3c65ee926c088210207262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd77967750395e809b17521ff0a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529e68ac");
	assert(!res);*/

	testrefund("200a47fa92352ac70161366dad4b45ad2a2fcbf5cef40fec43b29d33128ace529ead0395e809b17551");
	assert(!res);
	testclaim("82012088a9142c2d5441ef4a4469eae063941463e3c65ee926c0882007262fc331c1c845c6f8f7cca7a04ec3bdb09ef82cddac3eda2953c10bddd779ac7551");
	assert(!res);

	return 0;
}
