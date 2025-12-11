#include"Boss/Mod/BoltzSwapper/Env.hpp"
#include"Boss/Mod/BoltzSwapper/Main.hpp"
#include"Boss/Mod/BoltzSwapper/ServiceCreator.hpp"
#include"Util/make_unique.hpp"

namespace {

auto const boltz_instances = std::map< Boss::Msg::Network
				     , std::vector<Boss::Mod::BoltzSwapper::Instance>
				     >
{ { Boss::Msg::Network_Bitcoin
/* Historically, the clearnet API endpoint was used as the in-database label
 * for Boltz services.
 * The boltz.exchange service thus uses the API endpoint as the label for
 * back-compatibility.
 * Other services since then were added after we had a separate label.
 */
  , { { "https://boltz.exchange/api"
      , "https://boltz.exchange/api"
      , "http://boltzzzbnus4m7mta3cxmflnps4fp7dueu2tgurstbvrbt6xswzcocyd.onion/api"
      }
    , { "AutonomousOrganization@github.com"
      , ""
      , "http://jsyqqszgfrya6nj7nhi4hu4tdpuvfursl7dyxeiukzit5mvckqbzxpad.onion"
      }
    , { "Boltz_API_v2_mainnet"
      , "https://api.boltz.exchange"
      , "http://boltzzzbnus4m7mta3cxmflnps4fp7dueu2tgurstbvrbt6xswzcocyd.onion/api"
      }
    }
  }
, { Boss::Msg::Network_Testnet
  , { { "https://testnet.boltz.exchange/api"
      , "https://testnet.boltz.exchange/api"
      , "http://tboltzzrsoc3npe6sydcrh37mtnfhnbrilqi45nao6cgc6dr7n2eo3id.onion/api"
      }
    , { "Boltz_API_v2_testnet3"
      , "https://api.testnet.boltz.exchange"
      , "http://boltzzzbnus4m7mta3cxmflnps4fp7dueu2tgurstbvrbt6xswzcocyd.onion/api"
      }
    }
  }
};

}

namespace Boss { namespace Mod { namespace BoltzSwapper {

class Main::Impl {
private:
	Env env;
	ServiceCreator creator;

public:
	Impl(S::Bus& bus, Ev::ThreadPool& threadpool)
		: env(bus)
		, creator(bus, threadpool, env, boltz_instances)
		{ }
};

Main::Main(Main&&) =default;
Main& Main::operator=(Main&&) =default;
Main::~Main() =default;

Main::Main( S::Bus& bus
	  , Ev::ThreadPool& threadpool
	  ) : pimpl(Util::make_unique<Impl>(bus, threadpool))
	    { }

}}}

