#include"Boltz/Detail/reverse_terms.hpp"
#include"Jsmn/Object.hpp"

namespace Boltz { namespace Detail {

ReverseTerms parse_reverse_terms(Jsmn::Object const& pairs) {
	/* The pairs are keyed by the asset sent on Lightning, then
	 * by the asset received onchain.  A pair the provider does
	 * not offer is simply absent, which the TypeError reports.  */
	auto pair = pairs["BTC"]["BTC"];
	auto terms = ReverseTerms();
	terms.rate = (double) pair["rate"];
	terms.minimal = Ln::Amount::sat((double) pair["limits"]["minimal"]);
	terms.maximal = Ln::Amount::sat((double) pair["limits"]["maximal"]);
	terms.fee_percent = (double) pair["fees"]["percentage"];
	terms.fee_lockup = Ln::Amount::sat((double)
		pair["fees"]["minerFees"]["lockup"]
	);
	return terms;
}

Ln::Amount quoted_onchain_amount( ReverseTerms const& terms
				, Ln::Amount offchainAmount
				) {
	auto onchainAmount = (offchainAmount * terms.rate * 100)
			   / (100 + terms.fee_percent)
			   ;
	if (onchainAmount > offchainAmount)
		onchainAmount = offchainAmount;
	if (onchainAmount > terms.fee_lockup)
		onchainAmount -= terms.fee_lockup;
	else
		onchainAmount = Ln::Amount::sat(0);
	return onchainAmount;
}

}}
