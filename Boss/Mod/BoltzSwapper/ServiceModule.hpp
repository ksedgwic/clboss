#ifndef BOSS_MOD_BOLTZSWAPPER_SERVICEMODULE_HPP
#define BOSS_MOD_BOLTZSWAPPER_SERVICEMODULE_HPP

#include<cstdint>
#include<memory>
#include<string>
#include<vector>

namespace Boltz { class Service; }
namespace Boltz { struct SwapStatus; }
namespace Ev { template<typename a> class Io; }
namespace S { class Bus; }

namespace Boss { namespace Mod { namespace BoltzSwapper {

class ServiceModule {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	ServiceModule() =delete;

	ServiceModule(ServiceModule&&);
	ServiceModule& operator=(ServiceModule&&);
	~ServiceModule();

	explicit
	ServiceModule( S::Bus& bus
		     , std::unique_ptr<Boltz::Service>
		     , std::string label
		     );

	/* For the status report: the service's label, the last
	 * block height seen (0 before the first), and its swaps
	 * in flight.  */
	std::string const& label() const;
	std::uint32_t blockheight() const;
	Ev::Io<std::vector<Boltz::SwapStatus>> list_swaps();
};

}}}

#endif /* !defined(BOSS_MOD_BOLTZSWAPPER_SERVICEMODULE_HPP) */

