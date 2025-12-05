#include"Bitcoin/Tx.hpp"
#include"Boltz/Detail/find_lockup_outnum.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/fun.hpp"
#include<algorithm>

namespace Boltz { namespace Detail {

int find_lockup_outnum( Bitcoin::Tx const& tx
		      , std::vector<std::uint8_t> const& scriptPubKey
		      ) {
	auto it = std::find_if( tx.outputs.begin(), tx.outputs.end()
			      , [&scriptPubKey](Bitcoin::TxOut const& out) {
		return out.scriptPubKey == scriptPubKey;
	});
	if (it == tx.outputs.end())
		return -1;
	return it - tx.outputs.begin();
}

}}
