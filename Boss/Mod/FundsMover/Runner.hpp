#ifndef BOSS_MOD_FUNDSMOVER_RUNNER_HPP
#define BOSS_MOD_FUNDSMOVER_RUNNER_HPP

#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include<cstdint>
#include<memory>
#include<string>

namespace Boss { namespace Mod { namespace FundsMover { class Attempter; }}}
namespace Boss { namespace Mod { namespace FundsMover { class Claimer; }}}
namespace Boss { namespace Mod { class Rpc; }}
namespace Boss { namespace Msg { struct RequestMoveFunds; }}
namespace Ev { template<typename a> class Io; }
namespace S { class Bus; }

namespace Boss { namespace Mod { namespace FundsMover {

/** class Boss::Mod::FundsMover::Runner
 *
 * @brief Executes a run of the funds movement.
 */
class Runner {
private:
	S::Bus& bus;
	Boss::Mod::Rpc& rpc;
	Ln::NodeId self;
	Boss::Mod::FundsMover::Claimer& claimer;

	/* Specs from the request.  */
	void* requester;
	Ln::NodeId source;
	Ln::NodeId destination;
	Ln::Amount amount;
	/* Shared budget.  */
	std::shared_ptr<Ln::Amount> fee_budget;
	/* Remaining amount to send.  */
	std::shared_ptr<Ln::Amount> remaining_amount;
	/* The original budget.
	 * We determine how much budget was spent by the difference
	 * between orig_budget and fee_budget.
	 */
	Ln::Amount orig_budget;

	/* Time this run started.  */
	double start_time;

	Runner( S::Bus& bus
	      , Boss::Mod::Rpc& rpc
	      , Ln::NodeId self
	      , Boss::Mod::FundsMover::Claimer& claimer
	      , Boss::Msg::RequestMoveFunds const& req
	      );

	Ev::Io<void> core_run();

	/* Number of running attempts.  */
	std::size_t attempts;
	/* Total amount of funds already successfully moved
	 * to the destination.  */
	Ln::Amount transferred;

	Ev::Io<void> gather_info();
	Ev::Io<void> attempt(Ln::Amount amount);
	Ev::Io<void> finish();

	/* Information about last hop (destination->us).  */
	Ln::Scid last_scid;
	Ln::Amount base_fee;
	std::uint32_t proportional_fee;
	std::uint32_t cltv_delta;
	/* Information about first hop (us->source).  */
	Ln::Scid first_scid;

public:
	Runner() =delete;
	Runner(Runner&&) =delete;
	Runner(Runner const&) =delete;

	static
	std::shared_ptr<Runner>
	create( S::Bus& bus
	      , Boss::Mod::Rpc& rpc
	      , Ln::NodeId self
	      , Boss::Mod::FundsMover::Claimer& claimer
	      , Boss::Msg::RequestMoveFunds const& req
	      ) {
		/* Constructor is private, cannot use std::make_shared.  */
		return std::shared_ptr<Runner>(
			new Runner( bus
				  , rpc
				  , std::move(self)
				  , claimer
				  , req
				  )
		);
	}

	/* Resumes immediately: the run is executed in the background
	 * in a new greenthread.  */
	static
	Ev::Io<void> start(std::shared_ptr<Runner> const& self);
};

}}}

#endif /* !defined(BOSS_MOD_FUNDSMOVER_RUNNER_HPP) */
