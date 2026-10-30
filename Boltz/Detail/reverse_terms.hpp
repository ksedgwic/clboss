#ifndef BOLTZ_DETAIL_REVERSE_TERMS_HPP
#define BOLTZ_DETAIL_REVERSE_TERMS_HPP

#include"Ln/Amount.hpp"

namespace Jsmn { class Object; }

namespace Boltz { namespace Detail {

/** struct Boltz::Detail::ReverseTerms
 *
 * @brief the terms of the BTC/BTC reverse swap pair as
 * /v2/swap/reverse reports them.
 */
struct ReverseTerms {
	double rate;
	/* Service fee, in percent of the amount sent.  */
	double fee_percent;
	/* The provider's lockup transaction fee, taken from the
	 * amount locked up.  */
	Ln::Amount fee_lockup;
	Ln::Amount minimal;
	Ln::Amount maximal;
};

/** Boltz::Detail::parse_reverse_terms
 *
 * @brief the terms from the /v2/swap/reverse result; throws
 * Jsmn::TypeError when the pair is absent or malformed.
 */
ReverseTerms parse_reverse_terms(Jsmn::Object const& pairs);

/** Boltz::Detail::quoted_onchain_amount
 *
 * @brief the on-chain amount the terms promise for the given
 * amount sent on Lightning.
 */
Ln::Amount quoted_onchain_amount( ReverseTerms const& terms
				, Ln::Amount offchainAmount
				);

}}

#endif /* !defined(BOLTZ_DETAIL_REVERSE_TERMS_HPP) */
