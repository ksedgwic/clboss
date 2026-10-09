#include"Boss/Mod/InternetConnectionMonitor.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/InternetOnline.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/NeedsConnect.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/TaskCompletion.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/map.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Net/Connector.hpp"
#include"Net/SocketFd.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<algorithm>
#include<assert.h>
#include<iterator>
#include<random>
#include<string>

namespace {

/* How long before we timeout an LN-level ping attempt
 * with a peer and consider it to have failed?  */
auto const ln_ping_timeout = double(30.0);

/* List of servers that we try to connect to.
 * When we think we might get disconnected, we select
 * one of these by random and try to connect.
 * If the connection succeeds we consider ourselves
 * to be connected.
 * If the connection fails we try *all* of the below
 * and if at least one connects, we still consider
 * ourselves to be connected.
 *
 * Note that we connect, then disconnect immediately.
 * We just want to check the Internet, not spam these
 * servers.
 *
 * It would probably be better to "just" IP-level `ping`
 * the server, but that is suprisingly complicated.
 * Either we require some kind of `ping` command and
 * parse its output (including figuring out what
 * exactly its arguments are, its version, the exact
 * variant of the `ping` command, etc.), or we need to
 * open an ICMP socket, which, depending on OS policies,
 * might not be possible for non-root users (`ping` is
 * traditionally a setuid program).
 */
auto const servers = std::vector<std::pair<std::string, int>>
{ {"www.duckduckgo.com", 443}
, {"www.google.com", 443}
, {"www.yahoo.com", 443}
, {"www.facebook.com", 443}
, {"www.amazon.com", 443}
, {"www.reddit.com", 443}
, {"www.twitter.com", 443}
, {"www.alibaba.com", 443}
, {"www.netflix.com", 443}
, {"www.baidu.com", 443}
, {"www.paypal.com", 443}
, {"www.microsoft.com", 443}
, {"www.mozilla.org", 443}
, {"1.1.1.1", 443}
, {"www.squareup.com", 443}
, {"wikipedia.org", 443}
, {"www.bitcoin.org", 443}
, {"bitcoinstats.com", 443}
, {"www.iana.org", 443}
, {"www.example.org", 443}
, {"github.com", 443}
, {"lightning.network", 443}
, {"www.blockstream.com", 443}
, {"acinq.co", 443}
};
auto dist = std::uniform_int_distribution<std::size_t>(
	0, servers.size() - 1
);

}

namespace Boss { namespace Mod {

class InternetConnectionMonitor::Impl {
private:
	S::Bus& bus;
	Ev::ThreadPool& threadpool;
	Boss::Mod::Waiter& waiter;

	Net::Connector *connector;
	Boss::Mod::Rpc *rpc;

	bool online;
	bool checking_connectivity;

	void start() {
		bus.subscribe< Msg::Init
			     >([this](Msg::Init const& init) {
			connector = &init.connector;
			rpc = &init.rpc;
			checking_connectivity = true;
			return Boss::concurrent(server_check());
		});
		bus.subscribe< Msg::ListpeersAnalyzedResult
			     >([this](Msg::ListpeersAnalyzedResult const& l) {
			if (!connector || checking_connectivity)
				return Ev::lift();
			checking_connectivity = true;

			/* Copy connected nodes.  */
			return periodic_check(l);
		});
		/* If the NeedsConnectSolicitor failed, it might be due to
		 * Internet connection problems.  */
		bus.subscribe< Msg::TaskCompletion
			     >([this](Msg::TaskCompletion const& comp) {
			if (!connector || checking_connectivity)
				return Ev::lift();
			if (comp.comment != "NeedsConnectSolicitor")
				return Ev::lift();
			if (!comp.failed)
				return Ev::lift();
			checking_connectivity = true;
			return server_check();
		});
		/* Report status.  */
		bus.subscribe< Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const&) {
			auto status = Json::Out()
				.start_object()
					.field( "connection"
					      , online ? std::string("online")
						       : std::string("offline")
					      )
					.field( "checking_connectivity"
					      , checking_connectivity
					      )
				.end_object()
				;
			return bus.raise(Msg::ProvideStatus{
				"internet", std::move(status)
			});
		});
	}

	Ev::Io<void> server_check() {
		/* No peers, so query connectivity via server connecting.  */
		return Ev::lift().then([this]() {
			return check_server_connectivity()
					.then([this](bool n_online) {
				return set_online(n_online);
			});
		});
	}

	Ev::Io<bool> check_server_connectivity() {
		assert(connector);
		auto i = dist(Boss::random_engine);
		return check_server(servers[i]).then([this, i](bool res) {
			if (!res)
				return Boss::log( bus, Debug
						, "InternetConnectionMonitor: "
						  "Could not contact %s, "
						  "will contact other servers."
						, servers[i].first.c_str()
						).then([this]() {
					return check_all_servers();
				});
			else
				return Ev::lift(true);
		});
	}
	Ev::Io<bool> check_all_servers() {
		using std::placeholders::_1;
		return Ev::map( std::bind(&Impl::check_server, this, _1)
			      , servers
			      ).then([this](std::vector<bool> results) {
			auto bitvector = std::string("");
			/* If one of the servers passed, it turns out we
			 * are connected after all.  */
			auto res = false;
			for (auto r : results) {
				if (r) {
					res = true;
					bitvector += "1";
				} else
					bitvector += "0";
			}
			auto act = Ev::lift();
			if (res)
				act += Boss::log( bus, Debug
						, "InternetConnectionMonitor: "
						  "Other server contacted, "
						  "we are online after all. "
						  "(%s)"
						, bitvector.c_str()
						);
			else
				act += Boss::log( bus, Debug
						, "InternetConnectionMonitor: "
						  "Other servers also not "
						  "reached. (%s)"
						, bitvector.c_str()
						);
			return std::move(act).then([res]() {
				return Ev::lift(res);
			});
		});
	}
	Ev::Io<bool> check_server(std::pair<std::string, int> const& s) {
		return threadpool.background<bool>([this, s]() {
			auto fd = connector->connect(s.first, s.second);
			return !!fd;
		});
	}

	Ev::Io<void> set_online(bool n_online) {
		auto was_online = online;
		online = n_online;

		auto act = Ev::lift();
		act += Boss::log( bus, Info
				, "InternetConnectionMonitor: %s."
				, online ? "online" : "offline"
				);
		if (was_online != online)
			act += bus.raise(Msg::InternetOnline{online});
		if (online) {
			checking_connectivity = false;
			/* If we moved from offline->online,
			 * raise NeedsConnect to get us back
			 * to gossiping.
			 */
			if (!was_online)
				act += bus.raise(Msg::NeedsConnect());
		} else
			act += offline_quick_loop();
		return act;
	}

	/* If we are offline, we should keep trying to connect in a
	 * quick 5-second loop.
	 * This is not an attack on the rest of the Internet, because
	 * we are offline and cannot access it anyway.  */
	Ev::Io<void> offline_quick_loop() {
		return waiter.wait(5.0).then([this]() {
			return check_server_connectivity();
		}).then([this](bool result) {
			if (result)
				return set_online(true);
			else
				return offline_quick_loop();
		});
	}

	Ev::Io<void> periodic_check(Msg::ListpeersAnalyzedResult const& r) {
		/* No connected peers?
		 * Query servers directly.
		 */
		if ( r.connected_channeled.empty()
		  && r.connected_unchanneled.empty()
		   )
			return Boss::concurrent(server_check());

		/* Otherwise pick a random peer to ping.  Take one with a
		 * channel if there is one: `Boss::Mod::AutoDisconnector`
		 * handles this same message and may be disconnecting
		 * the peers without channels.  */
		auto const& candidates = r.connected_channeled.empty()
				       ? r.connected_unchanneled
				       : r.connected_channeled
				       ;
		auto peers = std::vector<Ln::NodeId>();
		std::copy( candidates.begin()
			 , candidates.end()
			 , std::back_inserter(peers)
			 );
		auto dist_peers = std::uniform_int_distribution<std::size_t>(
			0, peers.size() - 1
		);
		auto i = dist_peers(Boss::random_engine);
		return Boss::concurrent(ln_ping_check(peers[i]));
	}

	Ev::Io<void> ln_ping_check(Ln::NodeId const& p) {
		/* Prefer to check our connectivity by pinging a
		 * peer directly.
		 * Use a timeout from Mod::Waiter; on some systems,
		 * even if you are disconnected physically, TCP
		 * connections are still kept alive for some time,
		 * leading to long delays in the LN-level `ping`.
		 */
		return Ev::lift().then([this, p]() {
			return waiter.timed( ln_ping_timeout
					   , do_ln_ping(p)
					   ).catching< Waiter::TimedOut
						     >([](Waiter::TimedOut const& _) {
				return Ev::lift(false);
			});
		}).then([this, p](bool online) {
			if (online)
				return Boss::log( bus, Debug
						, "InternetConnectionMonitor: "
						  "ping %s success."
						, std::string(p).c_str()
						).then([this]() {
					return set_online(true);
				});
			return Boss::log( bus, Debug
					, "InternetConnectionMonitor: "
					  "ping %s failed."
					, std::string(p).c_str()
					).then([this]() {
				/* Fall back to connecting to a server.  */
				return server_check();
			});
		});
	}

	Ev::Io<bool> do_ln_ping(Ln::NodeId const& p) {
		return Ev::lift().then([this, p]() {
			auto p_s = std::string(p);
			auto presult = std::make_shared<bool>();
			return rpc->command( "ping"
					   , Json::Out()
						.start_object()
							.field("id", p_s)
						.end_object()
					   ).then([](Jsmn::Object _) {
				return Ev::lift(true);
			}).catching<RpcError>([](RpcError const& e) {
				return Ev::lift(false);
			}).then([this, presult](bool result) {
				*presult = result;
				return Boss::log( bus, Debug
						, "InternetConnectionMonitor: "
						  "do_ln_ping: ping %s."
						, result ? "success" : "failed"
						);
			}).then([presult]() {
				return Ev::lift(*presult);
			});
		});
	}

public:
	Impl() =delete;
	explicit
	Impl( S::Bus& bus_
	    , Ev::ThreadPool& threadpool_
	    , Boss::Mod::Waiter& waiter_
	    ) : bus(bus_)
	      , threadpool(threadpool_)
	      , waiter(waiter_)
	      , connector(nullptr)
	      , online(false)
	      , checking_connectivity(false)
	      { start(); }

	bool is_online() const { return online; }
};

InternetConnectionMonitor::InternetConnectionMonitor
		(InternetConnectionMonitor&& o)
	: pimpl(std::move(o.pimpl)) { }
InternetConnectionMonitor::~InternetConnectionMonitor() { }
InternetConnectionMonitor::InternetConnectionMonitor
		( S::Bus& bus
		, Ev::ThreadPool& threadpool
		, Boss::Mod::Waiter& waiter
		) : pimpl(Util::make_unique<Impl>(bus, threadpool, waiter))
		  { }

bool InternetConnectionMonitor::is_online() const {
	return pimpl->is_online();
}

}}
