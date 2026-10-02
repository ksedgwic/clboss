#include"Boss/Mod/ChannelCreator/NodeAddress.hpp"
#include"Jsmn/Object.hpp"
#include"Net/IPAddrOrOnion.hpp"
#include"Util/make_unique.hpp"
#include<stdexcept>
#include<string>

namespace Boss { namespace Mod { namespace ChannelCreator {

std::unique_ptr<Net::IPAddrOrOnion>
node_address(Jsmn::Object const& res) {
	/* A reply of the wrong shape throws, so that the caller logs
	 * it; only the shapes listnodes does produce for a node
	 * without a usable address return nullptr.  */
	auto nodes = res["nodes"];
	if (!nodes.is_array())
		throw std::runtime_error("listnodes: nodes is not an array");
	/* Node not known?  */
	if (nodes.length() == 0)
		return nullptr;
	auto node = nodes[0];
	/* A node without a node_announcement has no addresses field.  */
	if (!node.has("addresses"))
		return nullptr;
	auto addrs = node["addresses"];
	if (!addrs.is_array())
		throw std::runtime_error("listnodes: addresses is not an array");
	for (auto addr_j : addrs) {
		/* Only these types carry an address IPAddrOrOnion can
		 * parse; a dns name or a websocket entry cannot be
		 * binned by IP.  */
		auto type = std::string(addr_j["type"]);
		if ( type != "ipv4" && type != "ipv6"
		  && type != "torv2" && type != "torv3"
		   )
			continue;
		if (!addr_j.has("address"))
			throw std::runtime_error("listnodes: address missing");
		return Util::make_unique<Net::IPAddrOrOnion>(
			std::string(addr_j["address"])
		);
	}
	return nullptr;
}

}}}
