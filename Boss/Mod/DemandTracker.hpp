#ifndef BOSS_MOD_DEMANDTRACKER_HPP
#define BOSS_MOD_DEMANDTRACKER_HPP

namespace Ev { template<typename a> class Io; }
namespace Ln { namespace HtlcAccepted { struct Request; }}
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::DemandTracker
 *
 * @brief Observes forwards through an `htlc_accepted` deferrer
 * and raises `Boss::Msg::DemandObserved` naming the outgoing
 * channel of each one.
 *
 * Never holds an HTLC: the deferrer always declines immediately,
 * so forwarding latency is unaffected.  Consumers decide whether
 * an observation warrants action (`Boss::Mod::XRebalancer`'s
 * demand cycles).
 */
class DemandTracker {
private:
	S::Bus& bus;

	void start();
	Ev::Io<bool> htlc_accepted(Ln::HtlcAccepted::Request const& req);

public:
	DemandTracker() =delete;
	DemandTracker(DemandTracker&&) =delete;
	DemandTracker(DemandTracker const&) =delete;

	explicit
	DemandTracker(S::Bus& bus_) : bus(bus_) { start(); }
};

}}

#endif /* !defined(BOSS_MOD_DEMANDTRACKER_HPP) */
