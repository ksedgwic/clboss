#ifndef BOSS_MOD_DOWSER_HPP
#define BOSS_MOD_DOWSER_HPP

#include"Ln/NodeId.hpp"
#include<memory>

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

	~Dowser();
	explicit
	Dowser(S::Bus& bus);
};

}}

#endif /* !defined(BOSS_MOD_DOWSER_HPP) */
