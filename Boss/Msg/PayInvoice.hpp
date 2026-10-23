#ifndef BOSS_MSG_PAYINVOICE_HPP
#define BOSS_MSG_PAYINVOICE_HPP

#include"Ln/Amount.hpp"
#include"Sha256/Hash.hpp"
#include<string>

namespace Boss { namespace Msg {

/** struct Boss::Msg::PayInvoice
 *
 * @brief Emitted when we want to pay an
 * invoice for some reason.
 *
 * @desc The invoice is paid only if it decodes
 * to the given payment hash and amount.  The
 * swap flow asks the provider for an invoice
 * on the hash of a preimage CLBOSS generated;
 * an invoice on any other hash would be paid
 * without CLBOSS holding the claim on the swap
 * (#329).
 */
struct PayInvoice {
	std::string invoice;
	/* The payment hash the invoice must carry.  */
	Sha256::Hash expected_hash;
	/* The amount the invoice must be for.  */
	Ln::Amount expected_amount;
};

}}

#endif /* !defined(BOSS_MSG_PAYINVOICE_HPP) */
