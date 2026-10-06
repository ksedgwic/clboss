#include"Boss/Mod/ChannelCandidateInvestigator/Gumshoe.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/RpcProxy.hpp"
#include"Boss/Msg/RequestConnect.hpp"
#include"Boss/Msg/ResponseConnect.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<set>

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator {

class Gumshoe::Impl {
private:
	S::Bus& bus;
	ModG::RpcProxy rpc;

	std::function< Ev::Io<void>( Ln::NodeId
				   , bool
				   )
		     > report;

	void start() {
		bus.subscribe<Msg::ResponseConnect>([this](Msg::ResponseConnect const& rc) {
			return on_response_connect(rc);
		});
	}

	/* Nodes we are investigating.  */
	std::set<Ln::NodeId> cases;

	Ev::Io<void> investigate_core(Ln::NodeId n) {
		return Boss::log( bus, Debug
				, "ChannelCandidateInvestigator: "
				  "Investigating %s."
				, std::string(n).c_str()
				).then([this, n]() {
			return is_connected(n);
		}).then([this, n](bool connected) {
			/* A connection we did not make shows that the node
			 * is online, and is not ours to drop afterwards.  */
			if (connected)
				return Boss::log( bus, Debug
						, "ChannelCandidateInvestigator: "
						  "%s is online (already "
						  "connected)."
						, std::string(n).c_str()
						).then([this, n]() {
					return report(n, true);
				});
			cases.insert(n);
			return bus.raise(Msg::RequestConnect{std::string(n)});
		});
	}

	Ev::Io<bool> is_connected(Ln::NodeId n) {
		auto params = Json::Out()
			.start_object()
				.field("id", std::string(n))
			.end_object()
			;
		return rpc.command("listpeers", std::move(params)
				  ).then([](Jsmn::Object res) {
			if (!res.is_object() || !res.has("peers"))
				return Ev::lift(false);
			auto peers = res["peers"];
			if (!peers.is_array())
				return Ev::lift(false);
			for (auto peer : peers) {
				if (!peer.is_object() || !peer.has("connected"))
					continue;
				auto connected = peer["connected"];
				if (connected.is_boolean() && bool(connected))
					return Ev::lift(true);
			}
			return Ev::lift(false);
		}).catching<RpcError>([](RpcError const& _) {
			return Ev::lift(false);
		});
	}

	Ev::Io<void> on_response_connect(Msg::ResponseConnect const& rc) {
		/* Was it a plain nodeid like what we generate?  */
		if (!Ln::NodeId::valid_string(rc.node))
			return Ev::lift();

		/* Is it one of the cases we are watching?  */
		auto node = Ln::NodeId(rc.node);
		auto it = cases.find(node);
		if (it == cases.end())
			return Ev::lift();

		auto success = rc.success;

		/* Done with this case.  */
		cases.erase(it);
		return Boss::log( bus, Debug
				, "ChannelCandidateInvestigator: %s is %s."
				, rc.node.c_str()
				, success ? "online" : "offline"
				).then([this, node, success]() {
			return report(node, success);
		}).then([this, node, success]() {
			if (!success)
				return Ev::lift();
			return disconnect(node);
		});
	}

	/* The connection was made for the probe and nothing else uses
	 * it.  `disconnect` fails for a peer that has a channel with
	 * us, and the failure is ignored.  */
	Ev::Io<void> disconnect(Ln::NodeId n) {
		auto params = Json::Out()
			.start_object()
				.field("id", std::string(n))
			.end_object()
			;
		return rpc.command("disconnect", std::move(params)
				  ).then([](Jsmn::Object _) {
			return Ev::lift();
		}).catching<RpcError>([](RpcError const& _) {
			return Ev::lift();
		});
	}

public:
	Impl( S::Bus& bus_
	    ) : bus(bus_)
	      , rpc(bus_)
	      , report(nullptr)
	      {
		start();
	}

	void set_report_func(std::function< Ev::Io<void>( Ln::NodeId
							, bool
							)
					  > report_) {
		report = std::move(report_);
	}

	Ev::Io<void> investigate(Ln::NodeId n) {
		return Boss::concurrent(Ev::lift().then([this, n]() {
			return investigate_core(n);
		}));
	}
};

Gumshoe::Gumshoe(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }
Gumshoe::~Gumshoe() { }


void Gumshoe::set_report_func(std::function< Ev::Io<void>( Ln::NodeId
							 , bool
							 )
					   > report) {
	return pimpl->set_report_func(std::move(report));
}
Ev::Io<void> Gumshoe::investigate(Ln::NodeId n) {
	return pimpl->investigate(std::move(n));
}

}}}
