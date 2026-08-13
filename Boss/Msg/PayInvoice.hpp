#ifndef BOSS_MSG_PAYINVOICE_HPP
#define BOSS_MSG_PAYINVOICE_HPP

#include<string>
#include<cstdint>

namespace Boss { namespace Msg {

/** struct Boss::Msg::PayInvoice
 *
 * @brief Emitted when we want to pay an
 * invoice for some reason.
 */
struct PayInvoice {
	std::string invoice;
	std::string expected_payment_hash;
	std::uint64_t expected_amount_msat;
};

}}

#endif /* !defined(BOSS_MSG_PAYINVOICE_HPP) */
