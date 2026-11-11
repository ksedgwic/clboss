#include"Boltz/Service.hpp"
#include"Boltz/ServiceFactory.hpp"
#include"Boltz/SwapStatus.hpp"
#include"Boss/Mod/BoltzSwapper/ServiceCreator.hpp"
#include"Boss/Mod/BoltzSwapper/ServiceModule.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
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
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include<queue>
#include<set>
#include<sstream>
#include<stdexcept>

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
	std::queue<Planned> instances;
	std::vector<std::unique_ptr<ServiceModule>>& services;

	bool first;
	std::ostringstream report;

	Core( S::Bus& bus_
	    , Boltz::ServiceFactory factory_
	    , std::queue<Planned> instances_
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
	      , std::queue<Planned> instances
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

			auto planned = std::move(instances.front());
			instances.pop();
			auto const& instance = planned.instance;
			auto quoting = planned.quoting;
			if (first)
				first = false;
			else
				report << ", ";
			report << "Boltz::Service(\"" << instance.label << "\")";
			if (!quoting)
				report << " (swaps in flight only)";

			auto label = instance.label;

			return factory.create_service_detailed(
				instance.label, instance.clearnet, instance.onion
			).then([this, label, quoting
			       ](std::unique_ptr<Boltz::Service> core) {
				auto wrapper = Util::make_unique<ServiceModule>
					( bus, std::move(core), label, quoting );
				services.push_back(std::move(wrapper));
				return core_run();
			});
		});
	}
};

std::vector<Instance>
ServiceCreator::parse_instances(std::string const& value) {
	auto rv = std::vector<Instance>();
	auto labels = std::set<std::string>();
	auto const ws = " \t\r\n";
	auto pos = std::size_t(0);
	for (;;) {
		auto end = value.find(',', pos);
		auto entry = value.substr( pos
					 , end == std::string::npos ?
						end : end - pos
					 );
		pos = end;

		auto b = entry.find_first_not_of(ws);
		auto e = entry.find_last_not_of(ws);
		entry = (b == std::string::npos) ?
			std::string() : entry.substr(b, e - b + 1);
		while (!entry.empty() && entry.back() == '/')
			entry.pop_back();

		if (!entry.empty()) {
			auto rest = std::string();
			if (entry.rfind("https://", 0) == 0)
				rest = entry.substr(8);
			else if (entry.rfind("http://", 0) == 0)
				rest = entry.substr(7);
			auto host = rest.substr(0, rest.find_first_of("/:"));
			if ( host.empty()
			  || entry.find_first_of(ws) != std::string::npos
			   )
				throw std::invalid_argument(
					"not an http:// or https:// URL: '"
					+ entry + "'"
				);
			if (!labels.insert(entry).second)
				throw std::invalid_argument(
					"listed twice: '" + entry + "'"
				);
			auto const tld = std::string(".onion");
			auto onion = host.size() > tld.size()
				  && host.compare( host.size() - tld.size()
						 , tld.size(), tld
						 ) == 0
				   ;
			rv.push_back(Instance{
				entry,
				onion ? std::string() : entry,
				onion ? entry : std::string()
			});
		}

		if (pos == std::string::npos)
			break;
		++pos;
	}
	return rv;
}

std::vector<ServiceCreator::Planned>
ServiceCreator::plan( std::vector<Instance> const& builtin
		    , bool has_configured
		    , std::vector<Instance> const& configured
		    , std::vector<std::string> const& pending
		    , std::vector<std::string>& unknown
		    ) {
	auto rv = std::vector<Planned>();
	auto labels = std::set<std::string>();
	auto add = [&rv, &labels](Instance const& i, bool quoting) {
		if (labels.insert(i.label).second)
			rv.push_back(Planned{i, quoting});
	};

	if (!has_configured) {
		for (auto const& b : builtin)
			add(b, true);
	} else {
		for (auto const& c : configured) {
			auto known = (Instance const*) nullptr;
			for (auto const& b : builtin) {
				if ( c.label == b.label
				  || (c.clearnet != "" && c.clearnet == b.clearnet)
				  || (c.onion != "" && c.onion == b.onion)
				   ) {
					known = &b;
					break;
				}
			}
			add(known ? *known : c, true);
		}
	}

	for (auto const& p : pending) {
		if (labels.count(p) != 0)
			continue;
		auto found = false;
		for (auto const& b : builtin) {
			if (b.label != p)
				continue;
			add(b, false);
			found = true;
			break;
		}
		if (found)
			continue;
		/* The label of a configured instance is its URL.  */
		try {
			auto parsed = parse_instances(p);
			if (parsed.size() == 1 && parsed[0].label == p) {
				add(parsed[0], false);
				continue;
			}
		} catch (std::invalid_argument const&) { }
		unknown.push_back(p);
	}

	return rv;
}

void ServiceCreator::start() {
	bus.subscribe<Msg::Manifestation
		     >([this](Msg::Manifestation const& _) {
		return bus.raise(Msg::ManifestOption{
			"clboss-swap-providers", Msg::OptionType_String,
			Json::Out::direct(std::string("")),
			"Comma-separated API roots of the Boltz-API-v2 swap "
			"services to use instead of the built-in ones, e.g. "
			"`https://swap.example.com,http://example.onion/api`.  "
			"Empty (the default) uses the built-in list.",
			/* dynamic = */ false
		});
	});
	bus.subscribe<Msg::Option
		     >([this](Msg::Option const& o) {
		if (o.name != "clboss-swap-providers")
			return Ev::lift();
		auto value = o.value.is_string() ? std::string(o.value)
						 : std::string();
		try {
			auto parsed = parse_instances(value);
			configured = std::move(parsed);
			has_configured = !configured.empty();
		} catch (std::invalid_argument const& e) {
			/* Swapping through nobody is safer than through
			 * a list the operator did not write.  */
			configured.clear();
			has_configured = true;
			return Boss::log( bus, Error
					, "BoltzSwapper: clboss-swap-providers: "
					  "%s; no new swap will be started."
					, e.what()
					);
		}
		return Ev::lift();
	});
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
		auto builtin = std::vector<Instance>();
		auto it = instances.find(init.network);
		if (it != instances.end())
			builtin = it->second;

		auto factory = std::make_shared<Boltz::ServiceFactory>
			( threadpool
			, init.db
			, init.signer
			, env
			, init.proxy
			, init.always_use_proxy
			);
		auto db = init.db;
		return db.transact().then([ this, builtin, factory
					  ](Sqlite3::Tx tx) {
			/* The services we have swaps in flight with.
			 * A row is deleted when its swap is over.  */
			auto pending = std::vector<std::string>();
			auto have_table = false;
			auto check = tx.query(R"QRY(
			SELECT name FROM sqlite_master
			 WHERE type = 'table'
			   AND name = 'BoltzServiceFactory_rsub'
			     ;
			)QRY").execute();
			for (auto& r : check) {
				(void) r;
				have_table = true;
			}
			if (have_table) {
				auto fetch = tx.query(R"QRY(
				SELECT DISTINCT apiAccess
				  FROM "BoltzServiceFactory_rsub"
				     ;
				)QRY").execute();
				for (auto& r : fetch)
					pending.push_back(r.get<std::string>(0));
			}
			tx.commit();

			auto unknown = std::vector<std::string>();
			auto planned = plan( builtin
					   , has_configured, configured
					   , pending, unknown
					   );

			auto act = Ev::lift();
			for (auto const& u : unknown)
				act += Boss::log( bus, Error
						, "BoltzSwapper: swaps in flight "
						  "with \"%s\", which is neither "
						  "a built-in swap service nor a "
						  "URL; they cannot be seen "
						  "through."
						, u.c_str()
						);
			if (planned.empty())
				return std::move(act)
				     + Boss::log( bus, Debug
						, "BoltzSwapper: "
						  "No instances for this network."
						);

			auto q = std::queue<Planned>();
			for (auto const& p : planned)
				q.push(p);
			auto core = Core::create( bus
						, std::move(*factory)
						, std::move(q)
						, services
						);
			return std::move(act)
			     + Boss::concurrent(core->run());
		});
	});
}

}}}
