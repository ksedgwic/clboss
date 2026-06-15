#ifndef BOSS_MOD_XMOVEFUNDS_MAIN_HPP
#define BOSS_MOD_XMOVEFUNDS_MAIN_HPP

#include<memory>

namespace S { class Bus; }

namespace Boss { namespace Mod { namespace XMoveFunds {

/** class Boss::Mod::XMoveFunds::Main
 *
 * @brief Manual single-shot rebalance primitive.  Registers the
 * `clboss-xmovefunds` RPC command.  Given one or more source
 * channels (drain candidates) and one or more destination
 * channels (fill candidates) plus an amount, builds a transient
 * askrene layer that masks all other directions, calls
 * askrene-getroutes with source=us destination=us (circular mode
 * in the patched askrene), and optionally executes the returned
 * route via sendpay.
 *
 * Intended for one-off exploration and as the lowest-level
 * primitive that the eventual periodic xrebalance and JIT
 * xrebalance code paths will sit on top of.  Does not perform
 * tier selection, amount auto-sizing, or any algorithm choices --
 * the caller supplies the explicit channel set and amount.
 */
class Main {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	Main() =delete;

	Main(Main&&);
	~Main();

	explicit
	Main(S::Bus&);
};

}}}

#endif /* !defined(BOSS_MOD_XMOVEFUNDS_MAIN_HPP) */
