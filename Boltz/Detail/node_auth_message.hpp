#ifndef BOLTZ_DETAIL_NODE_AUTH_MESSAGE_HPP
#define BOLTZ_DETAIL_NODE_AUTH_MESSAGE_HPP

#include<cstdint>
#include<string>

namespace Boltz { namespace Detail {

/** Boltz::Detail::node_auth_message
 *
 * @brief the message a provider that authenticates
 * Lightning nodes expects our node to sign for a
 * request:
 * "boltz-api-node-auth:<ts>:<method>:<path>:<sha256 of
 * the body, in hex>".
 *
 * @desc `path` is the API path, e.g. "/v2/swap/reverse",
 * without the endpoint's base, so the signature holds
 * whatever prefix a reverse proxy adds or strips.
 */
std::string node_auth_message( std::uint64_t ts
			     , std::string const& method
			     , std::string const& path
			     , std::string const& body
			     );

}}

#endif /* !defined(BOLTZ_DETAIL_NODE_AUTH_MESSAGE_HPP) */
