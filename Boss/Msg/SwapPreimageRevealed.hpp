#ifndef BOSS_MSG_SWAPPREIMAGEREVEALED_HPP
#define BOSS_MSG_SWAPPREIMAGEREVEALED_HPP

#include"Sha256/Hash.hpp"
#include<memory>

namespace Sqlite3 { class Tx; }

namespace Boss { namespace Msg {

/** struct Boss::Msg::SwapPreimageRevealed
 *
 * @brief emitted by a swap provider when the preimage
 * of a swap is about to leave the node.  From then on
 * the provider can settle our payment, so the swap
 * counts as paid whatever `listpays` says.
 *
 * @desc The database transaction is the one recording
 * the reveal, so a subscriber that writes to it records
 * the reveal atomically with the provider.  The provider
 * commits it after the message is raised.
 */
struct SwapPreimageRevealed {
	std::shared_ptr<Sqlite3::Tx> dbtx;
	Sha256::Hash payment_hash;
};

}}

#endif /* !defined(BOSS_MSG_SWAPPREIMAGEREVEALED_HPP) */
