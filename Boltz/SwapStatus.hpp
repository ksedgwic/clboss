#ifndef BOLTZ_SWAPSTATUS_HPP
#define BOLTZ_SWAPSTATUS_HPP

#include"Ln/Amount.hpp"
#include<cstdint>
#include<string>

namespace Boltz {

/** struct Boltz::SwapStatus
 *
 * @brief one reverse swap in flight, as the service's table
 * records it; for status reports.
 */
struct SwapStatus {
	std::string swap_id;
	/* False for a swap made by the API v1 code.  */
	bool api_v2;
	std::string destination_address;
	std::uint32_t created_height;
	std::uint32_t timeout_height;
	Ln::Amount onchain_amount;
	/* The lockup the provider reported; empty before.  */
	std::string lockup_txid;
	/* 0 until the lockup is found in our own blocks.  */
	std::uint32_t lockup_confirmed_height;
	std::uint32_t lockup_searched_height;
	bool preimage_revealed;
	/* The claim broadcast; empty before.  */
	std::string claim_txid;
	/* The claim's feerate in perkw and its fee.  */
	std::uint32_t claim_feerate;
	Ln::Amount claim_fee;
	std::uint32_t spend_searched_height;
	std::string comment;
};

}

#endif /* !defined(BOLTZ_SWAPSTATUS_HPP) */
