#include"Boss/Mod/FundsMover/Attempter.hpp"
#include"Boss/Mod/FundsMover/create_label.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Preimage.hpp"
#include"Ln/Scid.hpp"
#include"Sha256/Hash.hpp"
#include"Util/stringify.hpp"
#include<assert.h>
#include<string>
#include<vector>

namespace {

/* Name of the persistent askrene layer that holds CLBOSS's
 * failure-feedback knowledge -- channel capacity constraints
 * (askrene-inform-channel) and node-level exclusions
 * (askrene-disable-node) accumulated across attempts.  Created
 * idempotently at startup by Boss::Mod::FundsMover::Main.
 *
 * Naming follows the convention established by plugins/xpay
 * (which uses "xpay"): the persistent shared-knowledge layer is
 * named after the plugin that owns it.
 */
auto const clboss_layer = std::string("clboss");

}

namespace Boss { namespace Mod { namespace FundsMover {

class Attempter::Impl : public std::enable_shared_from_this<Impl> {
private:
	S::Bus& bus;

	Boss::Mod::Rpc& rpc;
	Ln::NodeId self_id;
	Ln::Preimage preimage;
	Ln::Preimage payment_secret;
	Ln::NodeId source;
	Ln::NodeId destination;
	Ln::Amount amount;
	std::shared_ptr<Ln::Amount> fee_budget;
	std::shared_ptr<Ln::Amount> remaining_amount;
	/* Details of the last channel from destination to us.  */
	Ln::Scid last_scid;
	Ln::Amount base_fee;
	std::uint32_t proportional_fee;
	std::uint32_t cltv_delta;
	/* Details of the first channel from us to source.  */
	Ln::Scid first_scid;

	bool ok;

	Ln::Amount dest_amount;
	Ln::Amount source_amount;
	std::uint32_t source_delay;

	/* Route from source to destination, not including the
	 * us->source and destination->us hops.  Populated from
	 * getroutes' `path[]` array, with each hop translated into the
	 * sendpay-compatible shape (id, scid, direction, amount_msat,
	 * delay).
	 */
	struct Hop {
		Ln::NodeId id;
		Ln::Scid scid;
		std::uint32_t direction;
		Ln::Amount amount_msat;
		std::uint32_t delay;
	};
	std::vector<Hop> route;

	/* The fee we currently have.  */
	Ln::Amount our_fee;

public:
	Impl( S::Bus& bus_
	    , Boss::Mod::Rpc& rpc_
	    , Ln::NodeId self_id_
	    , Ln::Preimage preimage_
	    , Ln::Preimage payment_secret_
	    , Ln::NodeId source_
	    , Ln::NodeId destination_
	    , Ln::Amount amount_
	    , std::shared_ptr<Ln::Amount> fee_budget_
	    , std::shared_ptr<Ln::Amount> remaining_amount_
	    , Ln::Scid last_scid_
	    , Ln::Amount base_fee_
	    , std::uint32_t proportional_fee_
	    , std::uint32_t cltv_delta_
	    , Ln::Scid first_scid_
	    ) : bus(bus_)
	      , rpc(rpc_)
	      , self_id(std::move(self_id_))
	      , preimage(std::move(preimage_))
	      , payment_secret(std::move(payment_secret_))
	      , source(std::move(source_))
	      , destination(std::move(destination_))
	      , amount(amount_)
	      , fee_budget(std::move(fee_budget_))
	      , remaining_amount(std::move(remaining_amount_))
	      , last_scid(last_scid_)
	      , base_fee(base_fee_)
	      , proportional_fee(proportional_fee_)
	      , cltv_delta(cltv_delta_)
	      , first_scid(first_scid_)
	      , ok(false)
	      { }
	Ev::Io<bool> run() {
		auto self = shared_from_this();
		return self->core_run().then([self]() {
			return Ev::lift(self->ok);
		});
	}

private:
	Ev::Io<void> core_run() {
		return Ev::lift().then([this]() {
			/* dest_amount is the amount destination should
			 * receive on its incoming channel from us.  Old
			 * comment is preserved because the math is the
			 * same -- the legacy code used this in getroute's
			 * amount_msat; we now use it as getroutes'
			 * amount_msat (the amount delivered to the
			 * destination peer of askrene's request).
			 */
			dest_amount = amount + base_fee
				    + (amount * ( double(proportional_fee)
						/ 1000000
						))
				    + Ln::Amount::msat(1)
				    ;
			return getroute();
		});
	}
	Ev::Io<void> getroute() {
		return Ev::yield().then([this]() {
			/* Prorate our share of the global fee budget and
			 * pass it to askrene as a hard maxfee_msat cap.
			 * The prorated budget covers our TOTAL fee
			 * (source_amount - amount), but askrene's
			 * maxfee_msat only constrains the middle-route
			 * portion (source_amount - dest_amount).  The
			 * destination's outgoing-channel fee
			 * (dest_amount - amount) is a fixed cost we
			 * already know, so subtract it from the budget
			 * before handing it to askrene.  If the
			 * destination's last-hop fee alone exceeds our
			 * budget, we cannot proceed; askrene will
			 * return 206 even faster than we would compute
			 * it locally.  No fuzz-ladder retry analogue in
			 * askrene's probability-based cost model.
			 */
			assert(amount <= *remaining_amount);
			auto prorata = amount / *remaining_amount;
			auto prorated_fee_budget = *fee_budget * prorata;
			auto dest_hop_fee = dest_amount - amount;
			auto route_maxfee = prorated_fee_budget > dest_hop_fee
					  ? prorated_fee_budget - dest_hop_fee
					  : Ln::Amount::sat(0);

			auto parms = Json::Out()
				.start_object()
					.field("source", std::string(source))
					.field("destination",
					       std::string(destination))
					.field("amount_msat",
					       dest_amount.to_msat())
					.start_array("layers")
						.entry("auto.localchans")
						.entry("auto.sourcefree")
						.entry(clboss_layer)
					.end_array()
					.field("maxfee_msat",
					       route_maxfee.to_msat())
					.field("final_cltv",
					       cltv_delta + 14)
					.field("maxparts", 1)
				.end_object()
				;
			return rpc.command("getroutes", std::move(parms));
		}).then([this](Jsmn::Object res) {
			try {
				auto routes = res["routes"];
				if (!routes.is_array() || routes.size() == 0)
					throw Jsmn::TypeError();
				auto r0 = routes[0];
				auto path = r0["path"];
				if (!path.is_array() || path.size() == 0)
					throw Jsmn::TypeError();

				/* getroutes path[] hop fields were renamed
				 * in CLN v26.06.  Which set of names
				 * actually appears in the response
				 * depends on the CLN version AND whether
				 * the node runs with developer mode
				 * (which suppresses deprecated outputs):
				 *
				 *   v26.04                  -> old only
				 *   v26.06+ no developer    -> both emitted
				 *   v26.06+ developer=true  -> new only
				 *
				 * Bridge by preferring the new name,
				 * falling back to the old.  TODO: drop
				 * the fallback once CLN v26.04 is no
				 * longer supported and the old names are
				 * removed in v27.06.
				 *
				 * short_channel_id_dir is older (v24.11)
				 * and is emitted unconditionally.
				 */
				route.clear();
				for (auto hop_j : path) {
					Hop hop;
					hop.id = Ln::NodeId(std::string(
						hop_j.has("node_id_out")
							? hop_j["node_id_out"]
							: hop_j["next_node_id"]
					));
					auto sdir = std::string(
						hop_j["short_channel_id_dir"]
					);
					auto slash = sdir.find('/');
					if (slash == std::string::npos)
						throw Jsmn::TypeError();
					hop.scid = Ln::Scid(
						sdir.substr(0, slash)
					);
					hop.direction = std::uint32_t(
						std::stoul(sdir.substr(slash + 1))
					);
					hop.amount_msat = Ln::Amount::object(
						hop_j.has("amount_out_msat")
							? hop_j["amount_out_msat"]
							: hop_j["amount_msat"]
					);
					hop.delay = std::uint32_t(double(
						hop_j.has("cltv_out")
							? hop_j["cltv_out"]
							: hop_j["delay"]
					));
					route.push_back(hop);
				}
			} catch (std::exception const&) {
				/* Broaden catch to std::exception so we
				 * also handle std::invalid_argument and
				 * std::out_of_range from std::stoul on a
				 * malformed short_channel_id_dir tail.
				 * The other accesses in this block only
				 * raise Jsmn::TypeError (subclass of
				 * std::exception), so they remain caught.
				 * Matches the same fix in
				 * ActiveProber.cpp's parse block.
				 */
				return Boss::log( bus, Error
						, "FundsMover: Unexpected "
						  "getroutes response: %s"
						, Util::stringify(res)
							.c_str()
						).then([]() {
					return Ev::lift(false);
				});
			}
			return Ev::lift(true);
		}).catching<RpcError>([this](RpcError const& e) {
			/* Errors 205 ("Unable to find a route"), 206
			 * ("Route too expensive"), and any others mean
			 * we cannot proceed.  No retry: askrene already
			 * considered our maxfee_msat budget and the
			 * accumulated failure-feedback layer.
			 */
			return Boss::log( bus, Debug
					, "FundsMover: getroutes failed "
					  "(%s); giving up attempt to "
					  "move %s from %s to %s."
					, Util::stringify(e.error).c_str()
					, std::string(amount).c_str()
					, std::string(source).c_str()
					, std::string(destination).c_str()
					).then([]() {
				return Ev::lift(false);
			});
		}).then([this](bool ok) {
			if (ok)
				return compute_source_amount();
			return Ev::lift();
		});
	}

	/* Compute source_amount (what we send to source) and
	 * source_delay (initial CLTV) from the first hop of the route
	 * plus a listchannels round-trip to learn the source's
	 * outgoing-channel fees.
	 *
	 * Once CLN v26.04 is no longer supported, this can be folded
	 * back into getroute(): getroutes' new path[0].amount_in_msat
	 * and path[0].cltv_in (both added v26.06) give these values
	 * directly without the listchannels call.
	 */
	Ev::Io<void> compute_source_amount() {
		return Ev::lift().then([this]() {
			auto parms = Json::Out()
				.start_object()
					.field( "short_channel_id"
					      , std::string(route[0].scid)
					      )
				.end_object()
				;
			return rpc.command("listchannels", std::move(parms));
		}).then([this](Jsmn::Object res) {
			auto found = false;
			auto src_base_fee = Ln::Amount::sat(0);
			auto src_prop_fee = std::uint32_t(0);
			auto src_cltv_delta = std::uint32_t(0);
			try {
				auto cs = res["channels"];
				for (auto c : cs) {
					auto csrc = Ln::NodeId(std::string(
						c["source"]
					));
					if (csrc != source)
						continue;
					src_base_fee = Ln::Amount::msat(double(
						c["base_fee_millisatoshi"]
					));
					src_prop_fee = std::uint32_t(double(
						c["fee_per_millionth"]
					));
					src_cltv_delta = std::uint32_t(double(
						c["delay"]
					));
					found = true;
				}
			} catch (Jsmn::TypeError const&) {
				return Boss::log( bus, Error
						, "FundsMover: Unexpected "
						  "listchannels response: %s"
						, Util::stringify(res)
							.c_str()
						);
			}
			if (!found)
				return Boss::log( bus, Debug
						, "FundsMover: listchannels "
						  "did not include the "
						  "source's outgoing channel "
						  "%s; giving up attempt."
						, std::string(route[0].scid)
							.c_str()
						);

			source_amount = route[0].amount_msat + src_base_fee
				      + (route[0].amount_msat
					 * ( double(src_prop_fee)
					   / 1000000
					   ))
				      + Ln::Amount::msat(1)
				      ;
			source_delay = route[0].delay + src_cltv_delta;
			our_fee = source_amount - amount;

			/* Defensive: askrene's maxfee_msat constrains
			 * the middle-route portion of our_fee.  If
			 * gossip drift or rounding has produced a
			 * route whose actual fee exceeds our prorated
			 * budget, bail rather than overspend.
			 */
			assert(amount <= *remaining_amount);
			auto prorata = amount / *remaining_amount;
			auto prorated_fee_budget = *fee_budget * prorata;
			if (our_fee > prorated_fee_budget)
				return Boss::log( bus, Debug
						, "FundsMover: our_fee %s "
						  "exceeds prorated budget "
						  "%s; giving up attempt."
						, std::string(our_fee).c_str()
						, std::string(
							prorated_fee_budget
						  ).c_str()
						);

			*fee_budget -= our_fee;
			*remaining_amount -= amount;
			return sendpay();
		}).catching<RpcError>([this](RpcError const& e) {
			return Boss::log( bus, Debug
					, "FundsMover: listchannels failed "
					  "(%s); giving up attempt."
					, Util::stringify(e.error).c_str()
					);
		});
	}

	/* Tell askrene about a failed-channel constraint so future
	 * getroutes calls steer around it.  Recorded into the
	 * persistent "clboss" layer; benefits all subsequent attempts
	 * and (when they migrate to consume the same layer) other
	 * CLBOSS subsystems too.
	 */
	Ev::Io<void> inform_channel_constrained( Ln::Scid scid
					       , int dir
					       , Ln::Amount at
					       ) {
		auto sdir = std::string(scid) + "/" + Util::stringify(dir);
		auto parms = Json::Out()
			.start_object()
				.field("layer", clboss_layer)
				.field("short_channel_id_dir", sdir)
				.field("amount_msat", at.to_msat())
				.field("inform", "constrained")
			.end_object()
			;
		return rpc.command( "askrene-inform-channel"
				  , std::move(parms)
				  ).then([](Jsmn::Object _) {
			return Ev::lift();
		}).catching<RpcError>([](RpcError const&) {
			/* Non-fatal -- if the layer is missing
			 * (e.g. layer-create failed at startup on a
			 * CLN < v24.11), subsequent getroutes calls
			 * simply will not benefit from the constraint.
			 */
			return Ev::lift();
		});
	}

	/* Tell askrene to avoid a node entirely; same persistent
	 * layer.  Used for NODE-level onion failures (failcode bit
	 * 0x2000) and for unparsable-onion fallback.
	 */
	Ev::Io<void> disable_node(Ln::NodeId node) {
		auto parms = Json::Out()
			.start_object()
				.field("layer", clboss_layer)
				.field("node", std::string(node))
			.end_object()
			;
		return rpc.command( "askrene-disable-node"
				  , std::move(parms)
				  ).then([](Jsmn::Object _) {
			return Ev::lift();
		}).catching<RpcError>([](RpcError const&) {
			return Ev::lift();
		});
	}

	Ev::Io<void> sendpay() {
		auto payment_hash = std::make_shared<Sha256::Hash>();
		return Ev::lift().then([this, payment_hash]() {
			*payment_hash = preimage.sha256();
			auto label = create_label(*payment_hash);
			auto parms = Json::Out()
				.start_object()
					.field("route", make_route())
					.field( "payment_hash"
					      , std::string(*payment_hash)
					      )
					.field("label" , label)
					.field( "payment_secret"
					      , std::string(payment_secret)
					      )
				.end_object()
				;
			return rpc.command("sendpay", std::move(parms));
		}).then([this, payment_hash](Jsmn::Object _) {
			auto parms = Json::Out()
				.start_object()
					.field( "payment_hash"
					      , std::string(*payment_hash)
					      )
				.end_object()
				;
			return rpc.command("waitsendpay", std::move(parms));
		}).then([this, payment_hash](Jsmn::Object _) {
			/* Success?  Now we can stop.  */
			ok = true;
			return delpay(payment_hash, true)
			     + Boss::log( bus, Info
					, "FundsMover: Moved %s from %s, "
					  "getting %s to %s, costing us "
					  "%s."
					, std::string(source_amount).c_str()
					, std::string(source).c_str()
					, std::string(amount).c_str()
					, std::string(destination).c_str()
					, std::string(our_fee).c_str()
					)
			     ;
		}).catching<RpcError>([ this
				      , payment_hash
				      ](RpcError const& e) {
			/* Starting action: delete failing moves.  */
			auto act = delpay(payment_hash, false);

			/* Return our fee to the budget.  */
			*fee_budget += our_fee;
			*remaining_amount += amount;

			/* Figure out the error.  */
			auto code = int();
			auto eidx = std::size_t();
			auto echan = Ln::Scid();
			auto edir = int();
			auto enode = Ln::NodeId();
			auto fail = std::uint16_t();
			try {
				auto& error = e.error;
				code = int(double(
					error["code"]
				));
				/* Failure along route.  */
				if (code == 204) {
					auto data = error["data"];
					eidx = std::size_t(double(
						data["erring_index"]
					));
					echan = Ln::Scid(std::string(
						data["erring_channel"]
					));
					edir = int(double(
						data["erring_direction"]
					));
					enode = Ln::NodeId(std::string(
						data["erring_node"]
					));
					fail = std::uint16_t(double(
						data["failcode"]
					));
				}
			} catch (std::exception const& ex) {
				return std::move(act)
				     + Boss::log( bus, Error
						, "FundsMover: Attempt: "
						  "Unexpected error from "
						  "%s: %s: %s"
						, e.command.c_str()
						, Util::stringify(e.error).c_str()
						, ex.what()
						);
			}

			if (code != 202 && code != 204)
				return std::move(act)
				     + Boss::log( bus, Info
						, "FundsMover: Attempt: "
						  "Unexpected error code "
						  "%d from %s, error: %s"
						, code
						, e.command.c_str()
						, Util::stringify(e.error)
							.c_str()
						);
			/* Unparsable onion with a 1-hop route means the
			 * source or destination node has massive issues,
			 * so cannot advance.  */
			if (code == 202 && route.size() <= 1)
				return std::move(act)
				     + Boss::log( bus, Info
						, "FundsMover: Attempt: "
						  "Unparsable onion, cannot "
						  "advance further."
						);

			/* feedback: action recorded into the persistent
			 * "clboss" askrene layer, so the subsequent
			 * getroute() retry routes around the failure.
			 * Empty if the failure mode means we should
			 * stop entirely.
			 */
			auto feedback = Ev::lift();

			if (code == 204) {
				act += Boss::log( bus, Debug
						, "FundsMover: code 204, "
						  "erring_index: %zu, "
						  "erring_channel: %s/%d, "
						  "erring_node: %s, "
						  "failcode: 0x%04x"
						, eidx
						, std::string(echan).c_str()
						, edir
						, std::string(enode).c_str()
						, int(fail)
						);

				if ( eidx == 0
				  || (eidx == 1 && (fail & 0x2000))
				   )
					return std::move(act)
					     + Boss::log( bus, Info
							, "FundsMover: "
							  "Failed at source, "
							  "cannot advance "
							  "further."
							)
					     ;
				if (eidx == route.size() + 1)
					return std::move(act)
					     + Boss::log( bus, Info
							, "FundsMover: "
							  "Failed at "
							  "destination, "
							  "cannot advance "
							  "further."
							)
					     ;
				/* 0x2000 == NODE level error.  */
				if ((fail & 0x2000))
					feedback = disable_node(enode);
				else
					/* The amount to pass to askrene-inform-channel
					 * is the HTLC amount attempted on the specific
					 * failing channel, not the downstream destination
					 * amount.  eidx indexes [hop0, route..., hoplast];
					 * we have already returned early for eidx == 0
					 * and eidx == route.size() + 1, so eidx is in
					 * [1, route.size()] here and route[eidx - 1]
					 * describes the failing channel.
					 */
					feedback = inform_channel_constrained(
						echan, edir,
						route[eidx - 1].amount_msat
					);
			} else {
				/* Unparsable onion (code 202): we cannot pin
				 * the failure to a specific channel.  The
				 * legacy excludes-vector code disabled a
				 * random middle hop locally for the remainder
				 * of the rebalance attempt, but doing the
				 * same via the persistent clboss askrene
				 * layer would blacklist a healthy random node
				 * across all future attempts and restarts
				 * after a single ambiguous failure.  We drop
				 * the write entirely and rely on askrene's
				 * natural route diversity to pick a different
				 * path on retry; the route.size() <= 1
				 * guard above already bails out when there
				 * is no diversity available.
				 */
				act += Boss::log( bus, Debug
						, "FundsMover: code 202, "
						  "unparsable onion; no per-"
						  "channel feedback, relying "
						  "on askrene route diversity "
						  "for retry."
						);
			}

			return std::move(act)
			     + std::move(feedback)
			     + getroute();
		});
	}
	/* Splice the us->source and destination->us hops with the
	 * source->destination middle hops we got from getroutes.
	 */
	Json::Out make_route() {
		auto ret = Json::Out();
		auto arr = ret.start_array();
		arr.entry(make_hop0());
		for (auto const& hop : route) {
			arr.start_object()
					.field("id", std::string(hop.id))
					.field("channel",
					       std::string(hop.scid))
					.field("direction", hop.direction)
					.field("amount_msat",
					       hop.amount_msat.to_msat())
					.field("delay", hop.delay)
					.field("style", "tlv")
				.end_object();
		}
		arr.entry(make_hoplast());
		arr.end_array();
		return ret;
	}
	Json::Out make_hop0() {
		/* Us->source hop.  */
		return Json::Out()
			.start_object()
				.field("id", std::string(source))
				.field("channel", std::string(first_scid))
				.field( "direction"
				      , self_id > source ? 1 : 0
				      )
				.field("amount_msat", source_amount.to_msat())
				.field("delay", source_delay)
				/* This used to be an explicit "style": "legacy",
				 * since we did not want to have to parse listnodes
				 * just to find out if the first peer supports
				 * "tlv".
				 * However, recent C-Lightning versions have
				 * dropped support for legacy completely, so at
				 * this point it is now safer to switch explicitly
				 * to "style": "tlv".
				 * 0.11.x C-Lightning has it optional and requires
				 * it to be "tlv" if specified, but some older
				 * versions have it optional and default to
				 * "legacy".
				 * Unfortunately, some new node software releases
				 * *do not support* "legacy" *at all*, so if our
				 * first peer runs such no-legacy-nope-nada
				 * software, dropping "style" may lead to our
				 * own node using "legacy"-by-default, which
				 * would cause funds movement to fail.
				 * So now we explicitly say "tlv" here.
				 */
				.field("style", "tlv")
			.end_object()
			;
	}
	Json::Out make_hoplast() {
		/* Destination->us hop.  */
		return Json::Out()
			.start_object()
				.field("id", std::string(self_id))
				.field("channel", std::string(last_scid))
				.field( "direction"
				      , destination > self_id ? 1 : 0
				      )
				.field("amount_msat", amount.to_msat())
				.field("delay", 14)
				/* We always support "tlv", at least for now... */
				.field("style", "tlv")
			.end_object()
			;
	}

	Ev::Io<void> delpay( std::shared_ptr<Sha256::Hash> payment_hash
			   , bool success
			   ) {
		return Ev::lift().then([this, payment_hash, success]() {
			auto status = std::string(
				success ? "complete" : "failed"
			);
			auto parms = Json::Out()
				.start_object()
					.field( "payment_hash"
					      , std::string(*payment_hash)
					      )
					.field("status", status)
				.end_object()
				;
			return rpc.command("delpay", std::move(parms));
			/* Do not care if it succeeds or fails or what
			 * it returns.  */
		}).then([](Jsmn::Object _) {
			return Ev::lift();
		}).catching<RpcError>([](RpcError const& _) {
			return Ev::lift();
		});
	}

};

Ev::Io<bool>
Attempter::run( S::Bus& bus
	      , Boss::Mod::Rpc& rpc
	      , Ln::NodeId self
	      /* The preimage should have been pre-arranged to be claimed.  */
	      , Ln::Preimage preimage
	      , Ln::Preimage payment_secret
	      , Ln::NodeId source
	      , Ln::NodeId destination
	      , Ln::Amount amount
	      , std::shared_ptr<Ln::Amount> fee_budget
	      , std::shared_ptr<Ln::Amount> remaining_amount
	      /* Details of the channel from destination to us.  */
	      , Ln::Scid last_scid
	      , Ln::Amount base_fee
	      , std::uint32_t proportional_fee
	      , std::uint32_t cltv_delta
	      /* The channel from us to source.  */
	      , Ln::Scid first_scid
	      ) {
	auto impl = std::make_shared<Impl>( bus
					  , rpc
					  , std::move(self)
					  , std::move(preimage)
					  , std::move(payment_secret)
					  , std::move(source)
					  , std::move(destination)
					  , amount
					  , std::move(fee_budget)
					  , std::move(remaining_amount)
					  , last_scid
					  , base_fee
					  , proportional_fee
					  , cltv_delta
					  , first_scid
					  );
	return impl->run();
}

}}}
