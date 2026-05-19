#include"Boss/Mod/ActiveProber.hpp"
#include"Boss/Mod/ChannelCandidateInvestigator/Main.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ProbeActively.hpp"
#include"Boss/Msg/ProvideDeletablePaymentLabelFilter.hpp"
#include"Boss/Msg/SolicitDeletablePaymentLabelFilter.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Preimage.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Sha256/Hash.hpp"
#include"Util/Str.hpp"
#include"Util/stringify.hpp"
#include<algorithm>
#include<memory>
#include<queue>
#include<set>
#include<vector>

namespace {

/* Try to probe the following amount.  */
auto const reference_amount = Ln::Amount::sat(160000);

Ev::Io<void> wait_for_rpc(Boss::Mod::Rpc*& rpc) {
	return Ev::yield().then([&rpc]() {
		if (!rpc)
			return wait_for_rpc(rpc);
		return Ev::lift();
	});
}

/* Prefix for all labels.  */
auto const label_prefix = std::string( "CLBOSS ActiveProber "
				       "payment, this will fail "
				       "and should automatically "
				       "get deleted. Hash: "
				     );

bool is_our_label(std::string const& label) {
	if (label.length() != label_prefix.length() + 64)
		return false;
	if ( std::string(label.begin(), label.begin() + label_prefix.length())
	  != label_prefix
	   )
		return false;
	auto hash = std::string( label.begin() + label_prefix.length()
			       , label.end()
			       );
	return Util::Str::ishex(hash);
}

}

namespace Boss { namespace Mod {

class ActiveProber::Run : public std::enable_shared_from_this<Run> {
private:
	S::Bus& bus;
	ChannelCandidateInvestigator::Main& investigator;
	Rpc& rpc;
	Ln::NodeId self_id;

	Ln::NodeId peer;

	Secp256k1::Random& random;

	Run( ActiveProber& prober
	   , Ln::NodeId const& peer_
	   ) : bus(prober.bus)
	     , investigator(prober.investigator)
	     , rpc(*prober.rpc)
	     , self_id(prober.self_id)
	     , peer(peer_)
	     , random(prober.random)
	     { }

public:
	static
	std::shared_ptr<Run>
	create( ActiveProber& prober
	      , Ln::NodeId const& peer
	      ) {
		return std::shared_ptr<Run>(new Run(prober, peer));
	}

	Ev::Io<void> run() {
		auto self = shared_from_this();
		return self->core_run().then([self]() {
			return Ev::lift();
		});
	}
private:
	/* First channel.  */
	Ln::Scid chan0;
	Ln::Amount cap0;
	Ln::Amount amount;

	Ev::Io<void> core_run() {
		return Ev::lift().then([this]() {
			return Boss::log( bus, Info
					, "ActiveProber: Probe peer %s."
					, std::string(peer).c_str()
					);
		}).then([this]() {
			auto parms = Json::Out()
				.start_object()
					.field("id", std::string(peer))
				.end_object()
				;
			return rpc.command("listpeerchannels", std::move(parms));
		}).then([this](Jsmn::Object res) {
			try {
				auto cs = res["channels"];
				for (auto c : cs) {
					if (!c.has("short_channel_id"))
						continue;
					if (!c.has("spendable_msat"))
						continue;
					auto state = std::string(
						c["state"]
					);
					if (state != "CHANNELD_NORMAL")
						continue;

					chan0 = Ln::Scid(std::string(
						c["short_channel_id"]
					));
					cap0 = Ln::Amount::object(
						c["spendable_msat"]
					);
					break;
				}
			} catch (Jsmn::TypeError const& _) {
				return Boss::log( bus, Error
						, "ActiveProber: unexpected "
						  "listpeerchannels result: %s"
						, Util::stringify(res).c_str()
						);
			}

			if (!chan0)
				return Boss::log( bus, Info
						, "ActiveProber: No "
						  "CHANNELD_NORMAL channel "
						  "with node %s, cannot probe."
						, std::string(peer).c_str()
						);

			amount = reference_amount;
			if (amount > cap0 * 0.95)
				amount = cap0 * 0.95;

			return get_destinations();
		});
	}

	/* A candidate set of destinations.  */
	std::queue<Ln::NodeId> to_try;

	Ev::Io<void> get_destinations() {
		return Ev::lift().then([this]() {
			return investigator.get_channel_candidates();
		}).then([this](std::vector<std::pair< Ln::NodeId
						    , Ln::NodeId
						    >> cands) {
			/* Put both proposal and patron into the
			 * set of candidate nodes.  */
			auto n_set = std::set<Ln::NodeId>();
			for (auto& e : cands) {
				n_set.insert(std::move(e.first));
				n_set.insert(std::move(e.second));
			}
			/* If the peer itself is in the set, remove it.  */
			auto it = n_set.find(peer);
			if (it != n_set.end())
				n_set.erase(it);
			/* Copy to a vector and shuffle.  */
			auto n_vec = std::vector<Ln::NodeId>(n_set.size());
			std::copy( n_set.begin(), n_set.end()
				 , n_vec.begin()
				 );
			std::shuffle( n_vec.begin(), n_vec.end()
				    , Boss::random_engine
				    );
			/* Push to queue.  */
			for (auto& n : n_vec)
				to_try.push(std::move(n));

			if (to_try.empty())
				return Boss::log( bus, Info
						, "ActiveProber: No trial "
						  "destinations found "
						  "for peer %s."
						, std::string(peer).c_str()
						);

			return Boss::log( bus, Debug
					, "ActiveProber: Found %zu trial "
					  "destinations for peer %s."
					, to_try.size()
					, std::string(peer).c_str()
					)
			     + getroute()
			     ;
		});
	}

	/* First hop after `peer`: the peer's neighbor that we probe
	 * towards.  Extracted into typed values from getroutes' path[0]
	 * so we can rebuild the sendpay hop later without keeping the
	 * raw Jsmn::Object around.
	 */
	Ln::NodeId id1;
	Ln::Scid chan1;
	std::uint32_t direction1;
	Ln::Amount amount1;
	std::uint32_t delay1;

	Ev::Io<void> getroute() {
		if (to_try.empty())
			return Boss::log( bus, Info
					, "ActiveProber: No more trial "
					  "destinations for peer %s."
					, std::string(peer).c_str()
					);
		return Ev::yield().then([this]() {
			auto const& dest = to_try.front();

			auto parms = Json::Out()
				.start_object()
					.field("source", std::string(peer))
					.field("destination", std::string(dest))
					.field("amount_msat", amount.to_msat())
					/* No layers: source is a remote node
					 * (peer), not us, so the
					 * auto.localchans / auto.sourcefree
					 * helpers do not apply -- they would
					 * inject our private local channels
					 * and zero out the source's outgoing
					 * fees, either of which could make
					 * askrene pick a path[0] that the
					 * peer cannot actually reach via
					 * public topology (and could even
					 * produce a route that loops back
					 * through us).
					 */
					.start_array("layers").end_array()
					/* Generous max-fee tolerance for a
					 * probe; askrene optimizes for
					 * cheaper paths anyway via its
					 * probability scoring.
					 */
					.field("maxfee_msat",
					       (amount * 0.01).to_msat())
					.field("final_cltv", 14)
					.field("maxparts", 1)
				.end_object()
				;
			return rpc.command("getroutes", std::move(parms));
		}).then([this](Jsmn::Object res) {
			try {
				/* getroutes path[] hop fields were renamed
				 * in CLN v26.06.  Which names actually get
				 * emitted depends on the CLN version AND
				 * whether the node runs with developer
				 * mode (which suppresses deprecated
				 * outputs):
				 *
				 *   v26.04                  -> old names only
				 *   v26.06+ no developer    -> both names emitted
				 *   v26.06+ developer=true  -> new names only
				 *
				 * Bridge by preferring the new name, falling
				 * back to the old.  TODO: drop the fallback
				 * once CLN v26.04 is no longer supported and
				 * the old names are gone for good in v27.06.
				 *
				 * short_channel_id_dir is older (v24.11)
				 * and is emitted unconditionally.
				 */
				auto path0 = res["routes"][0]["path"][0];
				id1 = Ln::NodeId(std::string(
					path0.has("node_id_out")
						? path0["node_id_out"]
						: path0["next_node_id"]
				));
				/* short_channel_id_dir is "SCID/dir"; split
				 * into the SCID and the direction.  If
				 * the slash is missing the response is
				 * malformed; treat it as a parse error
				 * so the enclosing handler logs and
				 * skips this destination rather than
				 * feeding garbage to Ln::Scid / std::stoul.
				 */
				auto sdir = std::string(
					path0["short_channel_id_dir"]
				);
				auto slash = sdir.find('/');
				if (slash == std::string::npos)
					throw Jsmn::TypeError();
				chan1 = Ln::Scid(sdir.substr(0, slash));
				direction1 = std::uint32_t(std::stoul(
					sdir.substr(slash + 1)
				));
				amount1 = Ln::Amount::object(
					path0.has("amount_out_msat")
						? path0["amount_out_msat"]
						: path0["amount_msat"]
				);
				delay1 = std::uint32_t(double(
					path0.has("cltv_out")
						? path0["cltv_out"]
						: path0["delay"]
				));
			} catch (std::exception const& _) {
				/* Broaden catch to std::exception so we also
				 * handle std::invalid_argument and
				 * std::out_of_range from std::stoul on a
				 * malformed short_channel_id_dir tail.  Pop
				 * the bad destination before logging so the
				 * recursive getroute() picks a different
				 * candidate instead of looping on the same
				 * one indefinitely.
				 */
				if (!to_try.empty())
					to_try.pop();
				return Boss::log( bus, Error
						, "ActiveProber: Unexpected "
						  "result from getroutes: %s"
						, Util::stringify(res).c_str()
						).then([]() {
					return Ev::lift(false);
				});
			}

			return Ev::lift(true);
		}).catching<RpcError>([this](RpcError const& e) {
			/* Go to next.  */
			to_try.pop();
			return Ev::lift(false);
		}).then([this](bool flag) {
			if (flag)
				return compute_hop0();
			else
				return getroute();
		});
	}

	/* Details from the first hop in the found route, which have to be
	 * added in the 0th hop we will insert.
	 */
	Ln::Amount base_fee;
	std::uint32_t proportional_fee;
	std::uint32_t cltv_delta;
	Ln::Amount amount0;
	std::uint32_t delay0;

	Ev::Io<void> compute_hop0() {
		return Ev::lift().then([this]() {
			/* Get information on chan1.  */
			auto parms = Json::Out()
				.start_object()
					.field( "short_channel_id"
					      , std::string(chan1)
					      )
				.end_object()
				;
			return rpc.command("listchannels", std::move(parms));
		}).then([this](Jsmn::Object res) {
			auto found = false;
			try {
				auto cs = res["channels"];
				for (auto c : cs) {
					auto src = Ln::NodeId(std::string(
						c["source"]
					));
					if (src != peer)
						continue;
					base_fee = Ln::Amount::msat(double(
						c["base_fee_millisatoshi"]
					));
					proportional_fee
						= std::uint32_t(double(
						c["fee_per_millionth"]
					));
					cltv_delta = std::uint32_t(double(
						c["delay"]
					));
					found = true;
				}
			} catch (Jsmn::TypeError const&) {
				return Boss::log( bus, Error
						, "ActiveProber: Unexpected "
						  "result from listchannels: "
						  "%s"
						, Util::stringify(res).c_str()
						);
			}

			/* Channel could have disappeared from under us.  */
			if (!found) {
				to_try.pop();
				return getroute();
			}

			amount0 = amount1 + base_fee
				+ (amount1 * ( double(proportional_fee)
					     / 1000000
					     ))
				/* Fudge roundoff erors.  */
				+ Ln::Amount::msat(1)
				;
			delay0 = delay1 + cltv_delta;

			return select_hash();
		});
	}

	Sha256::Hash hash;

	Ev::Io<void> select_hash() {
		/* Generate a random preimage and hash it.  */
		auto preimage = Ln::Preimage(random);
		hash = preimage.sha256();

		/* Flip the bits of the generated hash.
		 * This ensures that even if the entropy of our preimage
		 * is low, this is still not exploitable, as an attacker
		 * that knows every bit of our preimage still cannot
		 * reverse the inverse-hash of our preimage.
		 */
		std::uint8_t buf[32];
		hash.to_buffer(buf);
		for (auto i = std::size_t(0); i < 32; ++i)
			buf[i] = ~buf[i];
		hash.from_buffer(buf);

		return sendpay();
	}

	Ev::Io<void> sendpay() {
		return Ev::lift().then([this]() {
			auto os = std::ostringstream();
			os << chan0 << " " << std::string(chan1);
			return Boss::log( bus, Debug
					, "ActiveProber: Probe %s by route %s."
					, std::string(peer).c_str()
					, os.str().c_str()
					);
		}).then([this]() {
			auto routeparm = Json::Out();
			auto routearr = routeparm.start_array();
			/* Load the 0th hop.  */
			routearr.start_object()
					.field("id", std::string(peer))
					.field("channel", std::string(chan0))
					.field( "direction"
					      , self_id > peer ? 1 : 0
					      )
					.field( "amount_msat"
					      , amount0.to_msat()
					      )
					.field("delay", delay0)
					.field("style", "tlv")
				.end_object();
			/* Load the first hop after the peer, rebuilt from
			 * the getroutes path[0] we extracted earlier.
			 *
			 * We always probe with a short two-hop route (hop
			 * 0 above, and this hop 1).  This gives the peer
			 * the "benefit of the doubt": we only probe the
			 * peer and *its* direct peer for uptime and
			 * capacity.  Still "realistic" since the
			 * destinations were chosen as popular nodes (or
			 * at least to nodes that CLBOSS thinks are good
			 * to have capacity towards).
			 */
			routearr.start_object()
					.field("id", std::string(id1))
					.field("channel", std::string(chan1))
					.field("direction", direction1)
					.field("amount_msat", amount1.to_msat())
					.field("delay", delay1)
					.field("style", "tlv")
				.end_object();
			routearr.end_array();

			auto label = label_prefix + std::string(hash);

			auto parms = Json::Out()
				.start_object()
					.field("route", std::move(routeparm))
					.field( "payment_hash"
					      , std::string(hash)
					      )
					.field("label", label)
				.end_object()
				;
			return rpc.command("sendpay", std::move(parms));
		}).then([this](Jsmn::Object _) {

			auto parms = Json::Out()
				.start_object()
					.field( "payment_hash"
					      , std::string(hash)
					      )
				.end_object()
				;
			return rpc.command("waitsendpay", std::move(parms));
		}).then([](Jsmn::Object _) {

			/* Oh look, we succeeded.
			 * Should not happen though.
			 */
			return Ev::lift(true);
		}).catching<RpcError>([](RpcError const& _) {
			/* Oh no, we failed, as expected.  */
			return Ev::lift(false);
		}).then([this](bool success) {

			auto status = std::string(
				success ? "complete" : "failed"
			);
			/* Now delete the payment, so that the operator
			 * does not get confused with random failing
			 * payments they did not make.  */
			auto parms = Json::Out()
				.start_object()
					.field( "payment_hash"
					      , std::string(hash)
					      )
					.field( "status"
					      , status
					      )
				.end_object()
				;
			return rpc.command("delpay", std::move(parms));
		}).then([](Jsmn::Object _) {
			/* We do not actually care if the `delpay` succeeds
			 * or not.
			 */
			return Ev::lift();
		}).catching<RpcError>([](RpcError const& _) {
			return Ev::lift();
		}).then([this]() {
			return Boss::log( bus, Info
					, "ActiveProber: Finished probing "
					  "peer %s."
					, std::string(peer).c_str()
					);
		});
	}
};

void ActiveProber::start() {
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		self_id = init.self_id;
		return Ev::lift();
	});
	bus.subscribe<Msg::ProbeActively
		     >([this](Msg::ProbeActively const& m) {
		auto run = Run::create(*this, m.peer);
		return Boss::concurrent( wait_for_rpc(rpc)
				       + run->run()
				       );
	});

	using Msg::ProvideDeletablePaymentLabelFilter;
	using Msg::SolicitDeletablePaymentLabelFilter;
	bus.subscribe<SolicitDeletablePaymentLabelFilter
		     >([this](SolicitDeletablePaymentLabelFilter const& _) {
		return bus.raise(ProvideDeletablePaymentLabelFilter{
			&is_our_label
		});
	});
}

}}
