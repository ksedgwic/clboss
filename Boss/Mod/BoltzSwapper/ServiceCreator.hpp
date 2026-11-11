#ifndef BOSS_MOD_BOLTZSWAPPER_SERVICECREATOR_HPP
#define BOSS_MOD_BOLTZSWAPPER_SERVICECREATOR_HPP

#include"Boss/Mod/BoltzSwapper/ServiceModule.hpp"
#include"Boss/Msg/Network.hpp"
#include<map>
#include<memory>
#include<string>
#include<vector>

namespace Boltz { class EnvIF; }
namespace Ev { class ThreadPool; }
namespace S { class Bus; }

namespace Boss { namespace Mod { namespace BoltzSwapper {

/** struct Boss::Mod::BoltzSwapper::Instance
 *
 * @brief Describes an instance of a Boltz-like exchange.
 */
struct Instance {
	/* Label to use in database.  */
	std::string label;
	/* Clearnet API endpoint; can be "" if no clearnet.  */
	std::string clearnet;
	/* Tor API endpoint; can be "" if no onion.  */
	std::string onion;
};

/** class Boss::Mod::BoltzSwapper::ServiceCreator
 *
 * @brief Constructs `ServiceModule` objects and
 * keeps them alive.
 */
class ServiceCreator {
private:
	S::Bus& bus;
	Ev::ThreadPool& threadpool;
	Boltz::EnvIF& env;

	/* Known BOLTZ instances and the networks they are for.  */
	std::map<Boss::Msg::Network, std::vector<Instance>> const&
	instances;

	/* Instances from `clboss-swap-providers`; when the option
	 * is given these replace the built-in ones, on any network.  */
	bool has_configured;
	std::vector<Instance> configured;

	/* The actual services we are maintaining.  */
	std::vector<std::unique_ptr<Boss::Mod::BoltzSwapper::ServiceModule>>
	services;

	/* Construction is performed via this object.  */
	class Core;
	void start();

public:
	/* Parse the value of `clboss-swap-providers`: API roots
	 * separated by commas, each `http://` or `https://`.
	 * An entry is its own database label, less the scheme and
	 * any trailing `/`; a `.onion` host is reached by proxy.
	 * Throws `std::invalid_argument` naming the bad entry.  */
	static
	std::vector<Instance> parse_instances(std::string const&);

	ServiceCreator() =delete;
	ServiceCreator(ServiceCreator&&) =delete;
	ServiceCreator(ServiceCreator const&) =delete;

	ServiceCreator( S::Bus& bus_
		      , Ev::ThreadPool& threadpool_
		      , Boltz::EnvIF& env_
		      , std::map<Boss::Msg::Network, std::vector<Instance>> const& instances_
		      ) : bus(bus_)
			, threadpool(threadpool_)
			, env(env_)
			, instances(instances_)
			, has_configured(false)
			, configured()
			, services()
			{ start(); }
};

}}}

#endif /* !defined(BOSS_MOD_BOLTZSWAPPER_SERVICECREATOR_HPP) */
