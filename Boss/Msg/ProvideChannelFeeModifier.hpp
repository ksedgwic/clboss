#ifndef BOSS_MSG_PROVIDECHANNELFEEMODIFIER_HPP
#define BOSS_MSG_PROVIDECHANNELFEEMODIFIER_HPP

#include<cstdint>
#include<functional>

namespace Ev { template<typename a> class Io; }
namespace Ln { class NodeId; }

namespace Boss { namespace Msg {

/** Boss::Msg::ProvideChannelFeeModifier
 *
 * @brief emitted in response to the
 * `Boss::Msg::SolicitChannelFeeModifier`
 * message.
 *
 * @desc the function you put into this
 * message is given the node ID, plus the
 * raw base median feerate and raw
 * proportional median feerate.
 *
 * The given function must then return a
 * `double` that is multiplied, together
 * with the results from other modifiers.
 */
struct ProvideChannelFeeModifier {
	std::function< Ev::Io<double>( Ln::NodeId
				     , std::uint32_t /* base */
				     , std::uint32_t /* proportional */
				     )
		     > modifier;
};

}}

#endif /* !defined(BOSS_MSG_PROVIDECHANNELFEEMODIFIER_HPP) */
