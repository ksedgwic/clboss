#include"Boss/Mod/ChannelCandidateMatchmaker.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/AmountSettings.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/PatronizeChannelCandidate.hpp"
#include"Boss/Msg/PreinvestigateChannelCandidates.hpp"
#include"Boss/Msg/ProposeChannelCandidates.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<memory>
#include<sstream>

namespace Boss { namespace Mod {

/* This is effectively a single run of the matchmaker.  */
class ChannelCandidateMatchmaker::Run
		: public std::enable_shared_from_this<Run> {
private:
	S::Bus& bus;
	Boss::Mod::Rpc& rpc;

	Ln::NodeId proposal;
	std::queue<Ln::NodeId> guide;

	Ln::Amount min_channel;

	explicit
	Run( S::Bus& bus_
	   , Boss::Mod::Rpc& rpc_
	   , Ln::NodeId proposal_
	   , std::queue<Ln::NodeId> guide_
	   , Ln::Amount min_channel_
	   ) : bus(bus_)
	     , rpc(rpc_)
	     , proposal(std::move(proposal_))
	     , guide(std::move(guide_))
	     , min_channel(min_channel_)
	     { }

public:
	Run() =delete;
	Run(Run&&) =delete;
	Run(Run const&) =delete;

	static
	std::shared_ptr<Run>
	create( S::Bus& bus
	      , Boss::Mod::Rpc& rpc
	      , Ln::NodeId proposal
	      , std::queue<Ln::NodeId> guide
	      , Ln::Amount min_channel
	      ) {
		return std::shared_ptr<Run>(
			new Run( bus
			       , rpc
			       , std::move(proposal)
			       , std::move(guide)
			       , min_channel
			       )
		);
	}

	Ev::Io<void> run() {
		auto self = shared_from_this();
		return self->core_run().then([self]() {
			return Ev::lift();
		});
	}
private:
	Ev::Io<void> core_run() {
		return Ev::yield().then([this]() {
			if (guide.empty())
				/* Failed.  */
				return Boss::log( bus, Debug
						, "ChannelCandidateMatchmaker:"
						  " Could not find patron for "
						  "%s."
						, std::string(proposal
							     ).c_str()
						);

			return step();
		});
	}
	Ev::Io<void> step() {
		auto target = std::move(guide.front());
		guide.pop();

		auto probe_amount = 2.0 * min_channel;
		auto parms = Json::Out()
			.start_object()
				.field("source", std::string(proposal))
				.field("destination", std::string(target))
				/* 2x min_channel because the dowser will
				 * halve the channel capacity of the first
				 * hop.
				 */
				.field("amount_msat", probe_amount.to_msat())
				.start_array("layers")
					.entry("auto.localchans")
					.entry("auto.sourcefree")
				.end_array()
				/* Generous max-fee tolerance for what is a
				 * route-discovery probe, not an actual
				 * payment.
				 */
				.field("maxfee_msat",
				       (probe_amount * 0.01).to_msat())
				.field("final_cltv", 14)
				.field("maxparts", 1)
			.end_object()
			;
		return rpc.command("getroutes", std::move(parms)
				  ).then([this](Jsmn::Object res) {
			auto patron = Ln::NodeId();
			try {
				patron = Ln::NodeId(std::string(
					res["routes"][0]["path"][0]["node_id_out"]
				));
			} catch (Jsmn::TypeError const&) {
				auto os = std::ostringstream();
				os << res;
				return Boss::log( bus, Error
						, "ChannelCandidateMatchmaker:"
						  " Unexpected result from "
						  "getroutes: %s"
						, os.str().c_str()
						).then([]()
							-> Ev::Io<void>{
					throw RpcError( "getroutes"
						      , Jsmn::Object()
						      );
				});
			}
			auto act = Ev::lift();
			act += Boss::log( bus, Debug
					, "ChannelCandidateMatchmaker: "
					  "Matched proposal %s to patron %s."
					, std::string(proposal).c_str()
					, std::string(patron).c_str()
					);
			auto propose = Msg::ProposeChannelCandidates{
				std::move(proposal), std::move(patron)
			};
			auto preinv = Msg::PreinvestigateChannelCandidates{
				{std::move(propose)},
				1
			};
			act += bus.raise(std::move(preinv));
			return act;
		}).catching<RpcError>([this](RpcError const&) {
			/* Try next.  */
			return core_run();
		});
	}
};

void ChannelCandidateMatchmaker::start() {
	bus.subscribe<Msg::AmountSettings
		     >([this](Msg::AmountSettings const& r) {
		min_channel = r.min_channel;
		return Ev::lift();
	});
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		return Ev::lift();
	});
	bus.subscribe<Msg::PatronizeChannelCandidate
		     >([this](Msg::PatronizeChannelCandidate const& m) {
		if (!rpc)
			return Ev::lift();

		/* Construct queue.  */
		auto q = std::queue<Ln::NodeId>();
		for (auto const& n : m.guide)
			q.push(n);
		/* Create run object.  */
		auto run = Run::create( bus, *rpc, m.proposal, std::move(q)
				      , min_channel
				      );
		return Boss::concurrent(run->run());
	});
}

}}
