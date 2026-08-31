#ifndef BOSS_MOD_DOWSER_HPP
#define BOSS_MOD_DOWSER_HPP

#include"Ln/NodeId.hpp"
#include<memory>

namespace Ln { class Amount; }
namespace Boss { namespace Mod { class Rpc; }}
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::Dowser
 *
 * @brief Determines the amount of plausible flow between two nodes.
 *
 * @desc This module responds to `Boss::Msg::RequestDowser`.
 */
class Dowser {
private:
	S::Bus& bus;
	Boss::Mod::Rpc* rpc;
	Ln::NodeId self_id;

	class CommandImpl;
	std::unique_ptr<CommandImpl> cmdimpl;

	class Run;

	void start();

public:
	Dowser() =delete;
	Dowser(Dowser&&) =delete;
	Dowser(Dowser const&) =delete;

	/* Lower bracket of the binary search: legacy
	 * probe_amount/32 + 1 msat, or — when the caller passes a
	 * relevance floor_target — the smaller of that and
	 * floor_target/0.985 + 1 sat, so flows the caller would accept
	 * are measured instead of zeroed (the E15-d residual band).
	 * Pinned by tests/boss/test_dowser_floor.cpp.  */
	static
	Ln::Amount floor_probe_amount( Ln::Amount probe_amount
				     , Ln::Amount floor_target
				     );

	~Dowser();
	explicit
	Dowser(S::Bus& bus);
};

}}

#endif /* !defined(BOSS_MOD_DOWSER_HPP) */
