#include"Boss/Mod/DemandTracker.hpp"
#include"Boss/Msg/DemandObserved.hpp"
#include"Boss/Msg/ProvideHtlcAcceptedDeferrer.hpp"
#include"Boss/Msg/SolicitHtlcAcceptedDeferrer.hpp"
#include"Ev/Io.hpp"
#include"Ln/HtlcAccepted.hpp"
#include"S/Bus.hpp"

namespace Boss { namespace Mod {

void DemandTracker::start() {
	bus.subscribe<Msg::SolicitHtlcAcceptedDeferrer
		     >([this](Msg::SolicitHtlcAcceptedDeferrer const&) {
		auto f = [this](Ln::HtlcAccepted::Request const& req) {
			return htlc_accepted(req);
		};
		return bus.raise(Msg::ProvideHtlcAcceptedDeferrer{
			std::move(f)
		});
	});
}

Ev::Io<bool>
DemandTracker::htlc_accepted(Ln::HtlcAccepted::Request const& req) {
	/* Not a forward (we are the recipient)?  */
	if (!req.next_channel)
		return Ev::lift(false);
	/* This runs in the hook path: raise (subscribers keep their
	 * synchronous part cheap) and decline the HTLC either way.  */
	auto msg = Msg::DemandObserved{req.next_channel};
	return bus.raise(std::move(msg)).then([]() {
		return Ev::lift(false);
	});
}

}}
