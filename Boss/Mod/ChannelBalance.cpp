#include"Boss/Mod/ChannelBalance.hpp"
#include"Jsmn/Object.hpp"
#include<cstddef>
#include<cstdint>

namespace Boss { namespace Mod {

ChannelBalance channel_balance(Jsmn::Object const& channel) {
	auto rv = ChannelBalance();
	rv.to_us = Ln::Amount::object(channel["to_us_msat"]);
	rv.total = Ln::Amount::object(channel["total_msat"]);

	auto inflights = channel["inflight"];
	if (!inflights.is_array())
		return rv;

	/* The lowest `splice_amount` over the inflight fundings, the
	 * rule `channeld` applies; a splice-out is negative, a
	 * splice-in positive and not credited.  */
	auto lowest = std::int64_t(0);
	auto lowest_total = Jsmn::Object();
	for (auto i = std::size_t(0); i < inflights.size(); ++i) {
		auto inflight = inflights[i];
		if (!inflight.is_object())
			continue;
		auto amount_j = inflight["splice_amount"];
		if (!amount_j.is_number())
			continue;
		auto amount = std::int64_t(double(amount_j));
		if (amount >= lowest)
			continue;
		lowest = amount;
		lowest_total = inflight["total_funding_msat"];
	}
	if (lowest >= 0)
		return rv;

	/* Ln::Amount subtraction floors at zero.  */
	auto out = Ln::Amount::sat(std::uint64_t(-lowest));
	rv.to_us = rv.to_us - out;
	if (lowest_total.is_null())
		rv.total = rv.total - out;
	else
		rv.total = Ln::Amount::object(lowest_total);
	return rv;
}

}}
