#undef NDEBUG
#include"Boss/Mod/Connector.hpp"
#include"Boss/Mod/NeedsConnectSolicitor.hpp"
#include"Boss/Mod/Reconnector.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/NeedsConnect.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Boss/Msg/ProposeConnectCandidates.hpp"
#include"Boss/Msg/RequestConnect.hpp"
#include"Boss/Msg/ResponseConnect.hpp"
#include"Boss/Msg/SolicitConnectCandidates.hpp"
#include"Boss/Msg/TaskCompletion.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Jsmn/Object.hpp"
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
#include<assert.h>
#include<errno.h>
#include<string>
#include<sys/socket.h>
#include<sys/types.h>
#include<vector>

/* With lightningd in --offline mode, the modules that dial peers
 * must not: the Connector answers every request as a failure
 * without an RPC call, the NeedsConnectSolicitor ignores the
 * connect trigger, and the Reconnector stays quiet on disconnect
 * notifications.  The RPC socket's far end is never read; if any
 * module sent a command it would sit there, and the check at the
 * end would find it.  */

namespace {

auto const self_id = "020000000000000000000000000000000000000000000000000000000000000000";
auto const peer_id = "020000000000000000000000000000000000000000000000000000000000000001";

class DummyConnector : public Net::Connector {
public:
	Net::SocketFd
	connect(std::string const& host, int port) override {
		(void) host;
		(void) port;
		return Net::SocketFd();
	}
};

class DummySigner : public Secp256k1::SignerIF {
public:
	Secp256k1::PubKey
	get_pubkey_tweak(Secp256k1::PrivKey const& tweak) override {
		(void) tweak;
		return Secp256k1::PubKey();
	}

	Secp256k1::Signature
	get_signature_tweak( Secp256k1::PrivKey const& tweak
			   , Sha256::Hash const& m
			   ) override {
		(void) tweak;
		(void) m;
		return Secp256k1::Signature();
	}

	Sha256::Hash
	get_privkey_salted_hash(std::uint8_t salt[32]) override {
		if (!salt)
			return Sha256::Hash();
		auto hash = Sha256::Hash();
		hash.from_buffer(salt);
		return hash;
	}
};

/* Offers candidates whenever asked, so a solicitation that did run
 * would produce connection attempts.  */
class DummyConnectDiscoverer {
public:
	std::size_t solicitations;
	DummyConnectDiscoverer(S::Bus& bus) : solicitations(0) {
		bus.subscribe<Boss::Msg::SolicitConnectCandidates
			     >([this, &bus](Boss::Msg::SolicitConnectCandidates const& _) {
			++solicitations;
			return bus.raise(Boss::Msg::ProposeConnectCandidates{
				std::vector<std::string>{peer_id}
			});
		});
	}
	DummyConnectDiscoverer(DummyConnectDiscoverer&&) =delete;
};

bool socket_is_silent(Net::Fd const& fd) {
	char buf[64];
	auto rd = recv(fd.get(), buf, sizeof(buf), MSG_DONTWAIT);
	return rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

}

int main() {
	S::Bus bus;
	Boss::Mod::Connector connector_mod(bus);
	Boss::Mod::NeedsConnectSolicitor solicitor(bus);
	Boss::Mod::Reconnector reconnector(bus);
	DummyConnectDiscoverer discoverer(bus);

	auto request_connects = std::size_t(0);
	auto needs_connects = std::size_t(0);
	auto task_completions = std::size_t(0);
	auto responses = std::vector<Boss::Msg::ResponseConnect>();
	bus.subscribe<Boss::Msg::RequestConnect
		     >([&](Boss::Msg::RequestConnect const& _) {
		++request_connects;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::NeedsConnect
		     >([&](Boss::Msg::NeedsConnect const& _) {
		++needs_connects;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::TaskCompletion
		     >([&](Boss::Msg::TaskCompletion const& _) {
		++task_completions;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::ResponseConnect
		     >([&](Boss::Msg::ResponseConnect const& r) {
		responses.push_back(r);
		return Ev::lift();
	});

	auto connector = DummyConnector();
	auto signer = DummySigner();
	auto db = Sqlite3::Db(":memory:");
	int sockets[2];
	auto sockres = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	assert(sockres >= 0);
	auto server_socket = Net::Fd(sockets[0]);
	auto client_socket = Net::Fd(sockets[1]);
	auto rpc = Boss::Mod::Rpc(bus, std::move(client_socket));

	auto code = Ev::lift().then([&]() {
		return bus.raise(Boss::Msg::Init{
			Boss::Msg::Network_Regtest,
			rpc,
			Ln::NodeId(self_id),
			db,
			connector,
			signer,
			std::string(),
			false,
			true /* offline */
		});
	}).then([&]() {
		/* A direct request is answered as a failure, without
		 * reaching lightningd.  */
		return bus.raise(Boss::Msg::RequestConnect{peer_id});
	}).then([&]() {
		assert(request_connects == 1);
		assert(responses.size() == 1);
		assert(responses[0].node == peer_id);
		assert(!responses[0].success);

		/* The connect trigger is ignored: no solicitation, no
		 * requests, and no task completion for the internet
		 * monitor to react to.  */
		return bus.raise(Boss::Msg::NeedsConnect());
	}).then([&]() {
		assert(needs_connects == 1);
		assert(discoverer.solicitations == 0);
		assert(request_connects == 1);
		assert(task_completions == 0);

		/* A disconnect notification does not re-trigger
		 * connection.  */
		return bus.raise(Boss::Msg::Notification{
			"disconnect", Jsmn::Object::parse_json("{}")
		});
	}).then([&]() {
		assert(needs_connects == 1);
		assert(request_connects == 1);
		assert(socket_is_silent(server_socket));

		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);
	return 0;
}
