#include"Boss/Mod/ChanneledPeers.hpp"
#include"Jsmn/Object.hpp"
#include<cstddef>

namespace {

bool has_prefix(std::string const& s, char const* prefix) {
	return s.rfind(prefix, 0) == 0;
}

}

namespace Boss { namespace Mod {

bool channeled_state(std::string const& state) {
	return has_prefix(state, "OPENINGD")
	    || has_prefix(state, "DUALOPEND")
	    || has_prefix(state, "CHANNELD")
	     ;
}

std::set<Ln::NodeId> channeled_peers(Jsmn::Object const& listpeerchannels) {
	auto rv = std::set<Ln::NodeId>();
	if (!listpeerchannels.is_object() || !listpeerchannels.has("channels"))
		return rv;
	auto channels = listpeerchannels["channels"];
	if (!channels.is_array())
		return rv;
	for (auto i = std::size_t(0); i < channels.size(); ++i) {
		auto c = channels[i];
		if (!c.is_object() || !c.has("state") || !c.has("peer_id"))
			continue;
		auto state_j = c["state"];
		auto peer_j = c["peer_id"];
		if (!state_j.is_string() || !peer_j.is_string())
			continue;
		if (!channeled_state(std::string(state_j)))
			continue;
		auto peer_s = std::string(peer_j);
		if (!Ln::NodeId::valid_string(peer_s))
			continue;
		rv.insert(Ln::NodeId(peer_s));
	}
	return rv;
}

}}
