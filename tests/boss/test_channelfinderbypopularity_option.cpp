#undef NDEBUG
#include"Boss/Mod/ChannelFinderByPopularity.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/JsonCout.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"Net/Connector.hpp"
#include"Net/Fd.hpp"
#include"Net/SocketFd.hpp"
#include"S/Bus.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include<array>
#include<assert.h>
#include<memory>
#include<sstream>
#include<string>
#include<sys/socket.h>
#include<vector>

/* clboss-min-nodes-to-process takes a number at startup and a
 * string from setconfig; -1 selects the per-network default, at
 * once when Msg::Init has told the module the network; a word, or
 * a number followed by anything but whitespace, is refused and the
 * current value kept.  The value is observed through the log line
 * each change writes.  */

namespace {

class DummyConnector : public Net::Connector {
public:
	Net::SocketFd connect(std::string const&, int) override {
		return Net::SocketFd();
	}
};
class DummySigner : public Secp256k1::SignerIF {
public:
	Secp256k1::PubKey get_pubkey_tweak(Secp256k1::PrivKey const&) override {
		return Secp256k1::PubKey();
	}
	Secp256k1::Signature get_signature_tweak( Secp256k1::PrivKey const&
						, Sha256::Hash const&
						) override {
		return Secp256k1::Signature();
	}
	Sha256::Hash get_privkey_salted_hash(std::uint8_t salt[32]) override {
		auto hash = Sha256::Hash();
		if (salt)
			hash.from_buffer(salt);
		return hash;
	}
};

Ev::Io<std::string> setconfig(S::Bus& bus, char const* value) {
	auto reason = std::make_shared<std::string>();
	auto json = "\"" + std::string(value) + "\"";
	return bus.raise(Boss::Msg::Option{
		"clboss-min-nodes-to-process",
		Jsmn::Object::parse_json(json.c_str()),
		reason
	}).then([reason]() {
		return Ev::lift(*reason);
	});
}

}

int main() {
	auto bus = S::Bus();
	auto db = Sqlite3::Db(":memory:");
	auto sockets = std::array<int, 2>();
	auto res = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data());
	assert(res >= 0);
	auto server_end = Net::Fd(sockets[0]);
	auto rpc = Boss::Mod::Rpc(bus, Net::Fd(sockets[1]));
	auto connector = DummyConnector();
	auto signer = DummySigner();
	auto self_id = Ln::NodeId(std::string(
		"020000000000000000000000000000000000000000000000000000000000000000"
	));

	auto waiter = Boss::Mod::Waiter(bus);
	auto mut = Boss::Mod::ChannelFinderByPopularity(bus, waiter);

	auto logs = std::vector<std::string>();
	bus.subscribe<Boss::Msg::JsonCout
		     >([&](Boss::Msg::JsonCout const& m) {
		auto is = std::istringstream(m.obj.output());
		auto js = Jsmn::Object();
		is >> js;
		if ( js.is_object() && js.has("method")
		  && std::string(js["method"]) == "log"
		   )
			logs.push_back(std::string(js["params"]["message"]));
		return Ev::lift();
	});
	auto last_has = [&](char const* needle) {
		return !logs.empty()
		    && logs.back().find(needle) != std::string::npos;
	};

	auto code = Ev::lift().then([&]() {
		/* Startup delivery: a JSON number.  (A bare number is
		 * not a complete JSON text for the parser, hence the
		 * wrapping object.)  */
		return bus.raise(Boss::Msg::Option{
			"clboss-min-nodes-to-process",
			Jsmn::Object::parse_json("{\"v\": 50}")["v"],
			nullptr
		});
	}).then([&]() {
		assert(last_has("set to 50"));
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Regtest, rpc, self_id, db,
			connector, signer, "", false
		});
	}).then([&]() {
		/* The configured value survives Init.  Reverting to
		 * the default applies the regtest value at once.  */
		return setconfig(bus, "-1");
	}).then([&](std::string reason) {
		assert(reason.empty());
		assert(last_has("network default 10"));
		return setconfig(bus, "abc");
	}).then([&](std::string reason) {
		assert(!reason.empty());
		assert(last_has("keeping 10"));
		/* std::stoll alone would read these as 25 and 1.  */
		return setconfig(bus, "25abc");
	}).then([&](std::string reason) {
		assert(!reason.empty());
		assert(last_has("keeping 10"));
		return setconfig(bus, "1.5");
	}).then([&](std::string reason) {
		assert(!reason.empty());
		assert(last_has("keeping 10"));
		/* Surrounding whitespace is fine.  */
		return setconfig(bus, " 25 ");
	}).then([&](std::string reason) {
		assert(reason.empty());
		assert(last_has("set to 25"));
		/* Stop the Rpc watchers so Ev::start can return.  */
		return bus.raise(Boss::Shutdown());
	}).then([&]() {
		return Ev::lift(0);
	});

	return Ev::start(code);
}
