#undef NDEBUG
#include"Bitcoin/Tx.hpp"
#include"Bitcoin/TxOut.hpp"
#include"Bitcoin/sighash.hpp"
#include"Ln/Amount.hpp"
#include"Sha256/HasherStream.hpp"
#include"Util/Str.hpp"
#include<assert.h>

std::string rawtx = "02000000097de20cbff686da83a54981d2b9bab3586f4ca7e48f57f5b55963115f3b334e9c010000000000000000d7b7cab57b1393ace2d064f4d4a2cb8af6def61273e127517d44759b6dafdd990000000000fffffffff8e1f583384333689228c5d28eac13366be082dc57441760d957275419a418420000000000fffffffff0689180aa63b30cb162a73c6d2a38b7eeda2a83ece74310fda0843ad604853b0100000000feffffffaa5202bdf6d8ccd2ee0f0202afbbb7461d9264a25e5bfd3c5a52ee1239e0ba6c0000000000feffffff956149bdc66faa968eb2be2d2faa29718acbfe3941215893a2a3446d32acd050000000000000000000e664b9773b88c09c32cb70a2a3e4da0ced63b7ba3b22f848531bbb1d5d5f4c94010000000000000000e9aa6b8e6c9de67619e6a3924ae25696bb7b694bb677a632a74ef7eadfd4eabf0000000000ffffffffa778eb6a263dc090464cd125c466b5a99667720b1c110468831d058aa1b82af10100000000ffffffff0200ca9a3b000000001976a91406afd46bcdfd22ef94ac122aa11f241244a37ecc88ac807840cb0000000020ac9a87f5594be208f8532db38cff670c450ed2fea8fcdefcc9a663f78bab962b0065cd1d";

std::vector<std::string> spk =
{"512053a1f6e454df1aa2776a2814a721372d6258050de330b3c6d10ee8f4e0dda343",
 "5120147c9c57132f6e7ecddba9800bb0c4449251c92a1e60371ee77557b6620f3ea3",
 "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac",
 "5120e4d810fd50586274face62b8a807eb9719cef49c04177cc6b76a9a4251d5450e",
 "512091b64d5324723a985170e4dc5a0f84c041804f2cd12660fa5dec09fc21783605",
 "00147dd65592d0ab2fe0d0257d571abf032cd9db93dc",
 "512075169f4001aa68f15bbed28b218df1d0a62cbbcf1188c6665110c293c907b831",
 "5120712447206d7a5238acc7ff53fbe94a3b64539ad291c7cdbc490b7577e4b17df5",
 "512077e30a5522dd9f894c3f8b8bd4c4b2cf82ca7da8a3ea6a239655c39c050ab220"};

std::vector<uint64_t> ams = {
 420000000,
 462000000,
 294000000,
 504000000,
 630000000,
 378000000,
 672000000,
 546000000,
 588000000
};

Bitcoin::SighashFlags flags[9] = {
	Bitcoin::SIGHASH_SINGLE,
	(Bitcoin::SighashFlags) int{131}, /* SIGHASH_SINGLE|SIGHASH_ANYONECANPAY */
	Bitcoin::SIGHASH_ALL,
	Bitcoin::SIGHASH_ALL,
	Bitcoin::SIGHASH_DEFAULT,
	Bitcoin::SIGHASH_ALL,
	Bitcoin::SIGHASH_NONE,
	(Bitcoin::SighashFlags) int{130}, /* SIGHASH_NONE|SIGHASH_ANYONECANPAY */
	(Bitcoin::SighashFlags) int{129} /* SIGHASH_ALL|SIGHASH_ANYONECANPAY */
};

Sha256::Hash expected[9] = {
Sha256::Hash("2514a6272f85cfa0f45eb907fcb0d121b808ed37c6ea160a5a9046ed5526d555"),
Sha256::Hash("325a644af47e8a5a2591cda0ab0723978537318f10e6a63d4eed783b96a71a4d"),
Sha256::Hash("0000000000000000000000000000000000000000000000000000000000000000"), /* <-- HACK */
Sha256::Hash("bf013ea93474aa67815b1b6cc441d23b64fa310911d991e713cd34c7f5d46669"),
Sha256::Hash("4f900a0bae3f1446fd48490c2958b5a023228f01661cda3496a11da502a7f7ef"),
Sha256::Hash("0000000000000000000000000000000000000000000000000000000000000000"), /* <-- HACK */
Sha256::Hash("15f25c298eb5cdc7eb1d638dd2d45c97c4c59dcaec6679cfc16ad84f30876b85"),
Sha256::Hash("cd292de50313804dabe4685e83f923d2969577191a3e1d2882220dca88cbeb10"),
Sha256::Hash("cccb739eca6c13a8a89e6e5cd317ffe55669bbda23f2fd37b0f18755e008edd2")
};

int main() {
	auto tx = Bitcoin::Tx(rawtx);
	auto spkvec = std::vector<std::vector<std::uint8_t>>(spk.size());

	for (size_t i{0}; i < spk.size(); ++i) {
		auto buf = Util::Str::hexread(spk[i]);
		spkvec[i] = std::vector<std::uint8_t>(buf.size() +1);
		spkvec[i][0] = buf.size();
		for (size_t j{1}, k{0}; k < buf.size(); ++j, ++k)
			spkvec[i][j] = buf[k];
	}

	auto amsvec = std::vector<Ln::Amount>(ams.size());
	for (size_t i{0}; i < ams.size(); ++i)
		amsvec[i] = Ln::Amount::sat(ams[i]);

	auto sighashes = std::vector<Sha256::Hash>(spkvec.size());
	for (size_t i{0}; i < spkvec.size(); ++i) {
		sighashes[i] = Bitcoin::p2tr_sighash( tx
											, flags[i]
											, i
											, amsvec
											, spkvec
											, Bitcoin::KEYPATH
											);
		if (i == 2 || i == 5) /* <-- HACK */
			continue;

		assert(expected[i] == sighashes[i]);
	}

	return 0;
}
