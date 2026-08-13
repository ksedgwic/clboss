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
#include"S/Bus.hpp"
#include"Util/Str.hpp"
#include<memory>
#include<ctime>

namespace {

/* The `maxfeepercent` setting to pass to `pay`.  */
auto const nonmpp_maxfeepercent = 0.5;
auto const mpp_maxfeepercent = 5.0;
/* Wait, 5%??
 * Actually as of 0.9.0, 0.9.0-1, and 0.9.1, the MPP split
 * payment tends to badly mismanage the fee budget, and in
 * practice specifying 5% tends to lead to fees of less than
 * 1%.
 * But if you specify max fee of 1%, for badly-connected
 * nodes you have low chance of success since some of the
 * straggling payment parts are unable to reach the target
 * with the very minimal amount of budget they happened to
 * have gotten.
 * Until `lightningd` can fix the MPP budget handling, we
 * need to use this budget.
 */

}

namespace Boss { namespace Mod {

void InvoicePayer::start() {
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		auto f = [this](Msg::PayInvoice p) {
			return pay(std::move(p.invoice), std::move(p.expected_payment_hash), p.expected_amount_msat);
		};
		return Boss::concurrent(
			Ev::foreach(f, std::move(pending_invoices))
		);
	});

	bus.subscribe<Msg::PayInvoice
		     >([this](Msg::PayInvoice const& p) {
		if (!rpc) {
			pending_invoices.push_back(p);
			return Ev::lift();
		}
		return Boss::concurrent(pay(p.invoice, p.expected_payment_hash, p.expected_amount_msat));
	});
}

Ev::Io<void> InvoicePayer::pay(std::string n_invoice, std::string expected_hash, std::uint64_t expected_amount) {
	auto inv = std::make_shared<std::string>(std::move(n_invoice));
	auto exp_hash = std::make_shared<std::string>(std::move(expected_hash));
	auto exp_amt = expected_amount;
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
	}).then([this, inv, exp_hash, exp_amt](Jsmn::Object res) {
		if (!res.has("type")
		|| std::string(res["type"]) != "bolt11 invoice"
		|| !res.has("valid")
		|| !res["valid"].is_boolean()
		|| !bool(res["valid"])
		) {
			throw Jsmn::TypeError();
		}

		if (!exp_hash->empty() && res.has("payment_hash")) {
			auto actual_hash = std::string(res["payment_hash"]);
			if (actual_hash != *exp_hash) {
				throw Jsmn::TypeError();
			}
		}

		if (exp_amt != 0 && res.has("amount_msat")) {
			auto actual_amt = (std::uint64_t)(double)res["amount_msat"];
			if (actual_amt != exp_amt) {
				throw Jsmn::TypeError();
			}
		}

		if (!exp_hash->empty() && res.has("timestamp") && res.has("expiry")) {
			auto created = (std::time_t)(double)res["timestamp"];
			auto expiry = (std::time_t)(double)res["expiry"];
			auto now = std::time(nullptr);
			if (created + expiry < now) {
				throw Jsmn::TypeError();
			}
		}

		/* Check the features and see if this is MPP-enabled.  */
		auto is_mpp = false;
		if (res.has("features")) {
			auto features_s = std::string(res["features"]);
			auto features = Util::Str::hexread(features_s);
			/* Basic MPP is bits 16 or 17.  */
			if (features.size() >= 3) {
				if (features[features.size() - 3] & 0x03)
					is_mpp = true;
			}
		}

		auto maxfeepercent =
			(is_mpp) ?	mpp_maxfeepercent :
			/*otherwise*/	nonmpp_maxfeepercent ;
		/* TODO: Get created_at and expiry, add them, then determine
		 * current time and subtract, to get retry_for.
		 */
		auto retry_for = 1000;

		auto parms = Json::Out()
			.start_object()
				.field("bolt11", *inv)
				.field("retry_for", retry_for)
				.field("maxfeepercent", maxfeepercent)
			.end_object()
			;
		return rpc->command("pay", std::move(parms));
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
	}).catching<Util::Str::HexParseFailure>([this, inv](Util::Str::HexParseFailure const& _) {
		return Boss::log( bus, Error
				, "InvoicePayer: "
				  "Unexpected 'features' for invoice: %s"
				, inv->c_str()
				);
	});
}

}}
