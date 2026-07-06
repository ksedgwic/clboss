#ifndef LN_ONIONERROR_HPP
#define LN_ONIONERROR_HPP

#include<cstdint>
#include<string>

namespace Ln { namespace OnionError {

/** char const* Ln::OnionError::failcode_name(failcode)
 *
 * @brief BOLT 04 onion failure code -> short human-readable
 * name.  Used in sendpay-204 logs so the failcode can be
 * grepped by name (FEE_INSUFFICIENT etc.) rather than only
 * the hex.  Returns "UNKNOWN" for codes not in the table.
 */
char const* failcode_name(std::uint16_t failcode);

/** struct Ln::OnionError::ChanUpdate
 *
 * @brief Parsed BOLT 07 channel_update policy fields, as
 * extracted from the payload a BOLT 04 onion failure embeds.
 * Mirrors the subset of channel_update fields that
 * askrene-update-channel accepts, plus the bLIP-18 inbound-fee
 * TLV.
 */
struct ChanUpdate {
	bool          enabled;
	std::uint16_t cltv_expiry_delta;
	std::uint64_t htlc_minimum_msat;
	std::uint32_t fee_base_msat;
	std::uint32_t fee_proportional_millionths;
	std::uint64_t htlc_maximum_msat;
	/* bLIP-18 inbound fees (TLV 55555), signed.  has_inbound_fee
	 * is false when the channel_update carries no such TLV. */
	bool          has_inbound_fee                     = false;
	std::int32_t  inbound_fee_base_msat               = 0;
	std::int32_t  inbound_fee_proportional_millionths = 0;

	/* Compares only the fields askrene consumes, deliberately
	 * excluding the inbound-fee TLV: the use case is detecting a
	 * forwarder that returns the identical signed policy we
	 * already applied (enforcement diverging from gossip), and
	 * askrene never prices inbound fees, so a TLV-only change
	 * would not alter any route we build. */
	bool operator==(ChanUpdate const& o) const {
		return enabled == o.enabled
		    && cltv_expiry_delta == o.cltv_expiry_delta
		    && htlc_minimum_msat == o.htlc_minimum_msat
		    && fee_base_msat == o.fee_base_msat
		    && fee_proportional_millionths == o.fee_proportional_millionths
		    && htlc_maximum_msat == o.htlc_maximum_msat;
	}
};

/** bool Ln::OnionError::parse_chan_update(raw_message_hex, out)
 *
 * @brief Parse a BOLT 04 onion failure payload (the
 * `raw_message` hex from sendpay_failure data) and extract the
 * embedded BOLT 07 channel_update fields.  Returns true on
 * success and writes the parsed values into `out`; returns
 * false if the hex is malformed, the failcode does not carry a
 * channel_update, the payload is truncated, or the update
 * carries an absurd policy (proportional fee above 100%).
 *
 * The proportional-fee bound doubles as an overflow guarantee
 * for callers: with fee_proportional_millionths <= 1e6, a
 * ceil(amount * prop / 1e6) computed in uint64 cannot wrap for
 * any Lightning-plausible amount, whereas a forwarder-signed
 * 0xFFFFFFFF would.
 *
 * Wire layout of the onion failure for the relevant failcodes:
 *
 *   2  failcode
 *   X  variable per-failcode header:
 *        0x1007 / 0x100e:                  0 bytes
 *        0x100b / 0x100c (amount):         8 bytes htlc_msat
 *        0x100d (cltv):                    4 bytes cltv_expiry
 *   2  channel_update length (big-endian)
 *   N  channel_update bytes
 *
 * The channel_update's 2-byte type prefix 0x0102 is present in
 * CLN-issued channel_updates and absent in LND-pre-v0.18 ones;
 * both forms are accepted (detected by sniffing the first two
 * bytes).
 */
bool parse_chan_update( std::string const& raw_message_hex
		      , ChanUpdate& out
		      );

}}

#endif /* !defined(LN_ONIONERROR_HPP) */
