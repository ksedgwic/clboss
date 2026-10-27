#undef NDEBUG
#include"Bitcoin/Block.hpp"
#include"Bitcoin/Tx.hpp"
#include"Bitcoin/TxId.hpp"
#include"Util/Str.hpp"
#include<assert.h>
#include<cstdint>
#include<sstream>
#include<string>

namespace {

/* Signet block 1, as lightningd's getrawblockbyheight returns
 * it: the header and one coinbase transaction.  */
char const block1_hex[] =
	"00000020f61eee3b63a380a477a063af32b2bbc97c9ff9f01f2c4225e9739881"
	"08000000f575c83235984e7dc4afc1f30944c170462e84437ab6f2d52e16878a"
	"79e4678bd1914d5fae77031eccf40700010100000000010100000000000000000"
	"00000000000000000000000000000000000000000000000ffffffff025151feff"
	"ffff0200f2052a010000001600149243f727dd5343293eb83174324019ec16c26"
	"30f0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa3695375"
	"5c690689799962b48bebd836974e8cf94c4fecc7daa2490047304402205e423a8"
	"754336ca99dbe16509b877ef1bf98d008836c725005b3c787c41ebe4602204724"
	"6e4467ad7cc7f1ad98662afcaf14c115e0095a227c7b05c5182591c23e7e01000"
	"1200000000000000000000000000000000000000000000000000000000000000000"
	"00000000"
	;

}

int main() {
	auto block = Bitcoin::Block(std::string(block1_hex));
	assert(block.header.size() == 80);
	assert(block.txs.size() == 1);

	/* With one transaction, the merkle root in the header
	 * (bytes 36 to 67) is that transaction's id, in the same
	 * internal byte order the id serializes in.  */
	auto txid = block.txs[0].get_txid();
	auto os = std::ostringstream();
	os << txid;
	auto raw = os.str();
	assert(raw.size() == 32);
	for (auto i = std::size_t(0); i < 32; ++i)
		assert(std::uint8_t(raw[i]) == block.header[36 + i]);

	assert(block.contains(txid));
	assert(!block.contains(Bitcoin::TxId(std::string(
		"0000000000000000000000000000000000000000000000000000000000000001"
	))));

	/* Serializing gives the bytes back.  */
	auto bos = std::ostringstream();
	bos << block;
	auto bytes = bos.str();
	assert(Util::Str::hexdump(bytes.data(), bytes.size()) == block1_hex);

	/* A truncated block does not parse.  */
	auto threw = false;
	try {
		auto bad = Bitcoin::Block(std::string(block1_hex, 200));
		(void) bad;
	} catch (std::exception const&) {
		threw = true;
	}
	assert(threw);

	return 0;
}
