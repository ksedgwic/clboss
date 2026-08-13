#ifndef BOSS_MOD_INVOICEPAYER_HPP
#define BOSS_MOD_INVOICEPAYER_HPP

#include<vector>
#include<string>

#include"Boss/Msg/PayInvoice.hpp"

namespace Boss { namespace Mod { class Rpc; }}
namespace Ev { template<typename a> class Io; }
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::InvoicePayer
 *
 * @brief Handles `Boss::Msg::PayInvoice`
 * messages by actually doing the `pay`
 * command.
 *
 * @desc we do the `pay` command with long
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
	Ev::Io<void> pay(std::string, std::string, std::uint64_t);

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
