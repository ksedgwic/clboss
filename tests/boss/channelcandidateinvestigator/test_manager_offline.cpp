#undef NDEBUG
#include"Boss/Mod/ChannelCandidateInvestigator/Main.hpp"
#include"Boss/Mod/InternetConnectionMonitor.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/RequestConnect.hpp"
#include"Boss/Msg/SolicitChannelCandidates.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/Shutdown.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/start.hpp"
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
#include<string>
#include<sys/socket.h>
#include<sys/types.h>

/* With lightningd in --offline mode the candidate investigator must
 * neither solicit candidates from the finders at startup (it does
 * so when fewer than the minimum good candidates are on record,
 * which an empty database guarantees) nor start its hourly round.
 * The same sequence with the flag off solicits at startup, which
 * shows the assertions bite.  */

namespace {

auto const self_id = "020000000000000000000000000000000000000000000000000000000000000000";

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

struct Counts {
	std::size_t solicits_after_init;
	std::size_t solicits_after_timer;
	std::size_t request_connects;
};

Counts run_case(bool offline) {
	S::Bus bus;
	Ev::ThreadPool threadpool;
	Boss::Mod::Waiter waiter(bus);
	Boss::Mod::InternetConnectionMonitor imon(bus, threadpool, waiter);
	Boss::Mod::ChannelCandidateInvestigator::Main investigator(bus, imon);

	auto solicits = std::size_t(0);
	auto request_connects = std::size_t(0);
	bus.subscribe<Boss::Msg::SolicitChannelCandidates
		     >([&](Boss::Msg::SolicitChannelCandidates const& _) {
		++solicits;
		return Ev::lift();
	});
	bus.subscribe<Boss::Msg::RequestConnect
		     >([&](Boss::Msg::RequestConnect const& _) {
		++request_connects;
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

	auto counts = Counts();
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
			offline
		});
	}).then([&]() {
		/* The startup solicitation runs as a concurrent task;
		 * give it time to happen.  */
		return waiter.wait(0.5);
	}).then([&]() {
		counts.solicits_after_init = solicits;
		return bus.raise(Boss::Msg::TimerRandomHourly());
	}).then([&]() {
		return waiter.wait(0.5);
	}).then([&]() {
		counts.solicits_after_timer = solicits;
		counts.request_connects = request_connects;
		return bus.raise(Boss::Shutdown{});
	}).then([]() {
		return Ev::lift(0);
	});

	auto ec = Ev::start(code);
	assert(ec == 0);
	return counts;
}

}

int main() {
	auto online = run_case(false);
	/* An empty record is below the minimum, so a normal start
	 * solicits.  */
	assert(online.solicits_after_init >= 1);

	auto offline = run_case(true);
	assert(offline.solicits_after_init == 0);
	assert(offline.solicits_after_timer == 0);
	assert(offline.request_connects == 0);

	return 0;
}
