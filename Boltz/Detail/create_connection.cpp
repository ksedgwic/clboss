#include"Boltz/Detail/FallbackConnection.hpp"
#include"Boltz/Detail/NormalConnection.hpp"
#include"Boltz/Detail/NullConnection.hpp"
#include"Boltz/Detail/create_connection.hpp"
#include"Util/make_unique.hpp"

namespace Boltz { namespace Detail {

std::unique_ptr<Boltz::ConnectionIF>
create_connection( Ev::ThreadPool& threadpool
		 /* Can be "" if no clearnet endpoint.  */
		 , std::string clearnet_endpoint
		 /* Can be "" if no Tor endpoint.  */
		 , std::string tor_endpoint
		 /* Can be "" if no Tor proxy.  */
		 , std::string tor_proxy
		 , bool always_use_proxy
		 , Boltz::EnvIF* signer
		 ) {
	if (tor_proxy == "")
		/* Pointless anyway.  */
		tor_endpoint = "";

	if (clearnet_endpoint == "") {
		if (tor_endpoint == "")
			return Util::make_unique<NullConnection>();
		return Util::make_unique<NormalConnection>( threadpool
							  , tor_endpoint
							  , tor_proxy
							  , 600
							  , signer
							  );
	} else {
		auto first = Util::make_unique<NormalConnection>( threadpool
								, clearnet_endpoint
								, always_use_proxy ?
									tor_proxy : ""
								, 600
								, signer
								);

		auto rv = std::unique_ptr<Boltz::ConnectionIF>(std::move(first));

		if (tor_endpoint != "") {
			auto second = Util::make_unique<NormalConnection>( threadpool
									 , tor_endpoint
									 , tor_proxy
									 , 600
									 , signer
									 );
			auto combin = Util::make_unique<FallbackConnection>(
				std::move(rv), std::move(second)
			);
			rv = std::move(combin);
		}

		return rv;
	}
}

}}
