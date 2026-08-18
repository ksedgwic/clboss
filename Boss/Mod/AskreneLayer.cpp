#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"

namespace Boss { namespace Mod { namespace AskreneLayer {

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

std::string const self_layer_name = "clboss-self";

Ev::Io<bool>
ensure_self_layer( Boss::Mod::Rpc& rpc
		 , Ln::NodeId self_id
		 ) {
	auto parms = Json::Out()
		.start_object()
			.field("layer", self_layer_name)
			.field("persistent", true)
		.end_object()
		;
	return rpc.command( "askrene-create-layer"
			  , std::move(parms)
			  ).then([&rpc, self_id](Jsmn::Object _) {
		return is_node_disabled(rpc, self_layer_name, self_id);
	}).then([&rpc, self_id](bool already) {
		if (already)
			return Ev::lift();
		return disable_node(rpc, self_layer_name, self_id);
	}).then([]() {
		return Ev::lift(true);
	}).catching<RpcError>([](RpcError const&) {
		/* No askrene (CLN < v24.11) or create failed: the caller
		 * probes without the layer, as the code did before this
		 * layer existed.  is_node_disabled and disable_node
		 * swallow their own RpcErrors, so this catch fires only
		 * for create-layer -- if the disable is silently lost,
		 * the layer still exists and naming it stays safe.  */
		return Ev::lift(false);
	});
}

}}}
