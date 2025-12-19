#include"Boss/Mod/FeeModderBySize.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ChannelCreation.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/ProvideChannelFeeModifier.hpp"
#include"Boss/Msg/SolicitChannelFeeModifier.hpp"
#include"Boss/Msg/TimerRandomDaily.hpp"
#include"Boss/Shutdown.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/Semaphore.hpp"
#include"Ev/map.hpp"
#include"Ev/now.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<algorithm>
#include<assert.h>
#include<map>
#include<math.h>
#include<set>
#include<sstream>

/*
<zmnscpxj__> the"median of peers of peer" is a reasonable starting point IMO
<darosior> FWIW i don't think it is :)
<zmnscpxj__> how so
<zmnscpxj__> ?
<darosior> From my own experience setting really high fees when you are a consequent router is the more reasonnable
<darosior> Like way more than all your peers
<zmnscpxj__> "consequent router" means what?
<darosior> I mean i've been testing different economic strategies on a ~70-100 channels node for months
<zmnscpxj__> okay, so what does "consequent router" mean? larger than your competitors?
<darosior> Ah, i meant like big enough to do some stats..

ZmnSCPxj: Not sure what "big enough to do some stats" here is but I
assume for now we are talking about large nodes with high amounts of
liquidity and channels.
Thus, this module works by getting our size, in terms of total channel
size, and comparing ourselves with the competitors.
*/

namespace {

/* Given the number of nodes that are lower-capacity than us,
 * the number who are higher-capacity, and the total number of
 * competitors, what is the multiplier on the feerates we
 * should use.
 */
double
calculate_multiplier( std::size_t worse
		    , std::size_t better
		    , std::size_t total
		    ) {
	assert(worse + better <= total);
	if (total == 0)
		return 1.0;

	/* Form our position as a number between 0.0 to 1.0.  */
	auto pos = double(worse) / double(worse + better);

	auto x = double();
	if (worse < better) {
		/* Remap position from 0 to 0.5 as 0.0 to 1.0.  */
		pos *= 2;
		/* Map position of 0 to sqrt(0.5) and 0.5 as 1.0.  */
		auto static const base = sqrt(0.5);
		x = base - pos * base + pos * 1.0;
	} else {
		/* The more total peers, the lower our fee multiplier limit.  */
		auto log_inv_total = log(1.0 / double(total));
		auto lim = 16.0 + log_inv_total;
		/* This would require the peer to have ridiculous numbers
		 * of peers.
		 */
		if (lim < 2)
			lim = 2.0;

		/* Remap position from 0.5 to 1.0 as 0.0 to 1.0.  */
		pos = pos * 2 - 1.0;
		/* Map 0.0 to sqrt(1.0) and 1.0 as sqrt(lim).  */
		auto static constexpr base = 1.0;
		x = base - pos * base + pos * sqrt(lim);
	}

	return x * x;
}

/** class NetworkCapacityTable
 *
 * @brief Keeps a table of the capacity of each node on
 * the network.
 */
class NetworkCapacityTable {
private:
	S::Bus& bus;
	Boss::Mod::Rpc& rpc;

	/* Protects these private variables.  */
	Ev::Semaphore sem;
	std::unique_ptr<std::map<Ln::NodeId, Ln::Amount>> capacities;

	Ev::Io<Ln::Amount> core_get_capacity(Ln::NodeId node) {
		return Ev::yield().then([this, node]() {
			if (!capacities) {
				return load_capacities().then([this, node]() {
					return core_get_capacity(node);
				});
			}
			auto it = capacities->find(node);
			if (it == capacities->end())
				return Ev::lift(Ln::Amount::sat(0));
			return Ev::lift(it->second);
		});
	}
	Ev::Io<void> load_capacities() {
		return Ev::lift().then([this]() {
			return Boss::log( bus, Boss::Debug
					, "FeeModderBySize: Will compute sizes "
					  "of all nodes on network."
					);
		}).then([this]() {
			capacities = Util::make_unique<std::map<Ln::NodeId, Ln::Amount>>();
			return rpc.command( "listchannels"
					  , Json::Out::empty_object()
					  );
		}).then([this](Jsmn::Object result) {
			auto pchannels = std::make_shared<Jsmn::Object>(
				result["channels"]
			);
			auto nchannels = pchannels->size();
			auto pit = std::make_shared<Jsmn::Object::iterator>(
				pchannels->begin()
			);
			return load_capacities_loop( std::move(pchannels)
						   , std::move(pit)
						   , Ev::now()
						   , 0
						   , nchannels
						   );
		});
	}
	Ev::Io<void> load_capacities_loop( std::shared_ptr<Jsmn::Object> pchannels
					 , std::shared_ptr<Jsmn::Object::iterator> pit
					 , double prev_time
					 , std::size_t processed
					 , std::size_t nchannels
					 ) {
		if (*pit == pchannels->end())
			return Boss::log( bus, Boss::Debug
					, "FeeModderBySize: Got sizes of all nodes on "
					  "network."
					);

		auto act = Ev::yield();
		if (prev_time + 5.0 < Ev::now()) {
			prev_time = Ev::now();
			act += Boss::log( bus, Boss::Info
					, "FeeModderBySize: Computing node sizes "
					  "for all network nodes: "
					  "%zu / %zu"
					, processed, nchannels
					);
		}

		return std::move(act).then([=, this]() {
			auto channel = **pit;
			++(*pit);
			auto source = Ln::NodeId(std::string(
				channel["source"]
			));
			auto amount = Ln::Amount::object(
				channel["amount_msat"]
			);
			(*capacities)[source] += amount;

			return load_capacities_loop( pchannels
						   , pit
						   , prev_time
						   , processed + 1
						   , nchannels
						   );
		});
	}

public:
	NetworkCapacityTable() =delete;
	explicit
	NetworkCapacityTable( S::Bus& bus_
		            , Boss::Mod::Rpc& rpc_
			    ) : bus(bus_)
	       		      , rpc(rpc_)
			      , sem(1)
			      { }

	Ev::Io<Ln::Amount> get_capacity(Ln::NodeId node) {
		return sem.run(core_get_capacity(std::move(node)));
	}
	Ev::Io<void> reset() {
		return sem.run(Ev::lift().then([this]() {
			capacities = nullptr;
			return Ev::lift();
		}));
	}
};


}

namespace Boss { namespace Mod {

class FeeModderBySize::Impl {
private:
	S::Bus& bus;

	Boss::Mod::Rpc* rpc;
	Ln::NodeId self_id;

	std::unique_ptr<NetworkCapacityTable> capacity_table;

	/* Cache of multipliers.  */
	std::map<Ln::NodeId, double> multipliers;

	bool shutdown;

	void start() {
		bus.subscribe<Msg::Init
			     >([this](Msg::Init const& init) {
			rpc = &init.rpc;
			self_id = init.self_id;
			capacity_table = Util::make_unique<NetworkCapacityTable>(
				bus, *rpc
			);
			return Ev::lift();
		});
		bus.subscribe<Msg::SolicitChannelFeeModifier
			     >([this
			       ](Msg::SolicitChannelFeeModifier const& _) {
			auto f = [this]( Ln::NodeId peer
				       , std::uint32_t base
				       , std::uint32_t proportional
				       ) {
				return get_cached_multiplier(peer);
			};
			return bus.raise(Msg::ProvideChannelFeeModifier{
				std::move(f)
			});
		});
		/* Reset our cached data daily to force recomputation.  */
		bus.subscribe<Msg::TimerRandomDaily
			     >([this](Msg::TimerRandomDaily const& _) {
			return reset();
		});
		/* Channel creation and destruction events can change
		 * our channel capacity, so we should reset our cached
		 * data as well.
		 */
		bus.subscribe<Msg::ChannelCreation
			     >([this](Msg::ChannelCreation const& _) {
			return reset();
		});
		bus.subscribe<Msg::ChannelDestruction
			     >([this](Msg::ChannelDestruction const& _) {
			return reset();
		});

		bus.subscribe<Shutdown>([this](Shutdown const& _) {
			shutdown = true;
			return Ev::lift();
		});
	}

	Ev::Io<void> reset() {
		multipliers.clear();

		if (!capacity_table)
			return Ev::lift();
		return Boss::concurrent(capacity_table->reset());
	}

	Ev::Io<void> wait_for_rpc() {
		return Ev::yield().then([this]() {
			if (rpc)
				return Ev::lift();
			if (shutdown)
				throw Shutdown();
			return wait_for_rpc();
		});
	}

	Ev::Io<Ln::Amount> compute_capacity(Ln::NodeId const& node) {
		return capacity_table->get_capacity(node);
	}

	Ev::Io<double> get_cached_multiplier(Ln::NodeId peer) {
		auto it = multipliers.find(peer);
		if (it != multipliers.end())
			return Ev::lift(it->second);
		return compute_multiplier(peer
					 ).then([ this
						, peer
						](double mult) {
			multipliers[peer] = mult;
			return Ev::lift(mult);
		});
	}
	Ev::Io<double> compute_multiplier(Ln::NodeId peer) {
		auto caps = std::make_shared<std::vector<Ln::Amount>>();

		return wait_for_rpc().then([this, peer]() {
			return get_competitors(peer);
		}).then([this](std::vector<Ln::NodeId> competitors) {
			auto f = [this](Ln::NodeId n) {
				return compute_capacity(n);
			};
			return Ev::map( std::move(f)
				      , std::move(competitors)
				      );
		}).then([this, caps](std::vector<Ln::Amount> n_caps) {
			*caps = std::move(n_caps);
			return compute_capacity(self_id);
		}).then([this, caps, peer](Ln::Amount my_capacity) {
			auto os = std::ostringstream();

			auto worse = std::size_t(0);
			auto better = std::size_t(0);
			auto total = std::size_t(0);
			for (auto const& c : *caps) {
				if (c < my_capacity)
					++worse;
				else if (my_capacity < c)
					++better;
				++total;
			}
			os << "Peer " << peer << " has "
			   << total << " other peers, "
			   << worse << " of which have less capacity than us, "
			   << better << " have more."
			    ;

			auto multiplier = calculate_multiplier( worse
							      , better
							      , total
							      );
			os << "  Multiplier: " << multiplier;

			return Boss::log( bus, Info
					, "FeeModderBySize: %s"
					, os.str().c_str()
					).then([multiplier]() {
				return Ev::lift(multiplier);
			});
		});
	}

	/* Get the competitors we have in the channel with the peer.
	 * These are basically the peers of that peer other than
	 * ourself.
	 */
	Ev::Io<std::vector<Ln::NodeId>>
	get_competitors(Ln::NodeId const& peer) {
		auto parms = Json::Out()
			.start_object()
				.field("source", std::string(peer))
			.end_object()
			;
		return rpc->command( "listchannels"
				   , std::move(parms)
				   ).then([this](Jsmn::Object res) {
			auto rv = std::set<Ln::NodeId>();
			try {
				auto cs = res["channels"];
				for (auto c : cs) {
					auto n = Ln::NodeId(std::string(
						c["destination"]
					));
					if (n == self_id)
						continue;
					rv.insert(n);
				}
			} catch (std::exception const& ex) {
				return Boss::log( bus, Error
						, "FeeModderBySize: "
						  "get_competitors: "
						  "Unexpected result from "
						  "listchannels: %s: %s"
						, Util::stringify(res).c_str()
						, ex.what()
						).then([rv]() {
					return Ev::lift(rv);
				});
			}
			return Ev::lift(std::move(rv));
		}).then([](std::set<Ln::NodeId> competitors) {
			/* Convert to vector.  */
			auto rv = std::vector<Ln::NodeId>(competitors.size());
			std::copy( competitors.begin(), competitors.end()
				 , rv.begin()
				 );
			return Ev::lift(std::move(rv));
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl(S::Bus& bus_) : bus(bus_)
			   , rpc(nullptr)
			   , shutdown(false)
			   {
		start();
	}
};

FeeModderBySize::FeeModderBySize(FeeModderBySize&&) =default;
FeeModderBySize::~FeeModderBySize() =default;

FeeModderBySize::FeeModderBySize(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
