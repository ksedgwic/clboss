#include"Boss/Mod/NeedsConnectSolicitor.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/NeedsConnect.hpp"
#include"Boss/Msg/ProposeConnectCandidates.hpp"
#include"Boss/Msg/RequestConnect.hpp"
#include"Boss/Msg/ResponseConnect.hpp"
#include"Boss/Msg/SolicitConnectCandidates.hpp"
#include"Boss/Msg/TaskCompletion.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<algorithm>
#include<chrono>
#include<iterator>
#include<queue>
#include<random>
#include<set>

namespace Boss { namespace Mod {

class NeedsConnectSolicitor::Impl {
private:
	S::Bus& bus;
	Boss::Mod::NeedsConnectSolicitor *self;

	/* Used for shuffling.  */
	std::default_random_engine engine;

	/* Solicited candidates.  */
	std::set<std::string> candidates;

	/* We run two parallel attempts to connect.
	 * We thus have these two arrays of nodes to
	 * connect to.
	 */
	std::queue<std::string> connects1;
	std::queue<std::string> connects2;

	bool connects_running;
	/* lightningd runs with --offline: nothing to solicit.  */
	bool offline = false;

	Ev::Io<void> solicit() {
		return Ev::lift().then([this]() {
			/* Clear the candidates.  */
			candidates.clear();
			connects1 = std::queue<std::string>();
			connects2 = std::queue<std::string>();

			/* Print log.  */
			return Boss::log( bus, Debug
					, "NeedsConnectSolicitor: "
					  "Soliciting candidates for "
					  "connection."
					);
		}).then([this]() {
			return bus.raise(Msg::SolicitConnectCandidates());
		}).then([this]() {
			/* Other modules should have immediately responded.  */
			if (candidates.empty())
				return fail();

			/* Shuffle.  */
			auto deck = std::vector<std::string>
					( candidates.begin()
					, candidates.end()
					);
			std::shuffle(deck.begin(), deck.end(), engine);
			candidates.clear();

			/* Now distribute them to the two connects queues.  */
			connects1 = std::queue<std::string>();
			connects2 = std::queue<std::string>();
			auto split = deck.begin() + deck.size() / 2;
			std::for_each( deck.begin(), split
				     , [this](std::string& s) {
				connects1.push(std::move(s));
			});
			std::for_each( split, deck.end()
				     , [this](std::string& s) {
				connects2.push(std::move(s));
			});

			/* And start them.  */
			return Ev::lift().then([this]() {
				if (connects1.empty())
					return Ev::lift();
				return Boss::concurrent(bus.raise(
					Msg::RequestConnect{connects1.front()}
				));
			}).then([this]() {
				if (connects2.empty())
					return Ev::lift();
				return Boss::concurrent(bus.raise(
					Msg::RequestConnect{connects2.front()}
				));
			});
		});
	}

	Ev::Io<void> response_connect( std::string const& node
				     , bool success
				     ) {
		return Ev::lift().then([this, node, success]() {
			if (!connects1.empty() && node == connects1.front())
				return response_connect1(node, success);
			else if (!connects2.empty() && node == connects2.front())
				return response_connect2(node, success);
			else
				return Ev::lift();
		});
	}
	Ev::Io<void> response_connect1( std::string const& node
				      , bool success
				      ) {
		if (success)
			return finish(node);
		/* Connect to the next one.  */
		connects1.pop();
		if (connects1.empty()) {
			if (connects2.empty())
				return fail();
			else
				return Ev::lift();
		}
		return Boss::concurrent(bus.raise(
			Msg::RequestConnect{connects1.front()}
		));
	}
	Ev::Io<void> response_connect2( std::string const& node
				      , bool success
				      ) {
		if (success)
			return finish(node);
		/* Connect to the next one.  */
		connects2.pop();
		if (connects2.empty()) {
			if (connects1.empty())
				return fail();
			else
				return Ev::lift();
		}
		return Boss::concurrent(bus.raise(
			Msg::RequestConnect{connects2.front()}
		));
	}

	Ev::Io<void> finish(std::string const& node) {
		connects_running = false;
		/* Clear the queues.  */
		connects1 = std::queue<std::string>();
		connects2 = std::queue<std::string>();
		return Boss::log( bus, Info
				, "NeedsConnectSolicitor: "
				  "Connection solicited to: %s"
				, node.c_str()
				).then([this]() { return complete(false); });
	}
	Ev::Io<void> fail() {
		/* Ran out of items to do!  */
		connects_running = false;
		return Boss::log( bus, Warn
				, "NeedsConnectSolicitor: "
				  "Ran out of candidates."
				).then([this]() { return complete(true); });
	}

	Ev::Io<void> complete(bool failed) {
		return bus.raise(Boss::Msg::TaskCompletion{
			"NeedsConnectSolicitor", self, failed
		});
	}

	void start() {
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			offline = init.offline;
			return Ev::lift();
		});
		bus.subscribe<Msg::NeedsConnect>([this](Msg::NeedsConnect const& _) {
			if (offline)
				return Boss::log( bus, Debug
						, "NeedsConnectSolicitor: "
						  "offline mode, not "
						  "soliciting connections."
						);
			if (connects_running)
				return Ev::lift();
			connects_running = true;
			return solicit();
		});
		bus.subscribe<Msg::ProposeConnectCandidates>([this](Msg::ProposeConnectCandidates const& p) {
			candidates.insert( p.candidates.begin()
					 , p.candidates.end()
					 );
			return Ev::lift();
		});
		bus.subscribe<Msg::ResponseConnect>([this](Msg::ResponseConnect const& resp) {
			if (!connects_running)
				return Ev::lift();
			return response_connect(resp.node, resp.success);
		});
	}

public:
	Impl( S::Bus& bus_
	    , Boss::Mod::NeedsConnectSolicitor *self_
	    ) : bus(bus_)
	      , self(self_)
	      /* TODO: use a better seed.  */
	      , engine(std::chrono::system_clock::now().time_since_epoch().count())
	      , connects_running(false)
	      {
		start();
	}
};

NeedsConnectSolicitor::NeedsConnectSolicitor(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus, this))
	{ }
NeedsConnectSolicitor::NeedsConnectSolicitor(NeedsConnectSolicitor&& o)
	: pimpl(std::move(o.pimpl))
	{ }
NeedsConnectSolicitor::~NeedsConnectSolicitor() { }

}}
