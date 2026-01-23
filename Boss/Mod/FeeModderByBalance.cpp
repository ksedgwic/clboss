#include"Boss/Mod/FeeModderByBalance.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/MonitorFeeByBalance.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ProvideChannelFeeModifier.hpp"
#include"Boss/Msg/SolicitChannelFeeModifier.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<assert.h>
#include<map>
#include<math.h>
#include<random>

namespace {

/*
This module is inspired by `feeadjuster` plugin of darosior in
https://github.com/lightningd/plugins

There are some major differences.

* This runs at the schedule determined by the `Boss::Mod::ChannelFeeManager`,
  whereas the `feeadjuster` runs at every forward notification.
  * The `feeadjuster` behavior should get smoothed out by `lightningd`,
    which will defer actual fee update broadcasts, effectively merging
    multiple updates together.
* This bins more strictly in order to reduce the accuracy of the
  fee-to-amount mapping.
  The fee set is in the middle of the bin and bins are set regularly,
  whereas `feeadjuster` will create a bin centered at the current balancing
  point with fuzzy boundaries.

However, we copy the exponential function `get_ratio` below from the
`feeadjuster` code.
*/

/* our_percentage is a misnomer, as it is 0.0 = 0% owned by us,
 * 1.0 = 100% owned by us.
 */
double get_ratio(double our_percentage) {
	auto static const log50 = log(50.0);
	return exp(log50 * (0.5 - our_percentage));
}
/* Given the bin number and max bins, compute `our_percentage` and return
 * the multiplier.
 * By forcing into bins, this should reduce leakage of our actual amount
 * in the channel, while still not being too lossy.
 */
double get_ratio_by_bin(std::size_t bin, std::size_t num_bins) {
	/* bin is 0-based.  */
	assert(bin < num_bins);
	/* Get the center of each bin.  */
	auto our_percentage = double(1 + bin * 2) / double(num_bins * 2);
	return get_ratio(our_percentage);
}

/* Preferred bin size in absolute amount.  */
auto const preferred_bin_size = Ln::Amount::btc(0.002);
/* Limits on number of bins.  */
auto const min_bins = std::size_t(4);
auto const max_bins = std::size_t(50);

std::size_t get_num_bins(Ln::Amount channel_size) {
	auto raw_num_bins = std::size_t(
		round(channel_size / preferred_bin_size)
	);
	if (raw_num_bins < min_bins)
		return min_bins;
	if (raw_num_bins > max_bins)
		return max_bins;
	return raw_num_bins;
}
/* Remap to_us / channel_size to a bin index plus fraction within bin.
 * The center of a bin would be the bin index + 0.5.
 *
 * Note that this can return == num_bins if the channel is funded by us
 * and has all the money on our side!
 */
double get_bin( Ln::Amount to_us
	      , Ln::Amount channel_size
	      , std::size_t num_bins
	      ) {
	return (to_us / channel_size) * double(num_bins);
}

}

namespace Boss { namespace Mod {

class FeeModderByBalance::Impl {
private:
	S::Bus& bus;
	Boss::Mod::Rpc* rpc;

	Ev::Io<void> wait_for_rpc() {
		if (rpc)
			return Ev::lift();
		return Ev::yield().then([this]() {
			return wait_for_rpc();
		});
	}
	void start() {
		bus.subscribe<Msg::Init
			     >([this](Msg::Init const& init) {
			rpc = &init.rpc;
			return Ev::lift();
		});
		bus.subscribe<Msg::SolicitChannelFeeModifier
			     >([this](Msg::SolicitChannelFeeModifier const&) {
			return bus.raise(Msg::ProvideChannelFeeModifier{
				[this]( Ln::NodeId node
				      , std::uint32_t base
				      , std::uint32_t prop
				      ) {
					return get_multiplier(
						std::move(node)
					);
				}
			});
		});
		bus.subscribe<Msg::ChannelDestruction
			     >([this](Msg::ChannelDestruction const& d) {
			auto it = infomap.find(d.peer);
			if (it == infomap.end())
				return Ev::lift();
			infomap.erase(it);
			return Boss::log( bus, Debug
					, "FeeModderByBalance: Peer %s "
					  "channel closed."
					, std::string(d.peer).c_str()
					);
		});
	}

	struct Info {
		/* Current bin.  */
		std::size_t bin;
		/* Number of bins, based on size of channel.  */
		std::size_t num_bins;
		/* Most recently sampled channel specifications.  */
		Ln::Amount to_us;
		Ln::Amount total;
	};
	std::map<Ln::NodeId, Info> infomap;

	struct ChannelSpecs {
		Ln::Amount to_us;
		Ln::Amount total;
	};
	Ev::Io<std::unique_ptr<ChannelSpecs>>
	get_channel_specs(std::shared_ptr<Ln::NodeId> const& node) {
		return wait_for_rpc().then([this, node]() {
			auto parms = Json::Out()
				.start_object()
					.field("id", std::string(*node))
				.end_object()
				;
			return rpc->command("listpeerchannels", std::move(parms));
		}).then([this](Jsmn::Object res) {
			auto act = Ev::lift();
			auto found = false;
			auto to_us = Ln::Amount();
			auto total = Ln::Amount();
			try {
				auto cs = res["channels"];
				for (auto c : cs) {
					auto state = std::string(
						c["state"]
					);
					if (state != "CHANNELD_NORMAL")
						continue;
					found = true;
					to_us = Ln::Amount::object(
						c["to_us_msat"]
					);
					total = Ln::Amount::object(
						c["total_msat"]
					);
					break;
				}
			} catch (std::exception const& ex) {
				found = false;
				act = Boss::log( bus, Error
					       , "FeeModderByBalance: "
						 "Unexpected result from "
						 "listpeerchannels: %s: %s"
					       , Util::stringify(res).c_str()
					       , ex.what()
					       );
			}
			typedef ChannelSpecs CS;
			if (found)
				return std::move(act).then([ to_us
							   , total
							   ]() {
					auto pa = Util::make_unique<CS>();
					pa->to_us = to_us;
					pa->total = total;
					return Ev::lift(std::move(pa));
				});
			else
				return std::move(act).then([]() {
					auto pa = std::unique_ptr<CS>();
					return Ev::lift(std::move(pa));
				});
		});
	}


	Ev::Io<double> get_multiplier(Ln::NodeId n_node) {
		auto node = std::make_shared<Ln::NodeId>(std::move(n_node));
		return Ev::lift().then([this, node]() {
			return get_channel_specs(node);
		}).then([this, node](std::unique_ptr<ChannelSpecs> cs) {
			auto it = infomap.find(*node);
			if (it == infomap.end())
				return set_starting_info(node, std::move(cs));
			auto& info = it->second;
			if (!cs)
				/* Now-unknown channel, just punt and
				 * use previous info.  */
				return Ev::lift(get_ratio_from_info(info));

			if ( info.to_us == cs->to_us
			  && info.total == cs->total
			   )
				/* No change, use current cached info.  */
				return Ev::lift(get_ratio_from_info(info));

			/* If total size of channel changed, recompute
			 * bins.
			 * This will not happen as of C-Lightning 0.9.1
			 * but maybe in the future when we have splicing.
			 */
			if (info.total != cs->total)
				return set_starting_info(node, std::move(cs));

			/* Implied by above.  */
			assert(info.to_us != cs->to_us);

			auto actual_bin = get_bin( cs->to_us
						 , cs->total
						 , info.num_bins
						 );
			auto target_bin = std::size_t(floor(actual_bin));
			/* No change?  */
			if (target_bin == info.bin)
				return Ev::lift(get_ratio_from_info(info));

			info.to_us = cs->to_us;

			auto dist = std::uniform_real_distribution<double>(
				0.0, 1.0
			);

			auto prev_bin = info.bin;

			if (target_bin < info.bin) {
				auto prob = 1.0 - (actual_bin - target_bin);
				if (dist(Boss::random_engine) < prob)
					info.bin = target_bin;
				else
					info.bin = target_bin + 1;
			} else {
				auto prob = actual_bin - target_bin;
				if (dist(Boss::random_engine) < prob)
					info.bin = target_bin;
				else
					info.bin = target_bin - 1;
				if (info.bin >= info.num_bins)
					info.bin = info.num_bins - 1;
			}

			auto mult = get_ratio_from_info(info);
			auto to_us = info.to_us;
			auto total = info.total;

			return Boss::log( bus, Debug
					, "FeeModderByBalance: Peer %s moved "
					  "from bin %zu to bin %zu of %zu due "
					  "to balance %s / %s: %g"
					, std::string(*node).c_str()
					, prev_bin, info.bin
					, info.num_bins
					, std::string(info.to_us).c_str()
					, std::string(info.total).c_str()
					, mult
					).then([this, node, mult, to_us, total]() {
				return bus.raise(Msg::MonitorFeeByBalance{
					*node,
					mult,
					to_us.to_msat(),
					total.to_msat()
				});
			}).then([mult]() {
				return Ev::lift(mult);
			});
		});
	}
	Ev::Io<double> set_starting_info( std::shared_ptr<Ln::NodeId
							 > const& node
					, std::unique_ptr<ChannelSpecs> cs
					) {
		if (!cs)
			return Ev::lift(1.0);

		auto& info = infomap[*node];
		info.to_us = cs->to_us;
		info.total = cs->total;
		info.num_bins = get_num_bins(cs->total);
		info.bin = floor(get_bin( cs->to_us
					, cs->total
					, info.num_bins
					));
		/* If the channel is newly singly-funded by us,
		 * we own all of it and the return of get_bin can
		 * be equal to num_bins.
		 */
		if (info.bin >= info.num_bins)
			info.bin = info.num_bins - 1;
		auto mult = get_ratio_from_info(info);
		auto to_us = info.to_us;
		auto total = info.total;
		return Boss::log( bus, Debug
				, "FeeModderByBalance: Peer %s set to bin "
				  "%zu of %zu due to balance %s / %s: %g"
				, std::string(*node).c_str()
				, info.bin
				, info.num_bins
				, std::string(info.to_us).c_str()
				, std::string(info.total).c_str()
				, mult
				).then([this, node, mult, to_us, total]() {
			return bus.raise(Msg::MonitorFeeByBalance{
				*node,
				mult,
				to_us.to_msat(),
				total.to_msat()
			});
		}).then([mult]() {
			return Ev::lift(mult);
		});
	}
	double get_ratio_from_info(Info const& info) {
		return get_ratio_by_bin(info.bin, info.num_bins);
	}

public:
	Impl() =delete;
	Impl(S::Bus& bus_) : bus(bus_), rpc(nullptr) { start(); }
};

FeeModderByBalance::~FeeModderByBalance() =default;
FeeModderByBalance::FeeModderByBalance(FeeModderByBalance&&) =default;

FeeModderByBalance::FeeModderByBalance(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
