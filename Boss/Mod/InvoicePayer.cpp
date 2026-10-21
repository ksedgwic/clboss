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
#include"Sha256/Hash.hpp"
#include<memory>
#include<string>

namespace {

/* Maximum routing fee we are willing to pay: 1% of the invoice
 * amount, floored at 5000 msat for small invoices -- the same
 * budget xpay would apply if maxfee were omitted, made explicit
 * here so the policy is visible at the call site and in review.
 * History: the legacy `pay` call used maxfeepercent=5.0 for
 * MPP-capable invoices (all Boltz swap-out invoices are), with
 * realized fees observed under 1%; an earlier xpay-migration pass
 * carried over the 0.5% non-MPP cap instead -- the branch that
 * never applied to these invoices -- which is 2x tighter than
 * xpay's own default and strands swap payments from modestly-
 * connected nodes.  A fee-capped failure on the sole PayInvoice
 * producer (SwapManager swap-outs) just stalls inbound-liquidity
 * acquisition, and SwapManager's amount-reduction retries cannot
 * help against a proportional cap.  Integer math to avoid float
 * rounding drift on larger invoices.
 */
auto constexpr maxfee_divisor = std::uint64_t(100);
auto constexpr maxfee_floor_msat = std::uint64_t(5000);

/* Thrown when the decoded invoice is not the one we were asked
 * to pay.  */
struct Mismatch {
	std::string reason;
};

}

namespace Boss { namespace Mod {

void InvoicePayer::start() {
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		/* Pay pending invoices.  */
		auto f = [this](Msg::PayInvoice p) {
			return pay(std::move(p));
		};
		return Boss::concurrent(
			Ev::foreach(f, std::move(pending_invoices))
		);
	});

	bus.subscribe<Msg::PayInvoice
		     >([this](Msg::PayInvoice const& p) {
		if (!rpc) {
			/* Not yet ready, add to pending.  */
			pending_invoices.push_back(p);
			return Ev::lift();
		}
		return Boss::concurrent(pay(p));
	});
}

Ev::Io<void> InvoicePayer::pay(Msg::PayInvoice n_p) {
	auto p = std::make_shared<Msg::PayInvoice>(std::move(n_p));
	return Ev::lift().then([this, p]() {
		return Boss::log( bus, Debug
				, "InvoicePayer: Initiating: %s"
				, p->invoice.c_str()
				);
	}).then([this, p]() {
		auto parms = Json::Out()
			.start_object()
				.field("string", p->invoice)
			.end_object()
			;
		return rpc->command("decode", std::move(parms));
	}).then([this, p](Jsmn::Object res) {
		if (!res.has("type")
		|| std::string(res["type"]) != "bolt11 invoice"
		|| !res.has("valid")
		|| !res["valid"].is_boolean()
		|| !bool(res["valid"])
		|| !res.has("amount_msat")
		|| !res.has("payment_hash")
		|| !res["payment_hash"].is_string()
		) {
			throw Jsmn::TypeError();
		}

		/* Pay only the invoice we were asked to pay: the
		 * one on the expected payment hash, for the
		 * expected amount.  See Msg::PayInvoice.  */
		auto hash_s = std::string(res["payment_hash"]);
		if (!Sha256::Hash::valid_string(hash_s))
			throw Jsmn::TypeError();
		if (Sha256::Hash(hash_s) != p->expected_hash)
			throw Mismatch{ std::string("payment_hash ")
				      + hash_s
				      + ", expected "
				      + std::string(p->expected_hash)
				      };
		auto amount = Ln::Amount::object(res["amount_msat"]);
		if (amount != p->expected_amount)
			throw Mismatch{ std::string("amount ")
				      + std::string(amount)
				      + ", expected "
				      + std::string(p->expected_amount)
				      };

		/* Compute an absolute maxfee from the invoice amount
		 * (policy documented on maxfee_divisor above).  xpay's
		 * MPP handling makes the legacy feature-bit-driven MPP
		 * branch unnecessary: askrene + xpay manage the fee
		 * budget across parts internally.
		 */
		auto maxfee_msat = amount.to_msat() / maxfee_divisor;
		if (maxfee_msat < maxfee_floor_msat)
			maxfee_msat = maxfee_floor_msat;

		/* TODO: Get created_at and expiry, add them, then determine
		 * current time and subtract, to get retry_for.
		 */
		auto retry_for = 1000;

		auto parms = Json::Out()
			.start_object()
				.field("invstring", p->invoice)
				.field("retry_for", retry_for)
				.field("maxfee", maxfee_msat)
			.end_object()
			;
		return rpc->command("xpay", std::move(parms));
	}).then([this, p](Jsmn::Object _) {
		return Boss::log( bus, Debug
				, "InvoicePayer: Paid: %s"
				, p->invoice.c_str()
				);
	}).catching<RpcError>([this, p](RpcError const& _) {
		return Boss::log( bus, Debug
				, "InvoicePayer: Failed to pay: %s"
				, p->invoice.c_str()
				);
	}).catching<Jsmn::TypeError>([this, p](Jsmn::TypeError const& _) {
		return Boss::log( bus, Error
				, "InvoicePayer: "
				  "Unexpected decode result for invoice: %s"
				, p->invoice.c_str()
				);
	}).catching<Mismatch>([this, p](Mismatch const& m) {
		return Boss::log( bus, Error
				, "InvoicePayer: Not paying %s: %s"
				, p->invoice.c_str()
				, m.reason.c_str()
				);
	});
}

}}
