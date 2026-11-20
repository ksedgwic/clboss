#ifndef BOLTZ_DETAIL_CREATE_CONNECITON_HPP
#define BOLTZ_DETAIL_CREATE_CONNECITON_HPP

#include<memory>
#include<string>

namespace Boltz { class ConnectionIF; }
namespace Boltz { class EnvIF; }
namespace Ev { class ThreadPool; }

namespace Boltz { namespace Detail {

/** Boltz::Detail::create_connection
 *
 * @brief Creates a `Boltz::ConnectionIF` object appropriate
 * for the given setup for a Boltz-like service.
 */
std::unique_ptr<Boltz::ConnectionIF>
create_connection( Ev::ThreadPool& threadpool
		 /* Can be "" if no clearnet endpoint.  */
		 , std::string clearnet_endpoint
		 /* Can be "" if no Tor endpoint.  */
		 , std::string tor_endpoint
		 /* Can be "" if no Tor proxy.  */
		 , std::string tor_proxy
		 , bool always_use_proxy
		 /* Signs requests with our node key; nullptr
		  * sends them unsigned.  */
		 , Boltz::EnvIF* signer = nullptr
		 );

}}

#endif /* !defined(BOLTZ_DETAIL_CREATE_CONNECITON_HPP) */
