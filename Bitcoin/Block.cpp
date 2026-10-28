#include"Bitcoin/Block.hpp"
#include"Bitcoin/TxId.hpp"
#include"Bitcoin/varint.hpp"
#include"Util/BacktraceException.hpp"
#include"Util/Str.hpp"
#include<algorithm>
#include<sstream>
#include<stdexcept>

namespace {

/* No block holds this many transactions; a count above it
 * is a parse gone wrong, not a block.  */
auto const max_txs = std::uint64_t(1000000);

}

namespace Bitcoin {

Block::Block(std::string const& s) : header(80, 0) {
	auto buf = Util::Str::hexread(s);
	auto str = std::string(buf.begin(), buf.end());
	auto is = std::istringstream(std::move(str));
	is >> *this;
	if (!is.good())
		throw Util::BacktraceException<std::invalid_argument>("Bitcoin::Block: invalid hex string input.");
	if (is.get() != std::char_traits<char>::eof())
		throw Util::BacktraceException<std::invalid_argument>("Bitcoin::Block: input string too long.");
}

bool Block::contains(Bitcoin::TxId const& txid) const {
	return std::any_of( txs.begin(), txs.end()
			  , [&txid](Tx const& tx) {
		return tx.get_txid() == txid;
	});
}

Tx const* Block::spender( Bitcoin::TxId const& txid
			, std::uint32_t outnum
			) const {
	for (auto const& tx : txs)
		for (auto const& in : tx.inputs)
			if (in.prevTxid == txid && in.prevOut == outnum)
				return &tx;
	return nullptr;
}

}

std::ostream& operator<<(std::ostream& os, Bitcoin::Block const& v) {
	os.write((char const*) v.header.data(), v.header.size());
	auto len = std::uint64_t(v.txs.size());
	os << Bitcoin::varint(len);
	for (auto const& tx : v.txs)
		os << tx;
	return os;
}

std::istream& operator>>(std::istream& is, Bitcoin::Block& v) {
	v.header.resize(80);
	is.read((char*) v.header.data(), 80);
	if (!is.good())
		return is;

	auto len = std::uint64_t();
	is >> Bitcoin::varint(len);
	if (len > max_txs) {
		is.setstate(std::ios_base::failbit);
		return is;
	}
	v.txs.resize(std::size_t(len));
	for (auto& tx : v.txs)
		is >> tx;

	return is;
}
