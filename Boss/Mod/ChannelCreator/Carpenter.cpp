#include"Boss/Mod/ChannelCreator/Carpenter.hpp"
#include"Boss/Mod/ChanneledPeers.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Msg/ChannelCreateResult.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<algorithm>
#include<assert.h>
#include<iterator>
#include<queue>
#include<sstream>

namespace {

/* Round down the amount to nearest satoshi.  */
Ln::Amount rounddown_to_sat(Ln::Amount in) {
	auto msat = in.to_msat();
	auto out_msat = (msat / 1000) * 1000;
	return Ln::Amount::msat(out_msat);
}

/* Thrown to skip the construction.  */
struct SkipConstruction { };

}

namespace Boss { namespace Mod { namespace ChannelCreator {

void Carpenter::start() {
	bus.subscribe< Boss::Msg::Init
		     >([this](Boss::Msg::Init const& init) {
		rpc = &init.rpc;
		return Ev::lift();
	});
}

Ev::Io<void>
Carpenter::construct(std::map<Ln::NodeId, Ln::Amount> plan) {
	assert(!plan.empty());

	if (!rpc)
		return Boss::log( bus, Error
				, "ChannelCreator: attempt to "
				  "construct before RPC available."
				);

	/* Move the plan to shared storage.  */
	auto pplan = std::make_shared<std::map<Ln::NodeId, Ln::Amount>>(
		std::move(plan)
	);
	/* Nodes that failed to create.  */
	auto pfails = std::make_shared<std::queue<Ln::NodeId>>();
	/* Nodes that successfully created.  */
	auto ppasses = std::make_shared<std::queue<Ln::NodeId>>();

	return Ev::yield().then([pplan, pfails]() {
		/* The planner can mark some nodes with value 0, indicating
		 * they have too little capacity to be worth channeling with
		 * after all.
		 * Move them to `pfails` here.
		 */
		auto tmp_fails = std::queue<Ln::NodeId>();
		for (auto const& e : *pplan)
			if (e.second == Ln::Amount::sat(0))
				tmp_fails.push(e.first);
		/* Go through it again and erase the appropriate entry.  */
		while (!tmp_fails.empty()) {
			auto n = std::move(tmp_fails.front());
			tmp_fails.pop();
			/* Remove it.  */
			auto it = pplan->find(n);
			pplan->erase(it);
			/* And move to failures.  */
			pfails->push(std::move(n));
		}

		/* The plan can now be empty, if so, skip construction.  */
		if (pplan->empty())
			throw SkipConstruction();

		return Ev::lift();
	}).then([this, pplan]() {
		/* Try to connect to all of them in parallel,
		 * as recommended in the manpage of multifundchannel.  */
		auto nodes = std::vector<Ln::NodeId>();
		std::transform( pplan->begin(), pplan->end()
			      , std::back_inserter(nodes)
			      , [](std::pair<Ln::NodeId, Ln::Amount> const& e){
			return e.first;
		});
		return Ev::foreach( std::bind( &Carpenter::connect_1
					     , this
					     , std::placeholders::_1
					     )
				  , std::move(nodes)
				  );
	}).then([this]() {
		/* Now wait a few seconds as per multifundchannel manpage.  */
		return waiter.wait(3.0);
	}).then([this]() {
		/* The plan was made minutes ago: the dowser probed every
		 * candidate, and the connects above can take minutes.  A
		 * peer may have a channel with us by now (an inbound open,
		 * the operator, a cycle that overlapped with this one),
		 * and multifundchannel would open a second one.  Check
		 * again right before funding.  */
		return rpc->command( "listpeerchannels"
				   , Json::Out::empty_object()
				   ).catching<RpcError>([this](RpcError const& e) {
			/* Without the check the plan could fund a peer
			 * that has a channel; skip the plan instead.  */
			return Boss::log( bus, Error
					, "ChannelCreator: listpeerchannels "
					  "failed, not creating channels "
					  "this cycle: %s"
					, e.error.direct_text().c_str()
					).then([]() {
				throw SkipConstruction();
				return Ev::lift(Jsmn::Object());
			});
		});
	}).then([this, pplan, pfails](Jsmn::Object res) {
		auto channeled = channeled_peers(res);
		auto act = Ev::lift();
		for (auto it = pplan->begin(); it != pplan->end();) {
			if (channeled.count(it->first) == 0) {
				++it;
				continue;
			}
			act += Boss::log( bus, Info
					, "ChannelCreator: Not funding %s, "
					  "we already have a channel with it."
					, std::string(it->first).c_str()
					);
			pfails->push(it->first);
			it = pplan->erase(it);
		}
		return act;
	}).then([pplan]() {
		if (pplan->empty())
			throw SkipConstruction();
		return Ev::lift();
	}).then([this, pplan]() {

		/* The plan might have sub-satoshi amounts that are not
		 * rounded off.
		 * Just round down everything; fees for each channel are
		 * likely to dominate anyway.
		 */
		for (auto& p : *pplan)
			p.second = rounddown_to_sat(p.second);

		/* Now construct params.  */
		auto params = Json::Out()
			.start_object()
				.field( "destinations"
				      , json_plan(*pplan)
				      )
				.field("feerate", std::string("normal"))
				.field("minchannels", (double) 1)
			.end_object()
			;
		return rpc->command("multifundchannel", std::move(params));
	}).then([this, ppasses, pfails, pplan](Jsmn::Object res) {
		auto report = std::ostringstream();
		try {
			auto chans = res["channel_ids"];
			auto first = true;
			for (auto c : chans) {
				auto node = Ln::NodeId(std::string(c["id"]));
				ppasses->push(node);
				if (first)
					first = false;
				else
					report << ", ";
				report << node << ": "
				       << (*pplan)[node]
				       ;
			}

			auto bads = res["failed"];
			if (bads.size() > 0)
				report << "; FAILED: ";
			first = true;
			for (auto b : bads) {
				auto node = Ln::NodeId(std::string(b["id"]));
				pfails->push(node);
				if (first)
					first = false;
				else
					report << ", ";
				report << node;
			}
		} catch (std::invalid_argument const& ex) {
			auto os = std::ostringstream();
			os << res;
			return Boss::log( bus, Error
					, "ChannelCreator::Carpenter: "
					  "Unexpected result from "
					  "multifundchannel: %s: %s"
					, os.str().c_str()
					, ex.what()
					);
		}

		return Boss::log( bus, Info
				, "ChannelCreator: Created: %s"
				, report.str().c_str()
				);
	}).catching<RpcError>([this, pplan, pfails](RpcError const& e) {
		/* RPC error means all failed.  */
		for (auto const& p : *pplan)
			pfails->push(p.first);
		return Boss::log( bus, Info
				, "ChannelCreator: all channels failed to "
				  "construct."
				);
	}).catching<SkipConstruction>([this](SkipConstruction const&) {
		return Boss::log( bus, Info
				, "ChannelCreator: No channels to construct."
				);
	}).then([this, pfails]() {
		return Boss::concurrent(report_channelings(
			std::move(*pfails), false
		));
	}).then([this, ppasses]() {
		return Boss::concurrent(report_channelings(
			std::move(*ppasses), true
		));
	});
}

Json::Out
Carpenter::json_single_plan(Ln::NodeId const& n, Ln::Amount const& a) {
	return Json::Out()
		.start_object()
			.field("id", std::string(n))
			.field("amount", std::string(a))
			.field("announce", true)
		.end_object()
		;
}
Json::Out
Carpenter::json_plan(std::map<Ln::NodeId, Ln::Amount> const& plan) {
	auto parms = Json::Out();
	auto arr = parms.start_array();
	for (auto const& p : plan)
		arr.entry(json_single_plan(p.first, p.second));
	arr.end_array();
	return parms;
}
Ev::Io<void>
Carpenter::connect_1(Ln::NodeId node) {
	return rpc->command("connect"
			   , Json::Out()
				.start_object()
					.field("id", std::string(node))
				.end_object()
			   ).then([](Jsmn::Object) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const& e) {
		return Ev::lift();
	});
}

Ev::Io<void>
Carpenter::report_channelings(std::queue<Ln::NodeId> nodes, bool ok) {
	auto pq = std::make_shared<std::queue<Ln::NodeId>>(std::move(
		nodes
	));
	return Ev::yield().then([this, pq, ok]() {
		if (pq->empty())
			return Ev::lift();
		return bus.raise(Msg::ChannelCreateResult{
			pq->front(), ok
		}).then([this, pq, ok](){
			return Boss::log( bus, Debug
					, "ChannelCreator: %s %s."
					, std::string(pq->front()).c_str()
					, ok ? "created" : "rejected"
					);
		}).then([this, pq, ok](){
			pq->pop();
			return report_channelings(std::move(*pq), ok);
		});
	});
}

}}}
