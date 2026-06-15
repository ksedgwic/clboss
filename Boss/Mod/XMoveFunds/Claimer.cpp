#include"Boss/Mod/XMoveFunds/Claimer.hpp"
#include"Boss/Msg/ProvideHtlcAcceptedDeferrer.hpp"
#include"Boss/Msg/ReleaseHtlcAccepted.hpp"
#include"Boss/Msg/SolicitHtlcAcceptedDeferrer.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/concurrent.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Ln/HtlcAccepted.hpp"
#include"S/Bus.hpp"

namespace {

/* Self-payment claim entries are kept for 24 hours.  Anything
 * older than that almost certainly belongs to a stuck or
 * abandoned request and is safe to drop -- the in-flight HTLCs
 * will time out via the on-chain mechanism. */
auto const timeout = double(3600 * 24);

}

namespace Boss { namespace Mod { namespace XMoveFunds {

void Claimer::start() {
	bus.subscribe<Msg::SolicitHtlcAcceptedDeferrer
		     >([this](Msg::SolicitHtlcAcceptedDeferrer const&) {
		auto f = [this](Ln::HtlcAccepted::Request const& r) {
			auto const& h = r.payment_hash;
			auto it = entries.find(h);
			if (it == entries.end())
				return Ev::lift(false);
			auto const& payment_secret =
			    it->second.payment_secret;

			if (r.payment_secret != payment_secret)
				return Ev::lift(false);

			/* Extract data.  Note: we COPY the preimage and
			 * do NOT erase the entry, unlike FundsMover's
			 * Claimer.  Each xrebalance self-payment can
			 * involve multiple MPP parts arriving at us
			 * with the same payment_hash + payment_secret;
			 * we want every part to settle, not just the
			 * first.  For self-rebalancing, partial
			 * delivery is still a win -- every msat that
			 * settles is real rebalance value.  Stale
			 * entries are pruned by the periodic timer
			 * below on the 24h cadence; in practice every
			 * MPP arrives well inside that window. */
			auto id = r.id;
			auto preimage = it->second.preimage;

			/* Prepare background action.  */
			auto act = bus.raise(Msg::ReleaseHtlcAccepted{
				Ln::HtlcAccepted::Response::resolve(
					id, std::move(preimage)
				)
			});

			/* Launch background action and return.  */
			return Boss::concurrent(act).then([]() {
				return Ev::lift(true);
			});
		};
		return bus.raise(Msg::ProvideHtlcAcceptedDeferrer{
			std::move(f)
		});
	});

	bus.subscribe<Msg::TimerRandomHourly
		     >([this](Msg::TimerRandomHourly const&) {
		auto now = Ev::now();
		/* Scan all entries and erase those that have gone
		 * past timeout.  */
		for ( auto it = entries.begin(), next = entries.begin()
		    ; it != entries.end()
		    ; it = next
		    ) {
			/* Save next entry.  */
			next = it;
			++next;

			if (it->second.timeout < now)
				entries.erase(it);
		}

		return Ev::lift();
	});
}

std::pair<Ln::Preimage, Ln::Preimage> Claimer::generate() {
	auto pre = Ln::Preimage(rand);
	auto sec = Ln::Preimage(rand);

	auto h = pre.sha256();
	auto& entry = entries[h];
	entry.timeout = Ev::now() + timeout;
	entry.preimage = pre;
	entry.payment_secret = sec;

	return std::make_pair(std::move(pre), std::move(sec));
}

}}}
