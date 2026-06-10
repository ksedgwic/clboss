#ifndef BOSS_MOD_XREBALANCEPREDICTOR_HPP
#define BOSS_MOD_XREBALANCEPREDICTOR_HPP

#include"Boss/Mod/XRebalancePredict.hpp"
#include"Ev/now.hpp"
#include<cstddef>
#include<cstdint>
#include<functional>
#include<memory>
#include<string>
#include<vector>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::XRebalancePredictor
 *
 * @brief The live persistence forecaster (phase 2 of the
 * history+prediction design).
 *
 * @desc After each hourly aging pass over the clboss-xrebalance
 * layer (Boss::Msg::XRebalanceLayerAged), reads the
 * XRebalanceHistory observation store, runs the pure
 * Boss::Mod::XRebalancePredict algorithm per channel direction,
 * and re-asserts the surviving walls/floors into the routed layer
 * via askrene-inform-channel -- but ONLY for directions whose
 * newest real observation predates the aging cutoff (directions
 * with live evidence need no synthesis).  Synthetic assertions are
 * NEVER recorded back into the observation store (history purity;
 * no self-confirmation).
 *
 * OFF BY DEFAULT: the master switch is the dynamic option
 * clboss-xrebalance-predict-horizon-max-secs (0 = disabled), so a
 * build carrying this module changes nothing until the operator
 * flips it via setconfig.  Also dormant unless
 * clboss-rebalance-mode is xrebalance.  All constants are dynamic
 * options, mirroring the per-query override parameters of the
 * read-only clboss-xrebalance-history / -predictions spot-check
 * commands.
 */
class XRebalancePredictor {
public:
	/* One observation-store row; input to plan().  */
	struct Row {
		std::string scid;
		std::uint32_t dir;
		std::uint64_t time;
		std::string kind;
		std::uint64_t amount_msat;
	};
	/* One synthetic inform the live predictor would issue.  */
	struct Assertion {
		std::string scid;
		std::uint32_t dir;
		/* true: inform constrained (wall);
		 * false: inform unconstrained (floor).  */
		bool is_wall;
		std::uint64_t amount_msat;
	};
	struct Plan {
		std::vector<Assertion> assertions;
		/* Channel directions present in the store.  */
		std::size_t directions;
		/* Directions without live evidence (newest real
		 * observation older than the aging cutoff) -- the
		 * candidate set.  */
		std::size_t candidates;
	};

	/* The full per-cycle decision, as a pure function (exposed
	 * for unit testing): group rows per direction, gate on
	 * candidacy, run XRebalancePredict::predict, collect the
	 * asserting sides.  `rows` in any order.  */
	static Plan plan( std::vector<Row> const& rows
			, std::uint64_t cutoff
			, std::uint64_t now
			, XRebalancePredict::Params const& params
			);

private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	XRebalancePredictor() =delete;

	XRebalancePredictor(XRebalancePredictor&&);
	~XRebalancePredictor();

	explicit
	XRebalancePredictor( S::Bus& bus
			   , std::function<double()> get_now_ = &Ev::now
			   );
};

}}

#endif /* !defined(BOSS_MOD_XREBALANCEPREDICTOR_HPP) */
