#include"Boss/Mod/ProbeChannel.hpp"
#include"Jsmn/Object.hpp"
#include<string>

namespace Boss { namespace Mod {

ProbeChannel probe_channel(Jsmn::Object const& channels) {
	auto rv = ProbeChannel();
	for (auto c : channels) {
		if (!c.has("short_channel_id"))
			continue;
		if (!c.has("spendable_msat"))
			continue;
		if (std::string(c["state"]) != "CHANNELD_NORMAL")
			continue;
		auto spendable = Ln::Amount::object(c["spendable_msat"]);
		if (rv.scid && !(spendable > rv.spendable))
			continue;
		rv.scid = Ln::Scid(std::string(c["short_channel_id"]));
		rv.spendable = spendable;
	}
	return rv;
}

}}
