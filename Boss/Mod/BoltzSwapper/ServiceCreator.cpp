#include"Boltz/Service.hpp"
#include"Boltz/ServiceFactory.hpp"
#include"Boltz/SwapStatus.hpp"
#include"Boss/Mod/BoltzSwapper/ServiceCreator.hpp"
#include"Boss/Mod/BoltzSwapper/ServiceModule.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/Network.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Ev/yield.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<queue>
#include<sstream>

namespace {

/* Where a swap stands, from what its row records.  */
std::string swap_state(Boltz::SwapStatus const& s) {
	if (!s.preimage_revealed) {
		if (s.lockup_confirmed_height == 0)
			return "awaiting_lockup";
		return "lockup_confirming";
	}
	if (s.claim_txid.empty())
		return "claim_pending";
	return "claim_broadcast";
}

}

namespace Boss { namespace Mod { namespace BoltzSwapper {

class ServiceCreator::Core : public std::enable_shared_from_this<Core> {
private:
	S::Bus& bus;
	Boltz::ServiceFactory factory;
	std::queue<Instance> instances;
	std::vector<std::unique_ptr<ServiceModule>>& services;

	bool first;
	std::ostringstream report;

	Core( S::Bus& bus_
	    , Boltz::ServiceFactory factory_
	    , std::queue<Instance> instances_
	    , std::vector<std::unique_ptr<ServiceModule>>& services_
	    ) : bus(bus_)
	      , factory(std::move(factory_))
	      , instances(std::move(instances_))
	      , services(services_)
	      { }
public:
	static
	std::shared_ptr<Core>
	create( S::Bus& bus
	      , Boltz::ServiceFactory factory
	      , std::queue<Instance> instances
	      , std::vector<std::unique_ptr<ServiceModule>>& services
	      ) {
		return std::shared_ptr<Core>(
			new Core( bus
				, std::move(factory)
				, std::move(instances)
				, services
				)
		);
	}

	Ev::Io<void> run() {
		auto self = shared_from_this();
		self->first = true;
		return self->core_run().then([self]() {
			return Ev::lift();
		});
	}

private:
	Ev::Io<void> core_run() {
		return Ev::yield().then([this]() {
			if (instances.empty()) {
				return Boss::log( bus, Debug
						, "BoltzSwapper: Created: %s"
						, report.str().c_str()
						);
			}

			auto instance = std::move(instances.front());
			instances.pop();
			if (first)
				first = false;
			else
				report << ", ";
			report << "Boltz::Service(\"" << instance.label << "\")";

			auto label = instance.label;

			return factory.create_service_detailed(
				instance.label, instance.clearnet, instance.onion
			).then([this, label](std::unique_ptr<Boltz::Service> core) {
				auto wrapper = Util::make_unique<ServiceModule>
					( bus, std::move(core), label );
				services.push_back(std::move(wrapper));
				return core_run();
			});
		});
	}
};

void ServiceCreator::start() {
	/* `clboss-status`: every service's swaps in flight, keyed
	 * by the service label.  */
	bus.subscribe<Msg::SolicitStatus
		     >([this](Msg::SolicitStatus const& _) {
		typedef std::pair< ServiceModule*
				 , std::vector<Boltz::SwapStatus>
				 > Report;
		auto reports = std::make_shared<std::vector<Report>>();
		auto modules = std::vector<ServiceModule*>();
		for (auto const& m : services)
			modules.push_back(m.get());
		auto f = [reports](ServiceModule* m) {
			return m->list_swaps().then([ reports, m
						    ](std::vector<Boltz::SwapStatus> swaps) {
				reports->emplace_back(m, std::move(swaps));
				return Ev::lift();
			});
		};
		return Ev::foreach(f, std::move(modules)
		).then([this, reports]() {
			auto out = Json::Out();
			auto obj = out.start_object();
			for (auto const& r : *reports) {
				auto height = r.first->blockheight();
				auto arr = obj.start_array(r.first->label());
				for (auto const& s : r.second) {
					auto o = arr.start_object();
					o.field("swap_id", s.swap_id)
					 .field("api", std::string(s.api_v2 ? "v2" : "v1"))
					 .field("state", swap_state(s))
					 .field("destination_address", s.destination_address)
					 .field("onchain_amount_sat", s.onchain_amount.to_sat())
					 .field("created_height", s.created_height)
					 .field("timeout_height", s.timeout_height)
					 ;
					if (height != 0) {
						o.field( "blocks_to_timeout"
						       , std::int64_t(s.timeout_height)
						       - std::int64_t(height)
						       );
						o.field("past_timeout", height >= s.timeout_height);
					}
					if (!s.lockup_txid.empty())
						o.field("lockup_txid", s.lockup_txid);
					if (s.lockup_confirmed_height != 0)
						o.field("lockup_confirmed_height", s.lockup_confirmed_height);
					if (s.lockup_searched_height != 0)
						o.field("lockup_searched_height", s.lockup_searched_height);
					o.field("preimage_revealed", s.preimage_revealed);
					if (!s.claim_txid.empty()) {
						o.field("claim_txid", s.claim_txid)
						 .field("claim_feerate_perkw", s.claim_feerate)
						 .field("claim_fee_sat", s.claim_fee.to_sat())
						 ;
					}
					if (s.spend_searched_height != 0)
						o.field("spend_searched_height", s.spend_searched_height);
					if (!s.comment.empty())
						o.field("comment", s.comment);
					o.end_object();
				}
				arr.end_array();
			}
			obj.end_object();
			return bus.raise(Msg::ProvideStatus{
				"boltz_swaps", std::move(out)
			});
		});
	});
	bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
		auto it = instances.find(init.network);
		if (it == instances.end())
			return Boss::log( bus, Debug
					, "BoltzSwapper: "
					  "No instances for this network."
					);

		auto factory = Boltz::ServiceFactory
			( threadpool
			, init.db
			, init.signer
			, env
			, init.proxy
			, init.always_use_proxy
			);
		auto q = std::queue<Instance>();
		for (auto const& instance : it->second)
			q.push(instance);
		auto core = Core::create( bus
					, std::move(factory)
					, std::move(q)
					, services
					);
		return Boss::concurrent(core->run());
	});
}

}}}
