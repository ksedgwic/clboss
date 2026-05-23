#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Util/stringify.hpp"
#include"Uuid.hpp"
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

Ev::Io<std::string>
create_transient_layer(Boss::Mod::Rpc& rpc) {
	auto name = std::string("clboss-attempt-")
		  + std::string(Uuid::random());
	auto parms = Json::Out()
		.start_object()
			.field("layer", name)
			.field("persistent", false)
		.end_object()
		;
	return rpc.command( "askrene-create-layer"
			  , std::move(parms)
			  ).then([name](Jsmn::Object _) {
		return Ev::lift(name);
	}).catching<RpcError>([](RpcError const&) {
		/* On failure (e.g. CLN < v24.11 where askrene-create-
		 * layer does not exist), return empty so the caller
		 * can detect we have no transient layer and degrade
		 * gracefully -- crucially, NOT pass the would-be-name
		 * to subsequent getroutes calls, since askrene's
		 * param_layer_names validator (askrene.c) fails the
		 * whole getroutes call with "unknown layer" if any
		 * name in the layers array does not exist.
		 */
		return Ev::lift(std::string(""));
	});
}

Ev::Io<void>
remove_layer( Boss::Mod::Rpc& rpc
	    , std::string const& layer
	    ) {
	/* Skip the RPC roundtrip if the layer name is empty (the
	 * convention create_transient_layer uses on creation
	 * failure).  askrene-remove-layer with an empty name would
	 * error and we would swallow it, but the round-trip is
	 * wasted.
	 */
	if (layer.empty())
		return Ev::lift();
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
		.end_object()
		;
	return rpc.command( "askrene-remove-layer"
			  , std::move(parms)
			  ).then([](Jsmn::Object _) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const&) {
		return Ev::lift();
	});
}

}}}
