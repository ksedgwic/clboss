#include"Boss/Mod/FundsMover/Claimer.hpp"
#include"Boss/Mod/FundsMover/Main.hpp"
#include"Boss/Mod/FundsMover/Runner.hpp"
#include"Boss/Mod/FundsMover/create_label.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/RebalanceUnmanagerProxy.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ProvideDeletablePaymentLabelFilter.hpp"
#include"Boss/Msg/RequestGetCircularRebalanceFlag.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseGetCircularRebalanceFlag.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/Msg/SolicitDeletablePaymentLabelFilter.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
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
	Msg::ResponseGetCircularRebalanceFlag resp_circular_rebalance_flag;

	ModG::ReqResp< Msg::RequestGetCircularRebalanceFlag
		     , Msg::ResponseGetCircularRebalanceFlag
		     > get_circular_rebalance_rr;

	Boss::ModG::RebalanceUnmanagerProxy unmanager;

	void start() {
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			rpc = &init.rpc;
			self_id = init.self_id;
			return Ev::lift();
		});
		bus.subscribe<Msg::RequestMoveFunds
			     >([this](Msg::RequestMoveFunds const& m) {
			return get_circular_rebalance_rr.execute(
					Msg::RequestGetCircularRebalanceFlag{
				nullptr
			}).then([this, m](Msg::ResponseGetCircularRebalanceFlag res) {
				resp_circular_rebalance_flag = res;
				auto msg = std::make_shared<Msg::RequestMoveFunds>(m);
				return wait_for_rpc().then([this]() {
					return unmanager.get_unmanaged();
				}).then([this, m, msg](std::set<Ln::NodeId> const* unmanaged) {
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
					if (!resp_circular_rebalance_flag.state)
						return Boss::log( bus, Info
								, "FundsMover: %s."
								, resp_circular_rebalance_flag.comment.c_str()
								)
							+ bus.raise(Msg::ResponseMoveFunds{
								m.requester, Ln::Amount(), Ln::Amount()
							  })
							;
					auto runner = Runner::create( bus
								    , *rpc
								    , self_id
								    , claimer
								    , *msg
								    );
					return Runner::start(runner);
				});
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
	Ev::Io<void> wait_for_rpc() {
		return Ev::lift().then([this]() {
			if (!rpc)
				return Ev::yield() + wait_for_rpc();
			return Ev::lift();
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
			   , get_circular_rebalance_rr(bus_)
			   , unmanager(bus_)
			   { start(); }
};

Main::Main(Main&&) =default;
Main::~Main() =default;

Main::Main(S::Bus& bus) : pimpl(Util::make_unique<Impl>(bus)) { }

}}}
