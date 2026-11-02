#ifndef BOLTZ_CONNECTION_HPP
#define BOLTZ_CONNECTION_HPP

#include"Boltz/Detail/NormalConnection.hpp"

namespace Boltz {

/** class Boltz::Connection
 *
 * @brief shim for `Boltz::Detail::NormalConnection`.
 */
class Connection : public Boltz::Detail::NormalConnection {
public:
	explicit
	Connection( Ev::ThreadPool& threadpool
		  /* Base address of the API endpoint.  */
		  , std::string api_base = "https://api.boltz.exchange"
		  /* SOCKS5 proxy to use.  Empty string means no proxy.  */
		  , std::string proxy = ""
		  ) : Detail::NormalConnection( threadpool
					      , std::move(api_base)
					      , std::move(proxy)
					      )
		    { }
};

}

#endif /* !defined(BOLTZ_CONNECTION_HPP) */
