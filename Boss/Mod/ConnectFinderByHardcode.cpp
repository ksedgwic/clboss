#include"Boss/Mod/ConnectFinderByHardcode.hpp"
#include"Boss/Msg/ProposeConnectCandidates.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/Network.hpp"
#include"Boss/Msg/SolicitConnectCandidates.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<map>
#include<string>
#include<vector>

namespace {

auto const all_nodes =
		std::map<Boss::Msg::Network, std::vector<std::string>>
{ { Boss::Msg::Network_Bitcoin
  , { "03e2408a49f07d2f4083a47344138ef89e7617e63919202c92aa8d49b574a560ae@65.108.246.14:19736"
    , "035e4ff418fc8b5554c5d9eea66396c227bd429a3251c8cbc711002ba215bfc226@170.75.163.209:9735"
    , "03abf6f44c355dec0d5aa155bdbdd6e0c8fefe318eff402de65c6eb2e1be55dc3e@3.132.230.42:9735"
    , "03b65c19de2da9d35895b37e6fa263cefbf8d184e6b61c0713747ebf12409d219f@164.90.241.111:9735"
    , "02d96eadea3d780104449aca5c93461ce67c1564e2e1d73225fa67dd3b997a6018@45.86.229.190:9736"
    , "031f2669adab71548fad4432277a0d90233e3bc07ac29cfb0b3e01bd3fb26cb9fa@44.242.118.94:9735"
    , "03cde60a6323f7122d5178255766e38114b4722ede08f7c9e0c5df9b912cc201d6@34.65.85.39:9745"
    , "02f1a8c87607f415c8f22c00593002775941dea48869ce23096af27b0cfdcc0b69@52.13.118.208:9735"
    , "0294ac3e099def03c12a37e30fe5364b1223fd60069869142ef96580c8439c2e0a@8.210.134.135:26658",
    }
  }
  , { Boss::Msg::Network_Testnet
    ,  { "039fbf81305e417c2acbe3b96a4929ce40b7f3537720199db9dc010b1be3c28553:167.99.21.222:19735"
       , "030b686a163aa2bba03cebb8bab7778fac251536498141df0a436d688352d426f6:65.108.246.14:19735"
       , "03781444afe27babf724304c25370f195d9d955065d7894539ed7db5fe596badb8@138.197.57.50:19735"
       , "021713d5331898c206b57c4f7d40635079de9a97d97782646f31dac18a53f2d979@66.241.124.221:9735"
       , "03e84a109cd70e57864274932fc87c5e6434c59ebb8e6e7d28532219ba38f7f6df@139.144.22.237:9735"
       , "03f060953bef5b777dc77e44afa3859d022fc1a77c55138deb232ad7255e869c00@212.46.38.66:9735"
       , "036497fee1a0b963a63f0b4374819d9016fcca9c09dbef4cb427a2d0146649b025@44.234.48.207:9735",
    }
  }
  , { Boss::Msg::Network_Signet
    ,  { "032934d7fc89c180b4b0c75342dad01ddb6ae42022848410a58d1ad0bcb7944a0e@34.139.61.243:9735"
       , "03c3cb5270df8fdb7c8cbe496430871718cc78e8b781c99a30a921a6743c3e07eb@34.138.240.132:9735"
       , "0288fa27c07c47278bc3f54e8cc8c04857c3a1accd1607e5fec406bf193f60190f@95.216.185.106:19735"
       , "022b9e44b3a8093d9512b61f5f83a72d5634201efc49718efd34f2ee851b3afa8e@52.33.33.45:9735"
       , "03f3253afe7ebc8ff903dfd05e1ab3c116bae8927d9584b269a38f3ec0c12697fb@152.69.209.45:29735"
       , "020a79f04c9368eadb9a60a9c58cb939c81a5932d4e055726d319fe1a2ad03ad89@172.233.20.188:9735"
       , "023f3f4251e010bbf365ad2b53803691da8061b49613759bd3218896cc31a392a0@91.134.58.91:39735"
    }
  }
};

}

namespace Boss { namespace Mod {

class ConnectFinderByHardcode::Impl {
private:
	S::Bus& bus;

	std::vector<std::string> const *nodes;

	void start() {
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			auto it = all_nodes.find(init.network);
			if ( it == all_nodes.end()
			  || it->second.size() == 0
			   )
				return Boss::log( bus, Warn
						, "HardcodedSeeds: "
						  "Cannot seed by hardcoded: "
						  "No known nodes for "
						  "this network."
						);
			nodes = &it->second;
			return Ev::lift();
		});
		bus.subscribe<Msg::SolicitConnectCandidates>([this](Msg::SolicitConnectCandidates const& _) {
			return solicit();
		});
	}

	Ev::Io<void> solicit() {
		if (!nodes)
			return Ev::lift();
		/* Pick 2 nodes.  */
		auto dist = std::uniform_int_distribution<size_t>(
			0, nodes->size() - 1
		);

		auto selected = std::vector<std::string>();
		selected.push_back((*nodes)[dist(random_engine)]);
		selected.push_back((*nodes)[dist(random_engine)]);

		/* Raise it.  */
		return bus.raise(Msg::ProposeConnectCandidates{
			std::move(selected)
		});
	}

public:
	Impl(S::Bus& bus_) : bus(bus_), nodes(nullptr) { start(); }
};

ConnectFinderByHardcode::ConnectFinderByHardcode
		( S::Bus& bus
		) : pimpl(Util::make_unique<Impl>(bus))
		  { }
ConnectFinderByHardcode::ConnectFinderByHardcode
		( ConnectFinderByHardcode&& o
		) : pimpl(std::move(o.pimpl))
		  { }
ConnectFinderByHardcode::~ConnectFinderByHardcode() { }


}}
