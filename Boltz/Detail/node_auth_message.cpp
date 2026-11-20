#include"Boltz/Detail/node_auth_message.hpp"
#include"Sha256/fun.hpp"

namespace Boltz { namespace Detail {

std::string node_auth_message( std::uint64_t ts
			     , std::string const& method
			     , std::string const& path
			     , std::string const& body
			     ) {
	return std::string("boltz-api-node-auth:")
	     + std::to_string(ts) + ":"
	     + method + ":"
	     + path + ":"
	     + std::string(Sha256::fun(body.data(), body.size()))
	     ;
}

}}
