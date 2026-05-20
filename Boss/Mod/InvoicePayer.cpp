#include"Boss/Mod/InvoicePayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/PayInvoice.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"S/Bus.hpp"
#include<memory>

namespace {

/* Maximum routing fee we are willing to pay, expressed as a
 * divisor of the invoice amount.  200 = 0.5% cap, preserving the
 * historical pre-xpay `pay` non-MPP behaviour (xpay's own default
 * would be 1%).  Integer math: maxfee_msat = amount_msat / 200,
 * avoiding the rounding drift a 0.005 floating-point multiplication
 * would introduce on larger invoices.  The only producer of
 * Msg::PayInvoice is SwapManager paying a Boltz swap-out invoice,
 * so tight fee discipline matters here.
 */
auto constexpr maxfee_divisor = std::uint64_t(200);

}

namespace Boss { namespace Mod {

void InvoicePayer::start() {
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		/* Pay pending invoices.  */
		auto f = [this](std::string invoice) {
			return pay(std::move(invoice));
		};
		return Boss::concurrent(
			Ev::foreach(f, std::move(pending_invoices))
		);
	});

	bus.subscribe<Msg::PayInvoice
		     >([this](Msg::PayInvoice const& p) {
		if (!rpc) {
			/* Not yet ready, add to pending.  */
			pending_invoices.push_back(p.invoice);
			return Ev::lift();
		}
		return Boss::concurrent(pay(p.invoice));
	});
}

Ev::Io<void> InvoicePayer::pay(std::string n_invoice) {
	auto inv = std::make_shared<std::string>(std::move(n_invoice));
	return Ev::lift().then([this, inv]() {
		return Boss::log( bus, Debug
				, "InvoicePayer: Initiating: %s"
				, inv->c_str()
				);
	}).then([this, inv]() {
		auto parms = Json::Out()
			.start_object()
				.field("string", *inv)
			.end_object()
			;
		return rpc->command("decode", std::move(parms));
	}).then([this, inv](Jsmn::Object res) {
		if (!res.has("type")
		|| std::string(res["type"]) != "bolt11 invoice"
		|| !res.has("valid")
		|| !res["valid"].is_boolean()
		|| !bool(res["valid"])
		|| !res.has("amount_msat")
		) {
			throw Jsmn::TypeError();
		}

		/* Compute an absolute maxfee from the invoice amount,
		 * preserving the historical 0.5% cap via integer
		 * division.  xpay's MPP handling makes the legacy
		 * feature-bit-driven MPP branch unnecessary: askrene
		 * + xpay manage the fee budget across parts internally.
		 */
		auto amount = Ln::Amount::object(res["amount_msat"]);
		auto maxfee_msat = amount.to_msat() / maxfee_divisor;

		/* TODO: Get created_at and expiry, add them, then determine
		 * current time and subtract, to get retry_for.
		 */
		auto retry_for = 1000;

		auto parms = Json::Out()
			.start_object()
				.field("invstring", *inv)
				.field("retry_for", retry_for)
				.field("maxfee", maxfee_msat)
			.end_object()
			;
		return rpc->command("xpay", std::move(parms));
	}).then([this, inv](Jsmn::Object _) {
		return Boss::log( bus, Debug
				, "InvoicePayer: Paid: %s"
				, inv->c_str()
				);
	}).catching<RpcError>([this, inv](RpcError const& _) {
		return Boss::log( bus, Debug
				, "InvoicePayer: Failed to pay: %s"
				, inv->c_str()
				);
	}).catching<Jsmn::TypeError>([this, inv](Jsmn::TypeError const& _) {
		return Boss::log( bus, Error
				, "InvoicePayer: "
				  "Unexpected decode result for invoice: %s"
				, inv->c_str()
				);
	});
}

}}
