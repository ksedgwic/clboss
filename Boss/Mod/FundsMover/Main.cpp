#include"Boss/Mod/FundsMover/Claimer.hpp"
#include"Boss/Mod/FundsMover/Main.hpp"
#include"Boss/Mod/FundsMover/Runner.hpp"
#include"Boss/Mod/FundsMover/create_label.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/RebalanceUnmanagerProxy.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ProvideDeletablePaymentLabelFilter.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/SolicitDeletablePaymentLabelFilter.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"

#if HAVE_CONFIG_H
# include"config.h"
#endif

namespace Boss { namespace Mod { namespace FundsMover {

class Main::Impl {
private:
	S::Bus& bus;
	Claimer claimer;
	Boss::Mod::Rpc* rpc;
	Ln::NodeId self_id;
	/* True once create_clboss_layer() has resolved (either by
	 * successfully creating/finding the persistent layer, or by
	 * logging a non-fatal RpcError on older CLN).  Gated on by
	 * wait_for_ready() so that Msg::RequestMoveFunds handling
	 * never tries to use the layer before askrene is told about
	 * it.
	 */
	bool layer_ready;

	Boss::ModG::RebalanceUnmanagerProxy unmanager;

	void start() {
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			rpc = &init.rpc;
			self_id = init.self_id;
			return Boss::concurrent(create_clboss_layer());
		});
		bus.subscribe<Msg::RequestMoveFunds
			     >([this](Msg::RequestMoveFunds const& m) {
			auto msg = std::make_shared<Msg::RequestMoveFunds>(m);
			return wait_for_ready().then([this]() {
				return unmanager.get_unmanaged();
			}).then([this, msg](std::set<Ln::NodeId> const* unmanaged) {
				auto un_s = (unmanaged->count(msg->source) != 0);
				auto un_d = (unmanaged->count(msg->destination) != 0);
				if (un_s || un_d) {
					char const* tpl = nullptr;
					if (un_s && un_d) {
						tpl = "%1$sfrom an unmanaged node %2$s "
						      "to an unmanaged node %3$s%4$s"
						      ;
					} else if (un_s) {
						tpl = "%1$sfrom an unmanaged node %2$s"
						      "%4$s"
						      ;
					} else {
						tpl = "%1$s"
						      "to an unmanaged node %3$s%4$s"
						      ;
					}
					return Boss::log( bus, Error
							, tpl
							, "FundsMover: *SOMETHING* is "
							  "attempting to move funds "
							, Util::stringify(msg->source)
								.c_str()
							, Util::stringify(msg->destination)
								.c_str()
							, ", this may be a bug, "
							  "refusing to move.  "
							  "Contact " PACKAGE_BUGREPORT
							);
				}
				auto runner = Runner::create( bus
							    , *rpc
							    , self_id
							    , claimer
							    , *msg
							    );
				return Runner::start(runner);
			});
		});
		using Msg::ProvideDeletablePaymentLabelFilter;
		using Msg::SolicitDeletablePaymentLabelFilter;
		bus.subscribe<SolicitDeletablePaymentLabelFilter
			     >([this
			       ](SolicitDeletablePaymentLabelFilter const& _) {
			return bus.raise(ProvideDeletablePaymentLabelFilter{
					&is_our_label
			});
		});
	}
	/* Gate Msg::RequestMoveFunds handling on FundsMover's
	 * startup-time setup: rpc must have arrived via Msg::Init,
	 * and create_clboss_layer() must have completed (either
	 * successfully or via the logged-RpcError graceful-
	 * degradation path).  Without the layer_ready check there
	 * is a startup window where the first move can run before
	 * askrene-create-layer returns, and any subsequent
	 * askrene-inform-channel / askrene-disable-node writes
	 * would fail with "no such layer".
	 */
	Ev::Io<void> wait_for_ready() {
		return Ev::lift().then([this]() {
			if (!rpc || !layer_ready)
				return Ev::yield() + wait_for_ready();
			return Ev::lift();
		});
	}

	/* Ensure the persistent "clboss" askrene layer exists.  Called
	 * once at startup, fire-and-forget.  Idempotent: when persistent
	 * is true, askrene-create-layer succeeds even if the layer
	 * already exists.  Failures (e.g. CLN < v24.11 where the RPC
	 * does not exist) are logged but non-fatal -- subsequent
	 * getroutes calls will simply not benefit from the
	 * failure-learning layer.
	 */
	Ev::Io<void> create_clboss_layer() {
		return Ev::lift().then([this]() {
			auto parms = Json::Out()
				.start_object()
					.field("layer", "clboss")
					.field("persistent", true)
				.end_object()
				;
			return rpc->command( "askrene-create-layer"
					   , std::move(parms)
					   );
		}).then([this](Jsmn::Object _) {
			layer_ready = true;
			return Ev::lift();
		}).catching<RpcError>([this](RpcError const& e) {
			/* Mark ready even on failure: degraded mode (no
			 * persistent learning layer) must still allow
			 * rebalances to proceed, otherwise we'd livelock
			 * wait_for_ready() on CLN < v24.11.
			 */
			layer_ready = true;
			return Boss::log( bus, Error
					, "FundsMover: askrene-create-layer "
					  "(clboss) failed: %s; failure-"
					  "learning will not be available."
					, Util::stringify(e.error).c_str()
					);
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl(S::Bus& bus_) : bus(bus_)
			   , claimer(bus_)
			   , rpc(nullptr)
			   , layer_ready(false)
			   , unmanager(bus_)
			   { start(); }
};

Main::Main(Main&&) =default;
Main::~Main() =default;

Main::Main(S::Bus& bus) : pimpl(Util::make_unique<Impl>(bus)) { }

}}}
