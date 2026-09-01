#include"Boss/Mod/NodeBalanceSwapper.hpp"
#include"Boss/ModG/Swapper.hpp"
#include"Boss/Msg/ListpeersResult.hpp"
#include"Boss/Msg/OnchainFee.hpp"
#include"Boss/log.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/Amount.hpp"
#include"S/Bus.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<sstream>

namespace {

/* The minimum percentage of total channel capacity that is
 * incoming.
 *
 * Cannot be higher than 50.
 */
auto const min_receivable_percent = double(35);
/* If fees are high, we do not swap, ***unless*** our incoming
 * capacity is below this percentage, in which case we end up
 * triggering anyway.
 */
auto const min_receivable_percent_highfees = double(2.5);

}

namespace Boss { namespace Mod {

class NodeBalanceSwapper::Impl : ModG::Swapper {
private:
	std::unique_ptr<bool> fees_low;

	void start() {
		bus.subscribe<Msg::OnchainFee
			     >([this](Msg::OnchainFee const& m) {
			if (!fees_low)
				fees_low = Util::make_unique<bool>();
			*fees_low = m.fees_low;
			return Ev::lift();
		});
		bus.subscribe<Msg::ListpeersResult
			     >([this](Msg::ListpeersResult const& m) {
			if (!fees_low)
				/* Pretend fees are high temporarily.  */
				fees_low = Util::make_unique<bool>(false);
			/* Analyze the result.  */
			auto total_recv = Ln::Amount::sat(0);
			auto total_send = Ln::Amount::sat(0);
			try {
				for (auto peer : m.cpeers) {
                                  	auto channels = peer.second.channels;
					for (auto chan : channels) {
						/* Skip non-active channels.
						 */
						auto state = std::string(
							chan["state"]
						);
						if ( state != "CHANNELD_NORMAL"
						  && state != "CHANNELD_AWAITING_SPLICE"
						   )
							continue;
						auto recv = Ln::Amount();
						auto send = Ln::Amount();
						compute_sendable_receivable(
							send, recv, chan
						);
						total_recv += recv;
						total_send += send;
					}
				}
			} catch (Jsmn::TypeError const& e) {
				/* Should never happen.... */
				return Boss::log( bus, Error
						  , "NodeBalanceSwapper:"
						  " Unexpected exception: %s"
						  " handling: %s"
						  , e.what()
						  , Util::stringify(m.cpeers).c_str()
					);
			}

			/* Avoid division by 0. */
			if ( total_recv == Ln::Amount::sat(0)
			  && total_send == Ln::Amount::sat(0)
			   )
				return Ev::lift();

			auto total = total_recv + total_send;
			auto recv_ratio = total_recv / total;
			auto recv_percent = recv_ratio * 100;
			if (recv_percent >= min_receivable_percent)
				/* Nothing to do.  */
				return Ev::lift();

			auto f = [ this
				 , total_recv
				 , total_send
				 , recv_percent
				 ]( Ln::Amount& amount
				  , std::string& why
				  ) {
				amount = ( (total_send - total_recv)
					 * 2
					 ) / 3;

				auto os = std::ostringstream();
				os << "receivable = " << total_recv << ", "
				   << "spendable = " << total_send << "; "
				   << "%receivable = " << recv_percent << "; "
				    ;
				if (*fees_low) {
					os << "sending funds onchain";
					why = os.str();
					return true;
				} else if ( recv_percent
					  > min_receivable_percent_highfees
					  ) {
					os << "but fees are high";
					why = os.str();
					return false;
				} else {
					os << "fees are high but we have "
					   << "too little receivable"
					   ;
					why = os.str();
					return true;
				}
			};

			return trigger_swap(f);
		});
	}

	void
	compute_sendable_receivable( Ln::Amount& send
				   , Ln::Amount& recv
				   , Jsmn::Object const& c
				   ) {
		auto to_us = Ln::Amount::object(c["to_us_msat"]);
		auto total = Ln::Amount::object(c["total_msat"]);
		auto their = total - to_us;
		for (auto h : c["htlcs"])
			their -= Ln::Amount::object(h["amount_msat"]);
		auto our_reserve = Ln::Amount::object(
			c["our_reserve_msat"]
		);
		auto their_reserve = Ln::Amount::object(
			c["their_reserve_msat"]
		);
		send = to_us - our_reserve;
		recv = their - their_reserve;
	}

public:
	explicit
	Impl(S::Bus& bus_) : ModG::Swapper( bus_
					  , "NodeBalanceSwapper"
					  , "incoming_capacity_swapper"
					  ) { start(); }
};

NodeBalanceSwapper::NodeBalanceSwapper(NodeBalanceSwapper&&) =default;
NodeBalanceSwapper& NodeBalanceSwapper::operator=(NodeBalanceSwapper&&)
	=default;
NodeBalanceSwapper::~NodeBalanceSwapper() =default;

NodeBalanceSwapper::NodeBalanceSwapper(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
