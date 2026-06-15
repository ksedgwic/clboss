#ifndef BOSS_MOD_XMOVEFUNDS_CLAIMER_HPP
#define BOSS_MOD_XMOVEFUNDS_CLAIMER_HPP

#include"Ln/Preimage.hpp"
#include"Secp256k1/Random.hpp"
#include"Sha256/Hash.hpp"
#include<unordered_map>

namespace S { class Bus; }

namespace Boss { namespace Mod { namespace XMoveFunds {

/** class Boss::Mod::XMoveFunds::Claimer
 *
 * @brief claims incoming HTLCs at the destination of an xmovefunds
 * circular self-payment.
 *
 * Mirrors Boss::Mod::FundsMover::Claimer.  The two implementations
 * coexist because each subsystem manages its own pool of pending
 * preimages; the bus-level HtlcAcceptedDeferrer mechanism polls
 * every deferrer in turn until one claims the HTLC, so both can
 * run side by side.
 */
class Claimer {
private:
	S::Bus& bus;

	Secp256k1::Random rand;

	struct Entry {
		double timeout;
		Ln::Preimage preimage;
		Ln::Preimage payment_secret;
	};
	std::unordered_map<Sha256::Hash, Entry> entries;

	void start();

public:
	Claimer() =delete;
	Claimer(Claimer&&) =delete;
	Claimer(Claimer const&) =delete;

	explicit
	Claimer(S::Bus& bus_) : bus(bus_) { start(); }

	/* Generate a fresh preimage and payment_secret for one xmovefunds
	 * circular self-payment, register them in the claim table so the
	 * arriving HTLCs are auto-resolved, and return them to the caller
	 * for use in the sendpay parameters. */
	std::pair<Ln::Preimage, Ln::Preimage> generate();
};

}}}

#endif /* !defined(BOSS_MOD_XMOVEFUNDS_CLAIMER_HPP) */
