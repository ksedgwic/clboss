#ifndef BOSS_MSG_LISTPEERSRESULT_HPP
#define BOSS_MSG_LISTPEERSRESULT_HPP

#include"Boss/Mod/ConstructedListpeers.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::ListpeersResult
 *
 * @brief announced during `init` to inform all
 * modules about `listpeers` command result.
 * Also announced every 10 minutes.
 *
 * IMPORTANT - this msg is no longer directly obtained from
 * `listpeers` but rather is constructed by "convolving" the value
 * from `listpeerchannels`.  Specifically, the top level `peer`
 * objects are non-standard and only have what CLBOSS uses ...
 * A peer without channels has an entry too, taken from `listpeers`.
 */

struct ListpeersResult {
	Boss::Mod::ConstructedListpeers cpeers;

	/* Whether this listpeerchannelss was performed during `init`
	 * or on the 10-minute timer.
	 */
	bool initial;
};

}}

#endif /* !defined(BOSS_MSG_LISTPEERSRESULT_HPP) */
