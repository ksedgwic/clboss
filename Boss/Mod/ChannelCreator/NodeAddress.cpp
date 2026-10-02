#include"Boss/Mod/ChannelCreator/NodeAddress.hpp"
#include"Jsmn/Object.hpp"
#include"Net/IPAddrOrOnion.hpp"
#include"Util/make_unique.hpp"
#include<string>

namespace Boss { namespace Mod { namespace ChannelCreator {

std::unique_ptr<Net::IPAddrOrOnion>
node_address(Jsmn::Object const& res) {
	auto nodes = res["nodes"];
	/* Node not known?  */
	if (!nodes.is_array() || nodes.length() == 0)
		return nullptr;
	auto node = nodes[0];
	/* A node without a node_announcement has no addresses field.  */
	if (!node.has("addresses"))
		return nullptr;
	auto addrs = node["addresses"];
	if (!addrs.is_array())
		return nullptr;
	for (auto addr_j : addrs) {
		if (!addr_j.has("type") || !addr_j.has("address"))
			continue;
		/* Only these types carry an address IPAddrOrOnion can
		 * parse; a dns name or a websocket entry cannot be
		 * binned by IP.  */
		auto type = std::string(addr_j["type"]);
		if ( type != "ipv4" && type != "ipv6"
		  && type != "torv2" && type != "torv3"
		   )
			continue;
		return Util::make_unique<Net::IPAddrOrOnion>(
			std::string(addr_j["address"])
		);
	}
	return nullptr;
}

}}}
