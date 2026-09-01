#include"Boss/Mod/PeerJudge/DataGatherer.hpp"
#include"Boss/Mod/PeerJudge/Info.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/OnchainFee.hpp"
#include"Boss/Msg/RequestPeerStatistics.hpp"
#include"Boss/Msg/ResponsePeerStatistics.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"

namespace Boss { namespace Mod { namespace PeerJudge {

class DataGatherer::Impl {
private:
	S::Bus& bus;
	double timeframe;
	std::function<Ev::Io<double>(Ln::NodeId)> get_min_age;
	std::function<Ev::Io<void>(std::vector<Info>, double)> info_acceptor;

	ModG::ReqResp< Msg::RequestPeerStatistics
		     , Msg::ResponsePeerStatistics
		     > peerstats;

	bool have_feerate;
	double feerate;

	bool running;

	void start() {
		have_feerate = false;
		running = false;

		bus.subscribe< Msg::OnchainFee
			     >([this](Msg::OnchainFee const& m) {
			if (m.last_feerate) {
				have_feerate = true;
				feerate = *m.last_feerate;
			}
			return Ev::lift();
		});
		bus.subscribe<Msg::ListpeersResult
			     >([this](Msg::ListpeersResult const& m) {
			if (!have_feerate)
				return Ev::lift();
			if (running)
				return Ev::lift();

			auto peers = m.cpeers;

			running = true;
			auto code = Ev::lift().then([this, peers]() {
				return process_peers(peers);
			}).then([this]() {
				running = false;
				return Ev::lift();
			});
			return Boss::concurrent(code);
		});
	}
	Ev::Io<void> process_peers(Boss::Mod::ConstructedListpeers const& peers) {
		auto infos = std::make_shared<std::vector<Info>>();
		return Ev::lift().then([this, peers, infos]() {
			try {
				for (auto p : peers) {
					auto id = p.first;
					auto total = Ln::Amount::sat(0);

					for (auto c : p.second.channels) {
						auto state = std::string(
							c["state"]
						);
						if ( state != "CHANNELD_NORMAL"
						  && state != "CHANNELD_AWAITING_SPLICE"
						   )
							continue;
						total += Ln::Amount::object(
							c["total_msat"]
						);
					}

					/* No active channels?  Skip.  */
					if (total == Ln::Amount::sat(0))
						continue;
					infos->emplace_back(Info{
						id, total
					});
				}
			} catch (std::exception const& ex) {
				infos->clear();
				return Boss::log( bus, Error
						, "PeerJudge: Unexpected "
						  "listpeers result: %s: %s"
						, Util::stringify(peers).c_str()
						, ex.what()
						);
			}
			return Ev::lift();
		}).then([this, infos]() {
			if (infos->size() == 0)
				return Ev::lift();
			return filter_young(infos).then([this, infos]() {
				return gather_earnings(infos);
			});
		});
	}
	Ev::Io<void>
	filter_young(std::shared_ptr<std::vector<Info>> const& infos) {
		return filter_young_loop(infos, infos->begin());
	}
	Ev::Io<void>
	filter_young_loop( std::shared_ptr<std::vector<Info>> const& infos
			 , std::vector<Info>::iterator it
			 ) {
		return Ev::yield().then([infos, it, this]() {
			if (it == infos->end())
				return Ev::lift();
			return get_min_age(it->id).then([infos, it, this](double age) {
				if (age >= timeframe)
					return filter_young_loop(infos, it + 1);
				/* The peer is too young, remove it.  */
				auto next_it = infos->erase(it);
				return filter_young_loop(infos, next_it);
			});
		});
	}
	Ev::Io<void>
	gather_earnings(std::shared_ptr<std::vector<Info>> const& infos) {
		auto now = Ev::now();
		return peerstats.execute(Msg::RequestPeerStatistics{
			nullptr, now - timeframe, now
		}).then([this, infos](Msg::ResponsePeerStatistics m) {
			auto& stats = m.statistics;
			auto new_infos = std::vector<Info>();
			for (auto& info : *infos) {
				/* If no data, skip.  */
				if (stats.find(info.id) == stats.end())
					continue;

				auto const& stat = stats[info.id];

				/* If too young, skip.  */
				if (stat.age < timeframe)
					continue;

				/* Add earnings.  */
				info.earned = stat.in_fee + stat.out_fee;
				new_infos.emplace_back(info);
			}

			/* Did all the peers drop?  */
			if (new_infos.size() == 0)
				return Ev::lift();

			/* Call the function.  */
			return info_acceptor( std::move(new_infos)
					    , feerate
					    );
		});
	}

public:
	Impl( S::Bus& bus_
	    , double timeframe_
	    , std::function<Ev::Io<double>(Ln::NodeId)> get_min_age_
	    , std::function<Ev::Io<void>(std::vector<Info>, double)> info_acceptor_
	    ) : bus(bus_)
	      , timeframe(timeframe_)
	      , get_min_age(std::move(get_min_age_))
	      , info_acceptor(std::move(info_acceptor_))
	      , peerstats(bus_)
	      { start(); }
};

DataGatherer::DataGatherer( S::Bus& bus
			  , double timeframe_seconds
			  , std::function< Ev::Io<double>(Ln::NodeId)
					 > get_min_age
			  , std::function<Ev::Io<void>( std::vector<Info>
						      , double
						      )> fun
			  ) : pimpl(Util::make_unique< Impl
				  		     >( bus
						      , timeframe_seconds
						      , std::move(get_min_age)
						      , std::move(fun)
						      ))
			    { }
DataGatherer::~DataGatherer() =default;

}}}
