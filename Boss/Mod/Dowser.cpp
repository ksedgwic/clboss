#include"Boss/Mod/Dowser.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/RequestDowser.hpp"
#include"Boss/Msg/ResponseDowser.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/CommandId.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<memory>
#include<string>

namespace {

Ev::Io<void> wait_for_rpc(Boss::Mod::Rpc*& rpc) {
	return Ev::yield().then([&rpc]() {
		if (rpc)
			return Ev::lift();
		return wait_for_rpc(rpc);
	});
}

/* Probe amount used to ask askrene whether the destination has at
 * least this much usable flow available from the source.  Chosen
 * comfortably above clboss-min-channel's 500_000 sat floor so the
 * Janitor's threshold check has margin.
 */
auto const probe_amount = Ln::Amount::sat(1000000);

/* Maximum number of paths askrene may use to split the probe across.
 * Mirrors the iteration count of the previous loop-and-exclude
 * algorithm so an aggregate estimate similar in magnitude is plausible
 * on well-connected networks.
 */
auto const probe_maxparts = std::uint32_t(10);

/* Maximum fee tolerance for the probe.  We are not actually paying,
 * but askrene refuses routes whose total cost exceeds this, so set
 * it generously to avoid spurious 206 errors masking real route
 * availability.
 */
auto const probe_maxfee = Ln::Amount::sat(5000);

/* Final-cltv to pass to askrene.  Arbitrary low value; we are
 * probing, not paying.
 */
auto const probe_final_cltv = std::uint32_t(14);

/* Reserve factor applied to the dowsed delivered amount.  Preserved
 * from the legacy algorithm: 1% channel reserve plus 0.5% default
 * `maxfeepercent` headroom.
 */
auto const reserve_factor = double(0.985);

}

namespace Boss { namespace Mod {

class Dowser::Run : public std::enable_shared_from_this<Run> {
private:
	S::Bus& bus;
	void* requester;
	Ln::NodeId fromid;
	Ln::NodeId toid;

	Boss::Mod::Rpc* rpc;

	Ln::Amount amount;

public:
	Run( S::Bus& bus_
	   , void* requester_
	   , Ln::NodeId const& fromid_
	   , Ln::NodeId const& toid_
	   ) : bus(bus_)
	     , requester(requester_)
	     , fromid(fromid_)
	     , toid(toid_)
	     , amount(Ln::Amount::sat(0))
	     { }

	Ev::Io<void> run(Boss::Mod::Rpc& rpc_) {
		rpc = &rpc_;
		auto self = shared_from_this();
		return Ev::lift().then([self]() {
			return self->probe();
		}).then([self]() {
			return self->bus.raise(Msg::ResponseDowser{
				self->requester, self->amount
			});
		});
	}

private:
	/* Ask askrene whether `probe_amount` can flow from fromid to
	 * toid; on success, the response's `routes` collectively deliver
	 * that amount and we report it (less `reserve_factor`) as the
	 * dowsed capacity.  Replaces the legacy 10-iteration
	 * getroute+listchannels loop -- askrene's min-cost-flow solver
	 * does multi-path enumeration natively (CLN >= v24.08, getroutes).
	 */
	Ev::Io<void> probe() {
		auto params = Json::Out()
			.start_object()
				.field("source", std::string(fromid))
				.field("destination", std::string(toid))
				.field("amount_msat", probe_amount.to_msat())
				.start_array("layers")
					.entry("auto.localchans")
					.entry("auto.sourcefree")
				.end_array()
				.field("maxfee_msat", probe_maxfee.to_msat())
				.field("final_cltv", probe_final_cltv)
				.field("maxparts", probe_maxparts)
			.end_object()
			;
		return rpc->command( "getroutes"
				   , std::move(params)
				   ).then([this](Jsmn::Object res) {
			auto delivered = Ln::Amount::sat(0);
			if (res.is_object() && res.has("routes")) {
				auto routes = res["routes"];
				if (routes.is_array()) {
					for (auto r : routes) {
						if ( !r.is_object()
						  || !r.has("amount_msat")
						   )
							continue;
						auto amt_j = r["amount_msat"];
						if (!Ln::Amount::valid_object(amt_j))
							continue;
						delivered += Ln::Amount::object(amt_j);
					}
				}
			}
			amount = delivered * reserve_factor;
			return Ev::lift();
		}).catching<RpcError>([this](RpcError const& _) {
			/* getroutes errors -- including 205 "Unable to
			 * find a route" and 206 "Route too expensive" --
			 * are the askrene way of saying "no flow"; map
			 * them all to 0msat.
			 */
			amount = Ln::Amount::sat(0);
			return Ev::lift();
		});
	}
};

class Dowser::CommandImpl {
private:
	S::Bus& bus;
	ModG::ReqResp< Msg::RequestDowser
		     , Msg::ResponseDowser
		     > dowse_rr;

	void start() {
		/* clboss-dowser command.  */
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return bus.raise(Msg::ManifestCommand{
				"clboss-dowser", "fromid toid",
				"Execute the dowsing algorithm to "
				"estimate the useful capacity between "
				"two nodes.",
				false
			});
		});
		bus.subscribe<Msg::CommandRequest
			     >([this](Msg::CommandRequest const& m) {
			if (m.command != "clboss-dowser")
				return Ev::lift();
			return run_command(m.params, m.id);
		});
	}

	Ev::Io<void> run_command(Jsmn::Object params, Ln::CommandId id) {
		auto param_fail = [this, id]() {
			return bus.raise(Msg::CommandFail{
				id, -32602, "Parameter error",
				Json::Out::empty_object()
			});
		};
		auto fromid = Ln::NodeId();
		auto toid = Ln::NodeId();

		try {
			if (params.size() != 2)
				return param_fail();
			auto fromid_j = Jsmn::Object();
			auto toid_j = Jsmn::Object();
			if (params.is_object()) {
				fromid_j = params["fromid"];
				toid_j = params["toid"];
			} else if (params.is_array()) {
				fromid_j = params[0];
				toid_j = params[1];
			} else
				return param_fail();
			fromid = Ln::NodeId(std::string(fromid_j));
			toid = Ln::NodeId(std::string(toid_j));
		} catch (std::exception const&) {
			return param_fail();
		}

		return dowse_rr.execute(Msg::RequestDowser{
			nullptr, fromid, toid
		}).then([this, id](Msg::ResponseDowser r) {
			auto rsp = Json::Out()
				.start_object()
					.field("amount", std::string(r.amount))
				.end_object()
				;
			return bus.raise(Msg::CommandResponse{
				id, std::move(rsp)
			});
		});
	}

public:
	CommandImpl( S::Bus& bus_
		   ) : bus(bus_)
		     , dowse_rr(bus_)
		     { start(); }
};

void Dowser::start() {
	bus.subscribe<Msg::Init
		     >([this](Msg::Init const& init) {
		rpc = &init.rpc;
		return Ev::lift();
	});
	bus.subscribe<Msg::RequestDowser
		     >([this](Msg::RequestDowser const& r) {
		auto run = std::make_shared<Run>( bus, r.requester
						, r.fromid, r.toid
						);
		return Ev::lift().then([this]() {
			return wait_for_rpc(rpc);
		}).then([this, run]() {
			return Boss::concurrent(run->run(*rpc));
		});
	});

	cmdimpl = Util::make_unique<CommandImpl>(bus);
}

Dowser::~Dowser() =default;
Dowser::Dowser(S::Bus& bus_
	      ) : bus(bus_)
		, rpc(nullptr)
		{ start(); }

}}
