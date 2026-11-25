#ifndef BOSS_MSG_PROVIDESWAPQUOTATION_HPP
#define BOSS_MSG_PROVIDESWAPQUOTATION_HPP

#include"Ln/Amount.hpp"
#include<cstdint>
#include<string>

namespace Boss { namespace Msg {

/** struct Boss::Msg::ProvideSwapQuotation
 *
 * @brief broadcasted in response to handling a
 * `Boss::Msg::SolicitSwapQuotation` message.
 * If your module is selected, then a later
 * `Boss::Msg::AcceptSwapQuotation` message will
 * be broadcast identifying your module via the
 * given provider pointer.
 */
struct ProvideSwapQuotation {
	/* Estimate of how much will be deducted
	 * from the offchain amount to provide
	 * the onchain amount.
	 */
	Ln::Amount fee;

	/* Identify the solicitor we are responding
	 * to.
	 */
	void* solicitor;
	/* Identify the module that is responding.  */
	void* provider;
	/* String identifier of the swap service.  */
	std::string provider_name;
	/* The round of the `Boss::Msg::SolicitSwapQuotation`,
	 * as given.  */
	std::uint64_t round;
};

}}

#endif /* !defined(BOSS_MSG_PROVIDESWAPQUOTATION_HPP) */
