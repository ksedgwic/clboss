#ifndef BOSS_MSG_ACCEPTSWAPQUOTATION_HPP
#define BOSS_MSG_ACCEPTSWAPQUOTATION_HPP

#include"Ln/Amount.hpp"
#include<cstdint>
#include<string>

namespace Boss { namespace Msg {

/** struct Boss::Msg::AcceptSwapQuotation
 *
 * @brief broadcasted when a particular swap provider
 * has been selected for this swap.
 * The swap provider must acknowledge this with a
 * `Boss::Msg::SwapCreation` identifying the solicitor
 * and provider.
 */
struct AcceptSwapQuotation {
	/* How much to pay offchain to get funds onchain.  */
	Ln::Amount offchain_amount;
	/* The onchain address that should receive the funds.  */
	std::string onchain_address;

	/* Identify the solicitor that is asking for the swap.  */
	void* solicitor;
	/* Identify the module that we want to perform the swap.  */
	void* provider;
	/* The solicitor's round, to be replicated in the
	 * `Boss::Msg::SwapCreation`.  */
	std::uint64_t round;
};

}}

#endif /* !defined(BOSS_MSG_ACCEPTSWAPQUOTATION_HPP) */
