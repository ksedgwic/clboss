#include"Boltz/Detail/swaptree.hpp"
#include"Secp256k1/TapscriptTree.hpp"

using namespace Secp256k1;

namespace Boltz { namespace Detail {

std::vector<std::uint8_t> compute_root_hash
					( std::vector<std::uint8_t> const& claimscript
					, std::vector<std::uint8_t> const& refundscript
					) {
	/* leaves to buffers */
	auto leaf0 = TapTree::encoded_leaf(claimscript);
	auto leaf1 = TapTree::encoded_leaf(refundscript);

	/* tagged hashes of leaves */
	auto leafhashes = TapTree::LeafPair(leaf0, leaf1);

	/* combine leaves into a branch */
	return leafhashes.compute_branch_hash();
}

}}
