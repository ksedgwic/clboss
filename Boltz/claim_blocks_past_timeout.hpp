#ifndef BOLTZ_CLAIM_BLOCKS_PAST_TIMEOUT_HPP
#define BOLTZ_CLAIM_BLOCKS_PAST_TIMEOUT_HPP

#include<cstdint>

namespace Boltz {

/* Blocks past a reverse swap's timeout during which a claim with
 * the preimage out is still pushed.  Our claim stays valid past
 * the timeout; the provider only gains the option to refund.  The
 * claim handler gives up at this horizon, and the swap bookkeeping
 * waits for it before writing a paid swap off.  */
auto constexpr claim_blocks_past_timeout = std::uint32_t(432);

}

#endif /* !defined(BOLTZ_CLAIM_BLOCKS_PAST_TIMEOUT_HPP) */
