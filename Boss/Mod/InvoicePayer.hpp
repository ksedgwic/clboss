#ifndef BOSS_MOD_INVOICEPAYER_HPP
#define BOSS_MOD_INVOICEPAYER_HPP

#include"Boss/Msg/PayInvoice.hpp"
#include<vector>

namespace Boss { namespace Mod { class Rpc; }}
namespace Ev { template<typename a> class Io; }
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::InvoicePayer
 *
 * @brief Handles `Boss::Msg::PayInvoice`
 * messages by checking the invoice against
 * the expected payment hash and amount, then
 * actually doing the `xpay` command.
 *
 * @desc we do the `xpay` command with long
 * `retry_for` timeouts; this is an automated
 * system and we do not particularly care
 * about how long timeouts are.
 */
class InvoicePayer {
private:
	S::Bus& bus;
	Boss::Mod::Rpc* rpc;

	std::vector<Msg::PayInvoice> pending_invoices;

	void start();
	Ev::Io<void> pay(Msg::PayInvoice);

public:
	InvoicePayer() =delete;
	InvoicePayer(InvoicePayer&&) =delete;
	InvoicePayer(InvoicePayer const&) =delete;

	explicit
	InvoicePayer( S::Bus& bus_
		    ) : bus(bus_), rpc(nullptr)
		      { start(); }
};

}}

#endif /* !defined(BOSS_MOD_INVOICEPAYER_HPP) */
