#ifndef BOLTZ_DETAIL_MATCH_LOCKSCRIPT_HPP
#define BOLTZ_DETAIL_MATCH_LOCKSCRIPT_HPP

#include<cstdint>
#include<vector>

namespace Ripemd160 { class Hash; }
namespace Secp256k1 { class XonlyPubKey; }

namespace Boltz { namespace Detail {

/** Boltz::Detail::match_claimscript
 * @brief determines if the given supposed SCRIPT
 * matches the expected tapscript leaf from a proper
 * BOLTZ instance.
 */

bool match_claimscript( Ripemd160::Hash& hash
			 , Secp256k1::XonlyPubKey&
		     , std::vector<std::uint8_t> const& script
		     );

/** Boltz::Detail::match_refundscript
 * @brief determines if the given supposed SCRIPT
 * matches the expected tapscript leaf from a proper
 * BOLTZ instance.
 */

bool match_refundscript( std::uint32_t& locktime
			 , Secp256k1::XonlyPubKey&
		     , std::vector<std::uint8_t> const& script
		     );

}}

#endif /* !defined(BOLTZ_DETAIL_MATCH_LOCKSCRIPT_HPP) */
