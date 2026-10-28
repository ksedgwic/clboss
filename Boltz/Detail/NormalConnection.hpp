#ifndef BOLTZ_DETAIL_NORMALCONNECTION_HPP
#define BOLTZ_DETAIL_NORMALCONNECTION_HPP

#include"Boltz/ConnectionIF.hpp"

namespace Ev { class ThreadPool; }

namespace Boltz { namespace Detail {

/** class Boltz::Detail::NormalConnection
 *
 * @brief handles a connection to an actual
 * Boltz server, and wrangles JSON comunications
 * with it.
 */
class NormalConnection : public Boltz::ConnectionIF {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	NormalConnection() =delete;
	NormalConnection(NormalConnection const&) =delete;

	NormalConnection(NormalConnection&&);
	~NormalConnection();
	explicit
	NormalConnection( Ev::ThreadPool& threadpool
			/* Base address of the API endpoint.  */
			, std::string api_base = "https://api.boltz.exchange"
			/* SOCKS5 proxy to use.  Empty string means no proxy.  */
			, std::string proxy = ""
			);

	/* Send an API request.  */
	Ev::Io<Jsmn::Object>
	api( std::string api /* e.g. "/createswap" */
	   /* nullptr if GET, or the request body if POST.  */
	   , std::unique_ptr<Json::Out> params
	   ) override;
};


}}

#endif /* !defined(BOLTZ_DETAIL_NORMALCONNECTION_HPP) */
