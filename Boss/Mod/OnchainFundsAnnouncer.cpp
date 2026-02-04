#include"Boss/Mod/OnchainFundsAnnouncer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Block.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/OnchainFunds.hpp"
#include"Boss/Msg/RequestGetOnchainIgnoreFlag.hpp"
#include"Boss/Msg/ResponseGetOnchainIgnoreFlag.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/coroutine.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include<cstddef>
#include<cstdint>
#include<sstream>

namespace {

/** minconf
 *
 * @brief Number of confirmations before we consider
 * the funds to be safe to be spent.
 *
 * @desc We have to be wary of double-spending attacks
 * mounted on us by attackers who take advantage of
 * blocks getting orphaned.
 * 3 seems safe without taking too long.
 */
auto const minconf = std::size_t(3);

auto const startweight = std::size_t
( 42/* common*/
+ (8/*amount*/ + 1/*scriptlen*/ + 1/*push 0*/ + 1/*push*/ + 32/*p2wsh*/) * 4
);

}

namespace Boss { namespace Mod {

void OnchainFundsAnnouncer::start() {
	bus.subscribe< Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		return Ev::lift();
	});
	bus.subscribe< Msg::Block
		     >([this](Msg::Block const& _) {
		if (!rpc)
			return Ev::lift();
		return Boss::concurrent(on_block());
	});
}

Ev::Io<void> OnchainFundsAnnouncer::on_block() {
	auto res = co_await get_ignore_rr.execute(
		Msg::RequestGetOnchainIgnoreFlag{nullptr}
	);
	if (res.ignore) {
		co_await Boss::log( bus, Info
				  , "OnchainFundsAnnouncer: "
				    "Ignoring onchain funds until "
				    "%f seconds from now."
				  , res.seconds
				  );
		co_return;
	}
	co_await announce();
}

Ev::Io<void> OnchainFundsAnnouncer::announce() {
	auto no_onchain_funds = false;
	try {
		auto res = co_await fundpsbt();
		if (!res.is_object()) {
			co_await fail("fundpsbt did not return object", res);
			co_return;
		}
		if (!res.has("excess_msat")) {
			co_await fail("fundpsbt has no excess_msat", res);
			co_return;
		}
		auto excess_msat = res["excess_msat"];
		if (!Ln::Amount::valid_object(excess_msat)) {
			co_await fail( "fundpsbt excess_msat not a valid amount"
				     , excess_msat
				     );
			co_return;
		}
		auto amount = Ln::Amount::object(excess_msat);

		co_await Boss::log( bus, Debug
				  , "OnchainFundsAnnouncer: "
				    "Found %s (after deducting fee to spend) "
				    "onchain."
				  , std::string(amount).c_str()
				  );
		// Don't use aggregate temporaries in a `co_await`, see docs/COROUTINE.md
		Msg::OnchainFunds msg{amount};
		co_await bus.raise(std::move(msg));
	} catch (RpcError const&) {
		no_onchain_funds = true;
	}
	if (no_onchain_funds) {
		co_await Boss::log( bus, Debug
				  , "OnchainFundsAnnouncer: "
				    "No onchain funds found."
				  );
	}
}

Ev::Io<Jsmn::Object>
OnchainFundsAnnouncer::fundpsbt() {
	auto params = Json::Out()
		.start_object()
			/* Get all the funds.  */
			.field("satoshi", std::string("all"))
			.field("feerate", std::string("normal"))
			.field("startweight", (double) startweight)
			.field("minconf", (double) minconf)
			/* Do not reserve; we just want to know
			 * how much money could be spent.
			 */
			.field("reserve", std::uint32_t(0))
		.end_object()
		;
	co_return co_await rpc->command("fundpsbt", std::move(params));
}

Ev::Io<void>
OnchainFundsAnnouncer::fail( std::string const& msg
			   , Jsmn::Object res
			   ) {
	auto os = std::ostringstream();
	os << res;
	co_await Boss::log( bus, Error
			  , "OnchainFundsAnnouncer: %s: %s"
			  , msg.c_str()
			  , os.str().c_str()
			  );
	co_return;
}

OnchainFundsAnnouncer::~OnchainFundsAnnouncer() =default;
OnchainFundsAnnouncer::OnchainFundsAnnouncer(S::Bus& bus_)
	: bus(bus_), rpc(nullptr)
	, get_ignore_rr(bus_)
	{ start(); }

}}
