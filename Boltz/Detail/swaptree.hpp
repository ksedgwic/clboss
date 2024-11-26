#ifndef BOLTZ_DETAIL_SWAP_TREE_HPP
#define BOLTZ_DETAIL_SWAP_TREE_HPP

#include<cstdint>
#include<string>
#include<vector>

namespace Secp256k1 { namespace Musig { class AggPubKey; } }
namespace Secp256k1 { namespace Musig { class TapTweak; } }

namespace Boltz { namespace Detail {

/** Boltz::Detail::match_lockscript
 * TODO TODO TODO TODO TODO TODO TODO TODO TODO TODO TODO
 * @brief .
 */

std::vector<std::uint8_t> compute_root_hash
				( std::vector<std::uint8_t> const&
				, std::vector<std::uint8_t> const& );

}}

#endif /* !defined(BOLTZ_DETAIL_SWAP_TREE_HPP) */
