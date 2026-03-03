#ifndef BOSS_MSG_SENDPAYRESULT_HPP
#define BOSS_MSG_SENDPAYRESULT_HPP

#include<cstdint>
#include"Ln/NodeId.hpp"
#include"Sha256/Hash.hpp"

namespace Boss { namespace Msg {

/** struct Boss::Msg::SendpayResult
 *
 * @brief informs all modules that we now know that a
 * particular `sendonion`/`sendpay` resulted in success
 * or failure.
 *
 * @desc In particular this tracks which of our direct
 * peers is the first hop in the route that failed.
 */
struct SendpayResult {
	/* Time the sendpay was initiated, in seconds since epoch.  */
	double creation_time;
	/* Time we learned this result, in seconds since epoch.  */
	double completion_time;
	/* The first hop of the route.  */
	Ln::NodeId first_hop;
	/* Hash of the payment.  */
	Sha256::Hash payment_hash;
	/* For multipart payments, which part.  0 for single-part payments.  */
	std::uint64_t partid;
	/* Whether it succeeded or failed.  */
	bool success;
	/* Whether it reached destination or not.  */
	bool reached_destination;
};

}}

#endif /* !defined(BOSS_MSG_SENDPAYRESULT_HPP) */
