#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Util/stringify.hpp"
#include<assert.h>

namespace Boss { namespace Mod { namespace AskreneLayer {

std::string const clboss_layer_name = "clboss";

namespace {

/* Common machinery for the two inform_channel variants.  askrene
 * accepts inform=succeeded / constrained / unconstrained as the
 * only difference between them; everything else (scid_dir,
 * amount_msat, layer) is identical.
 */
Ev::Io<void>
inform_channel( Boss::Mod::Rpc& rpc
	      , std::string const& layer
	      , Ln::Scid scid
	      , std::uint32_t direction
	      , Ln::Amount amount
	      , char const* inform
	      ) {
	/* askrene only accepts direction 0 or 1 in
	 * short_channel_id_dir.  All callers feed values from
	 * CLN's getroutes/sendpay responses, which are
	 * guaranteed to be 0/1, but guard explicitly: a bad
	 * direction would produce a syntactically valid but
	 * semantically wrong RPC param that askrene rejects, and
	 * the silent-swallow RpcError handler below would drop
	 * the learning update without a trace.
	 */
	assert(direction <= 1);
	if (direction > 1)
		return Ev::lift();
	auto sdir = std::string(scid) + "/" + Util::stringify(direction);
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
			.field("short_channel_id_dir", sdir)
			.field("amount_msat", amount.to_msat())
			.field("inform", std::string(inform))
		.end_object()
		;
	return rpc.command( "askrene-inform-channel"
			  , std::move(parms)
			  ).then([](Jsmn::Object _) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const&) {
		/* Non-fatal -- if the layer is missing (e.g. layer-
		 * create failed at startup on CLN < v24.11),
		 * subsequent getroutes calls simply will not benefit
		 * from the constraint.  Better to degrade learning
		 * than to crash the caller.
		 */
		return Ev::lift();
	});
}

}

Ev::Io<void>
inform_channel_constrained( Boss::Mod::Rpc& rpc
			  , std::string const& layer
			  , Ln::Scid scid
			  , std::uint32_t direction
			  , Ln::Amount amount
			  ) {
	return inform_channel(rpc, layer, scid, direction, amount, "constrained");
}

Ev::Io<void>
inform_channel_unconstrained( Boss::Mod::Rpc& rpc
			    , std::string const& layer
			    , Ln::Scid scid
			    , std::uint32_t direction
			    , Ln::Amount amount
			    ) {
	return inform_channel(rpc, layer, scid, direction, amount, "unconstrained");
}

Ev::Io<void>
update_channel( Boss::Mod::Rpc& rpc
	      , std::string const& layer
	      , Ln::Scid scid
	      , std::uint32_t direction
	      , bool enabled
	      , Ln::Amount htlc_minimum_msat
	      , Ln::Amount htlc_maximum_msat
	      , Ln::Amount fee_base_msat
	      , std::uint32_t fee_proportional_millionths
	      , std::uint16_t cltv_expiry_delta
	      ) {
	/* Same direction-validity guard as inform_channel: askrene
	 * only accepts 0 or 1 in short_channel_id_dir.
	 */
	assert(direction <= 1);
	if (direction > 1)
		return Ev::lift();
	auto sdir = std::string(scid) + "/" + Util::stringify(direction);
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
			.field("short_channel_id_dir", sdir)
			.field("enabled", enabled)
			.field("htlc_minimum_msat", htlc_minimum_msat.to_msat())
			.field("htlc_maximum_msat", htlc_maximum_msat.to_msat())
			.field("fee_base_msat", fee_base_msat.to_msat())
			.field( "fee_proportional_millionths"
			      , fee_proportional_millionths
			      )
			.field( "cltv_expiry_delta"
			      , cltv_expiry_delta
			      )
		.end_object()
		;
	return rpc.command( "askrene-update-channel"
			  , std::move(parms)
			  ).then([](Jsmn::Object _) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const&) {
		return Ev::lift();
	});
}

Ev::Io<bool>
is_node_disabled( Boss::Mod::Rpc& rpc
		, std::string const& layer
		, Ln::NodeId node
		) {
	auto target = std::string(node);
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
		.end_object()
		;
	return rpc.command( "askrene-listlayers"
			  , std::move(parms)
			  ).then([target = std::move(target)
				 ](Jsmn::Object res) {
		try {
			auto layers = res["layers"];
			if (!layers.is_array() || layers.size() == 0)
				return Ev::lift(false);
			auto layer_obj = layers[0];
			if (!layer_obj.has("disabled_nodes"))
				return Ev::lift(false);
			auto disabled = layer_obj["disabled_nodes"];
			if (!disabled.is_array())
				return Ev::lift(false);
			for (auto entry : disabled) {
				if (std::string(entry) == target)
					return Ev::lift(true);
			}
		} catch (std::exception const&) {
			/* Malformed response shape -- fall through to
			 * false so the caller continues without
			 * deduping rather than crashing.
			 */
		}
		return Ev::lift(false);
	}).catching<RpcError>([](RpcError const&) {
		/* Conservative on RPC error: returning false lets
		 * the caller fall through to its disable_node call
		 * (which also swallows RpcError).  Worst case is
		 * an accumulating duplicate, same as the pre-dedup
		 * behaviour.
		 */
		return Ev::lift(false);
	});
}

Ev::Io<void>
disable_node( Boss::Mod::Rpc& rpc
	    , std::string const& layer
	    , Ln::NodeId node
	    ) {
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
			.field("node", std::string(node))
		.end_object()
		;
	return rpc.command( "askrene-disable-node"
			  , std::move(parms)
			  ).then([](Jsmn::Object _) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const&) {
		return Ev::lift();
	});
}

}}}
