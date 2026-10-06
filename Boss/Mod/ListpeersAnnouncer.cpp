#include"Boss/Mod/ListpeersAnnouncer.hpp"
#include"Boss/Mod/ConstructedListpeers.hpp"
#include"Boss/ModG/RpcProxy.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<sstream>

namespace Boss { namespace Mod {

/*
 * IMPORTANT - the msg is no longer directly obtained from
 * `listpeers` but rather is constructed by "convolving" the value
 * from `listpeerchannels`.  Specifically, the top level `peer`
 * objects are non-standard and only have what CLBOSS uses ...
 *
 * `listpeerchannels` lists channels, so a peer without one is not
 * in it.  Those peers are taken from `listpeers`, which is read for
 * `id` and `connected` only.
 */

void ListpeersAnnouncer::start() {
	auto invalid_result = [this](char const* command) {
		return Boss::log( bus, Error
				, "ListpeersAnnouncer: invalid result from "
				  "`%s`."
				, command
				);
	};
	auto do_listpeerchannels = [ this
			    , invalid_result
			    ](bool initial, Jsmn::Object peers) {
		return rpc->command("listpeerchannels"
				   , Json::Out::empty_object()
				   ).then([ this
					  , initial
					  , invalid_result
					  , peers
					  ](Jsmn::Object result) {
			if (!result.is_object() || !result.has("channels"))
				return invalid_result("listpeerchannels");
			auto channels = result["channels"];
			if (!channels.is_array())
				return invalid_result("listpeerchannels");

			Boss::Mod::ConstructedListpeers cpeers;
			for (auto c : channels) {
				auto id = Ln::NodeId(std::string(c["peer_id"]));
				auto connected = c["peer_connected"];
				cpeers[id].connected = connected.is_boolean() && bool(connected);
				cpeers[id].channels.push_back(c);
			}
			/* Add the peers without channels.  */
			for (auto p : peers) {
				if (!p.is_object() || !p.has("id"))
					continue;
				auto id_j = p["id"];
				if (!id_j.is_string())
					continue;
				auto id_s = std::string(id_j);
				if (!Ln::NodeId::valid_string(id_s))
					continue;
				auto id = Ln::NodeId(id_s);
				if (cpeers.count(id) != 0)
					continue;
				auto connected = false;
				if (p.has("connected")) {
					auto connected_j = p["connected"];
					connected = connected_j.is_boolean()
						 && bool(connected_j)
						  ;
				}
				cpeers[id].connected = connected;
			}

			return bus.raise(Msg::ListpeersResult{
				std::move(cpeers), initial
			});
		});
	};
	/* `listpeers` goes first.  A peer that gets its first channel
	 * between the two commands is then described by its channel,
	 * instead of being announced as a peer without channels.  */
	auto do_listpeers = [ this
			    , invalid_result
			    , do_listpeerchannels
			    ](bool initial) {
		return rpc->command("listpeers"
				   , Json::Out::empty_object()
				   ).then([ initial
					  , invalid_result
					  , do_listpeerchannels
					  ](Jsmn::Object result) {
			if (!result.is_object() || !result.has("peers"))
				return invalid_result("listpeers");
			auto peers = result["peers"];
			if (!peers.is_array())
				return invalid_result("listpeers");
			return do_listpeerchannels(initial, std::move(peers));
		});
	};
	bus.subscribe<Msg::Init
		     >([do_listpeers](Msg::Init const& init) {
		return Boss::concurrent(do_listpeers(true));
	});
	bus.subscribe<Msg::Timer10Minutes
		     >([do_listpeers](Msg::Timer10Minutes const& _) {
		return Boss::concurrent(do_listpeers(false));
	});
}

ListpeersAnnouncer::ListpeersAnnouncer(S::Bus& bus_)
	: bus(bus_)
	, rpc(Util::make_unique<ModG::RpcProxy>(bus))
	{ start(); }

ListpeersAnnouncer::~ListpeersAnnouncer() =default;
}}
