#ifndef BOSS_MSG_INIT_HPP
#define BOSS_MSG_INIT_HPP

#include"Boss/Msg/Network.hpp"
#include"Ln/NodeId.hpp"
#include"Sqlite3/Db.hpp"

namespace Boss { namespace Mod { class Rpc; }}
namespace Net { class Connector; }
namespace Secp256k1 { class SignerIF; }

namespace Boss { namespace Msg {

/** struct Boss::Msg::Init
 *
 * @brief emitted when the `init` command is
 * performed.
 */
struct Init {
	Network network;
	Boss::Mod::Rpc& rpc;
	Ln::NodeId self_id;
	Sqlite3::Db db;
	Net::Connector& connector;
	Secp256k1::SignerIF& signer;
	/* Empty string if no proxy.  */
	std::string proxy;
	bool always_use_proxy;
	/* True when lightningd runs with --offline: it neither
	 * reconnects to peers nor accepts connections, and CLBOSS
	 * must not connect either.  Defaulted so that aggregate
	 * constructions without the field still compile.  */
	bool offline = false;
};

}}

#endif /* !defined(BOSS_MSG_INIT_HPP) */
