#include"Bitcoin/Tx.hpp"
#include"Boss/Mod/BoltzSwapper/Env.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Util/stringify.hpp"

namespace Boss { namespace Mod { namespace BoltzSwapper {

void Env::start() {
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		return Ev::lift();
	});
}

Ev::Io<std::uint32_t> Env::get_feerate() {
	return Ev::yield().then([this]() {
		/* Should not happen.  */
		if (!rpc)
			return Ev::lift<std::uint32_t>(2000);

		return rpc->command("feerates"
				   , Json::Out()
					.start_object()
						.field("style", "perkw")
					.end_object()
		).then([this](Jsmn::Object res) {
			try {
				auto rate = res["perkw"]["opening"];
				return Ev::lift<std::uint32_t>(double(rate));
			} catch (Jsmn::TypeError const& _) {
				auto msg = std::string(
					"BoltzSwapper::Env: unexpected "
					"feerates response: "
				) + Util::stringify(res);
				return Boss::log(bus, Warn, "%s", msg.c_str())
					.then([]() {
						return Ev::lift<std::uint32_t>(
							2000
						);
					});
			}
		});
	});
}

Ev::Io<bool> Env::broadcast_tx(Bitcoin::Tx n_tx) {
	auto tx = std::make_shared<Bitcoin::Tx>(std::move(n_tx));
	return Ev::yield().then([this, tx]() {
		/* Should not happen.  */
		if (!rpc)
			return Ev::lift(false);

		return rpc->command( "sendrawtransaction"
				   , Json::Out()
					.start_object()
						.field( "tx"
						      , std::string(*tx)
						      )
						.field( "allowhighfees"
						      , true
						      )
					.end_object()
		).then([](Jsmn::Object res) {
			return Ev::lift(true);
		}).catching<RpcError>([](RpcError const& _) {
			return Ev::lift(false);
		});
	});
}

Ev::Io<void> Env::logd(std::string msg) {
	return Boss::log( bus, Debug
			, "%s", msg.c_str()
			);
}
Ev::Io<void> Env::loge(std::string msg) {
	return Boss::log( bus, Warn
			, "%s", msg.c_str()
			);
}

}}}
