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
		).then([](Jsmn::Object res) {
			auto rate = res["perkw"]["opening"];
			return Ev::lift<std::uint32_t>(double(rate));
		});
	});
}

Ev::Io<std::string> Env::get_block(std::uint32_t height) {
	return Ev::yield().then([this, height]() {
		/* Should not happen.  */
		if (!rpc)
			return Ev::lift(std::string());

		/* The bitcoin backend plugin's command; "block" is
		 * null past our node's tip.  */
		return rpc->command( "getrawblockbyheight"
				   , Json::Out()
					.start_object()
						.field("height", height)
					.end_object()
		).then([](Jsmn::Object res) {
			if (!res.has("block") || !res["block"].is_string())
				return Ev::lift(std::string());
			return Ev::lift(std::string(res["block"]));
		}).catching<RpcError>([](RpcError const& _) {
			return Ev::lift(std::string());
		});
	});
}

Ev::Io<bool> Env::has_confirmed_output( Bitcoin::TxId txid
				      , std::uint32_t outnum
				      ) {
	auto txid_str = std::string(txid);
	return Ev::yield().then([this, txid_str, outnum]() {
		/* Should not happen.  */
		if (!rpc)
			return Ev::lift(false);

		/* Spent outputs included: the wallet may have spent
		 * the claim before this looks.  */
		return rpc->command( "listfunds"
				   , Json::Out()
					.start_object()
						.field("spent", true)
					.end_object()
		).then([txid_str, outnum](Jsmn::Object res) {
			if (!res.has("outputs") || !res["outputs"].is_array())
				return Ev::lift(false);
			for (auto out : res["outputs"]) {
				if ( !out.has("txid") || !out["txid"].is_string()
				  || !out.has("output") || !out["output"].is_number()
				  || !out.has("status") || !out["status"].is_string()
				   )
					continue;
				if ( std::string(out["txid"]) != txid_str
				  || std::uint32_t(double(out["output"])) != outnum
				   )
					continue;
				auto status = std::string(out["status"]);
				if (status == "confirmed" || status == "spent")
					return Ev::lift(true);
			}
			return Ev::lift(false);
		}).catching<RpcError>([](RpcError const& _) {
			return Ev::lift(false);
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
