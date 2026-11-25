#ifndef BOSS_MSG_SWAPCREATION_HPP
#define BOSS_MSG_SWAPCREATION_HPP

#include"Sha256/Hash.hpp"
#include<cstdint>
#include<string>

namespace Boss { namespace Msg {

/** struct Boss::Msg::SwapCreation
 *
 * @brief broadcasted when a swap provider has
 * succeeded or failed to set up the swap.
 * Swap provider modules broadcast this in response
 * to `Boss::Msg::AcceptSwapQuotation` messages,
 * and may respond with this message asynchronously.
 */
struct SwapCreation {
	/* False if failed.  */
	bool success;

	/* The invoice to pay, ignored if `!success`.  */
	std::string invoice;
	/* The hash of the invoice.  */
	Sha256::Hash hash;
	/* An absolute timeout in blocks, at which the
	 * swap is known to be definitely failing.  */
	std::uint32_t timeout_blockheight;

	/* Identify the solicitor that asked for the swap.  */
	void* solicitor;
	/* Identify the swap provider that was asked.  */
	void* provider;
	/* The round of the `Boss::Msg::AcceptSwapQuotation`,
	 * as given.  */
	std::uint64_t round;
};

}}

#endif /* !defined(BOSS_MSG_SWAPCREATION_HPP) */
