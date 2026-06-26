#ifndef BOSS_MOD_FUNDSMOVER_ATTEMPTER_HPP
#define BOSS_MOD_FUNDSMOVER_ATTEMPTER_HPP

#include<cstdint>
#include<memory>
#include<string>

namespace Boss { namespace Mod { class Rpc; }}
namespace Ev { template<typename a> class Io; }
namespace Ln { class Amount; }
namespace Ln { class NodeId; }
namespace Ln { class Preimage; }
namespace Ln { class Scid; }
namespace S { class Bus; }

namespace Boss { namespace Mod { namespace FundsMover {

/** class Boss::Mod::FundsMover::Attempter
 *
 * @brief Makes an attempt to move a specific amount of funds
 * from one channel to another.
 *
 * @desc This object is dynamically created during runtime.
 */
class Attempter {
private:
	class Impl;
	std::shared_ptr<Impl> pimpl;

	Attempter() =default;

public:
	/* Return true if succeed, false if fail.  */
	static
	Ev::Io<bool>
	run( S::Bus& bus
	   , Boss::Mod::Rpc& rpc
	   , Ln::NodeId self
	   /* The preimage+payment_secret should have been pre-arranged to
	    * be claimed.  */
	   , Ln::Preimage preimage
	   , Ln::Preimage payment_secret
	   , Ln::NodeId source
	   , Ln::NodeId destination
	   , Ln::Amount amount
	   /* Budgeting information.
	    * Attempter will deduct from this fee budget when it sends out an
	    * attempt, and will return the deducted fee budget if an attempt
	    * fails.
	    */
	   , std::shared_ptr<Ln::Amount> fee_budget
	   /* Remaining amount that still needs to be sent out.
	    * Attempter will deduct from this remaining amount when it sends
	    * out an attempt, and will return the deducted amount if an
	    * attempt fails.
	    * This is used to rescale the fee limit of this attempt when
	    * this attempt is less than the remaining amount to send.
	    */
	   , std::shared_ptr<Ln::Amount> remaining_amount
	   /* Details of the channel from destination to us.  */
	   , Ln::Scid last_scid
	   , Ln::Amount base_fee
	   , std::uint32_t proportional_fee
	   , std::uint32_t cltv_delta
	   /* The channel from us to source.  */
	   , Ln::Scid first_scid
	   /* Snapshot of the Runner's original budget and original amount
	    * at the time the Attempter is constructed.  Used together to
	    * compute the per-Attempter absolute fee cap as
	    * orig_budget * amount / orig_amount, which the Attempter
	    * enforces alongside the existing prorata cap during the budget
	    * check.  The pair is the rate (msat per msat) the Runner was
	    * authorised to spend at; scaling to this Attempter's amount
	    * gives the maximum fee we may pay regardless of how the pool
	    * has drifted from sibling activity.  Snapshotted by value (not
	    * shared) because these are set once at Runner construction and
	    * never change.
	    */
	   , Ln::Amount orig_budget
	   , Ln::Amount orig_amount
	   /* Minimum askrene route success probability (ppm) below which a
	    * found route is not sent: the attempt fails and Runner splits to
	    * a smaller, more-probable amount.  0 disables.  Snapshot of
	    * clboss-min-rebalance-prob-ppm. */
	   , std::uint64_t min_prob_ppm
	   );
};

}}}

#endif /* !defined(BOSS_MOD_FUNDSMOVER_ATTEMPTER_HPP) */
