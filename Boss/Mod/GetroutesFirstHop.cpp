#include"Boss/Mod/GetroutesFirstHop.hpp"
#include"Jsmn/Object.hpp"
#include<string>

namespace Boss { namespace Mod {

GetroutesFirstHop::GetroutesFirstHop(Jsmn::Object const& route) {
	auto path = route["path"];
	auto hop = path[0];
	if ( hop.has("node_id_out")
	  && hop.has("amount_out_msat")
	  && hop.has("cltv_out")
	   ) {
		/* CLN v26.06 and later, or a backport build.  */
		node_id_out = Ln::NodeId(std::string(hop["node_id_out"]));
		amount_out = Ln::Amount::object(hop["amount_out_msat"]);
		cltv_out = std::uint32_t(double(hop["cltv_out"]));
		return;
	}
	/* Stock v26.04: derive the out-side values from the
	 * deprecated trio.  next_node_id is the hop's out-node as
	 * is; the deprecated amount_msat and delay are in-side
	 * values, so the first hop's out-values are the second
	 * hop's, or the route's delivered amount and final_cltv on
	 * a single-hop route.  Any field missing here means the
	 * response carries neither form; the accesses below then
	 * throw Jsmn::TypeError for the caller's malformed-response
	 * path.  */
	node_id_out = Ln::NodeId(std::string(hop["next_node_id"]));
	if (path.size() > 1) {
		auto hop1 = path[1];
		amount_out = Ln::Amount::object(hop1["amount_msat"]);
		cltv_out = std::uint32_t(double(hop1["delay"]));
	} else {
		amount_out = Ln::Amount::object(route["amount_msat"]);
		cltv_out = std::uint32_t(double(route["final_cltv"]));
	}
}

}}
