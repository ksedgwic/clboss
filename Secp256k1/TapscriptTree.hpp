#ifndef SECP256K1_TAPTREE_HPP
#define SECP256K1_TAPTREE_HPP

#include<cstdint>
#include<stdexcept>
#include<vector>

namespace Secp256k1 { class XonlyPubKey; }

namespace Secp256k1 { namespace TapTree {

/* Thrown in case of being fed an invalid argument.  */
class InvalidArg : public std::invalid_argument {
public:
	InvalidArg(std::string arg)
		: std::invalid_argument("Invalid argument: " +arg) { }
};

std::vector<std::uint8_t>
encoded_leaf(std::vector<std::uint8_t> const&);

/* The BIP341 tapleaf hash of a tapscript: the tagged
 * hash of its encoded leaf.  This is what a script-path
 * signature commits to, and what the control block's
 * merkle path is built from.  */
std::vector<std::uint8_t>
leaf_hash(std::vector<std::uint8_t> const& script);

struct LeafPair {
	std::vector<std::uint8_t> a;
	std::vector<std::uint8_t> b;

	LeafPair() =delete;
	LeafPair( std::vector<std::uint8_t> const&
			, std::vector<std::uint8_t> const& );

	// FIXME RVO doesn't call move ctor?
	LeafPair& operator= (LeafPair&&) =default;
	LeafPair(LeafPair&&) =default;

	std::vector<std::uint8_t> compute_branch_hash();
};

}}

#endif /* SECP256K1_TAPTREE_HPP */
