#ifndef BOLTZ_ENVIF_HPP
#define BOLTZ_ENVIF_HPP

#include"Bitcoin/TxId.hpp"
#include<cstdint>
#include<string>

namespace Bitcoin { class Tx; }
namespace Ev { template<typename a> class Io; }

namespace Boltz {

/** class Boltz::EnvIF
 *
 * @brief abstract interface to the local
 * environment the BOLTZ-swapping code is
 * running in.
 */
class EnvIF {
public:
	/** Boltz::EnvIF::get_feerate
	 *
	 * @brief get the recommended feerate
	 * for normal transactions.
	 */
	virtual
	Ev::Io<std::uint32_t> get_feerate() =0;

	/** Boltz::EnvIF::get_block
	 *
	 * @brief get the block at the given height from
	 * our own node, as the hex serialization of the
	 * block.
	 *
	 * @return "" if our node has no block at that
	 * height yet, or the fetch failed.
	 */
	virtual
	Ev::Io<std::string> get_block(std::uint32_t height) =0;

	/** Boltz::EnvIF::has_confirmed_output
	 *
	 * @brief whether our own node's wallet holds the
	 * given output, in a block, spent since or not.
	 *
	 * @desc the output is named by transaction id, not
	 * by address: a payment from anyone else to the
	 * same address must not pass for it.
	 */
	virtual
	Ev::Io<bool> has_confirmed_output( Bitcoin::TxId txid
					 , std::uint32_t outnum
					 ) =0;

	/** Boltz::EnvIF::cooperative_claims
	 *
	 * @brief whether claims ask the provider to co-sign
	 * first.  False makes every claim go through the
	 * claim leaf, for exercising that path; CLBOSS
	 * itself always answers true.
	 */
	virtual
	bool cooperative_claims() { return true; }

	/** Boltz::EnvIF::broadcast_tx
	 *
	 * @brief broadcasts the transaction.
	 *
	 * @return false if broadcasting failed,
	 * true if broadcasting succeeded.
	 */
	virtual
	Ev::Io<bool> broadcast_tx(Bitcoin::Tx) =0;

	/** Boltz::EnvIF::logd
	 *
	 * @brief prints a Debug-level log.
	 */
	virtual
	Ev::Io<void> logd(std::string) =0;

	/** Boltz::EnvIF::logi
	 *
	 * @brief prints an Info-level log: the milestones of a
	 * swap, few enough to read at that level.
	 */
	virtual
	Ev::Io<void> logi(std::string) =0;

	/** Boltz::EnvIF::loge
	 *
	 * @brief prints an Erro-level log.
	 */
	virtual
	Ev::Io<void> loge(std::string) =0;
};

}

#endif /* !defined(BOLTZ_ENVIF_HPP) */
