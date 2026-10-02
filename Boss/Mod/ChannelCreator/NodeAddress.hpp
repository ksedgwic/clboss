#ifndef BOSS_MOD_CHANNELCREATOR_NODEADDRESS_HPP
#define BOSS_MOD_CHANNELCREATOR_NODEADDRESS_HPP

#include<memory>

namespace Jsmn { class Object; }
namespace Net { class IPAddrOrOnion; }

namespace Boss { namespace Mod { namespace ChannelCreator {

/** Boss::Mod::ChannelCreator::node_address
 *
 * @brief Picks, from a `listnodes` reply for one node, the first
 * address that IP binning can use: an `ipv4`, `ipv6` or Tor
 * address.
 *
 * @return nullptr when the node is unknown, has no
 * node_announcement (the reply then has no `addresses` field),
 * or announces only addresses of other types, such as `dns` or
 * `websocket`.
 */
std::unique_ptr<Net::IPAddrOrOnion>
node_address(Jsmn::Object const& listnodes_result);

}}}

#endif /* !defined(BOSS_MOD_CHANNELCREATOR_NODEADDRESS_HPP) */
