#include"Ln/OnionError.hpp"
#include"Util/Str.hpp"
#include<vector>

namespace {

/* Read a big-endian unsigned integer of 1..8 bytes from `data`
 * starting at `offset`.  Caller ensures the read is in-bounds.
 */
std::uint64_t read_be( std::uint8_t const* data
		     , std::size_t offset
		     , std::size_t nbytes
		     ) {
	auto v = std::uint64_t(0);
	for (auto i = std::size_t(0); i < nbytes; ++i)
		v = (v << 8) | std::uint64_t(data[offset + i]);
	return v;
}

/* Read a BOLT 01 BigSize at `pos` in `data` (size `size`), advancing
 * `pos` past it.  Returns false if truncated. */
bool read_bigsize( std::uint8_t const* data
		 , std::size_t size
		 , std::size_t& pos
		 , std::uint64_t& out
		 ) {
	if (pos >= size)
		return false;
	auto first = data[pos];
	auto nbytes = std::size_t( first < 0xfd ? 0
				 : first == 0xfd ? 2
				 : first == 0xfe ? 4
				 :                 8 );
	if (nbytes == 0) {
		out = first;
		pos += 1;
		return true;
	}
	if (pos + 1 + nbytes > size)
		return false;
	out = read_be(data, pos + 1, nbytes);
	pos += 1 + nbytes;
	return true;
}

}

namespace Ln { namespace OnionError {

char const* failcode_name(std::uint16_t f) {
	switch (f) {
	case 0x1007: return "TEMPORARY_CHANNEL_FAILURE";
	case 0x100b: return "AMOUNT_BELOW_MINIMUM";
	case 0x100c: return "FEE_INSUFFICIENT";
	case 0x100d: return "INCORRECT_CLTV_EXPIRY";
	case 0x100e: return "EXPIRY_TOO_SOON";
	case 0x1014: return "CHANNEL_DISABLED";
	case 0x2002: return "TEMPORARY_NODE_FAILURE";
	case 0x4008: return "PERMANENT_CHANNEL_FAILURE";
	case 0x400a: return "UNKNOWN_NEXT_PEER";
	case 0x4010: return "REQUIRED_CHANNEL_FEATURE_MISSING";
	case 0x6002: return "PERMANENT_NODE_FAILURE";
	default:     return "UNKNOWN";
	}
}

bool parse_chan_update( std::string const& raw_message_hex
		      , ChanUpdate& out
		      ) {
	std::vector<std::uint8_t> bytes;
	try {
		bytes = Util::Str::hexread(raw_message_hex);
	} catch (std::exception const&) {
		return false;
	}
	if (bytes.size() < 4)
		return false;

	auto failcode = std::uint16_t((bytes[0] << 8) | bytes[1]);
	auto header   = std::size_t(0);
	switch (failcode) {
	case 0x1007: case 0x100e:           header = 0; break;
	case 0x100b: case 0x100c:           header = 8; break;
	case 0x100d:                        header = 4; break;
	default:                            return false;
	}
	auto pos = std::size_t(2) + header;
	if (bytes.size() < pos + 2)
		return false;
	auto cu_len = std::size_t((bytes[pos] << 8) | bytes[pos + 1]);
	pos += 2;
	if (cu_len == 0 || bytes.size() < pos + cu_len)
		return false;

	auto cu      = bytes.data() + pos;
	auto cu_size = cu_len;
	/* Skip the optional 2-byte type prefix 0x0102 if present. */
	if (cu_size >= 2 && cu[0] == 0x01 && cu[1] == 0x02) {
		cu      += 2;
		cu_size -= 2;
	}
	/* Fixed-layout body from offset 0 of the post-prefix
	 * channel_update: 64 sig + 32 chain_hash + 8
	 * short_channel_id + 4 timestamp + 1 message_flags + 1
	 * channel_flags + 2 cltv_expiry_delta + 8 htlc_minimum_msat
	 * + 4 fee_base_msat + 4 fee_proportional_millionths + 8
	 * htlc_maximum_msat = 136.
	 */
	if (cu_size < 136)
		return false;

	auto channel_flags = cu[109];
	out.enabled                     = !(channel_flags & 0x02);
	out.cltv_expiry_delta           = std::uint16_t(read_be(cu, 110, 2));
	out.htlc_minimum_msat           = read_be(cu, 112, 8);
	out.fee_base_msat               = std::uint32_t(read_be(cu, 120, 4));
	out.fee_proportional_millionths = std::uint32_t(read_be(cu, 124, 4));
	out.htlc_maximum_msat           = read_be(cu, 128, 8);

	/* Reject absurd signed policies rather than propagating them.
	 * A proportional fee above 100% is never a policy we would
	 * pay, and bounding it here is the overflow guarantee the
	 * header doc promises callers.  Failing the parse routes the
	 * failure into the callers' max_msat=0 hard-exclusion
	 * fallback -- the right response to a forwarder signing
	 * garbage.
	 */
	if (out.fee_proportional_millionths > 1000000)
		return false;

	/* Scan the trailing TLV stream for bLIP-18 inbound fees
	 * (type 55555): value is [i32 base][i32 prop], both signed. */
	out.has_inbound_fee                     = false;
	out.inbound_fee_base_msat               = 0;
	out.inbound_fee_proportional_millionths = 0;
	auto tpos = std::size_t(136);
	while (tpos < cu_size) {
		auto ttype = std::uint64_t(0);
		auto tlen  = std::uint64_t(0);
		if (!read_bigsize(cu, cu_size, tpos, ttype))
			break;
		if (!read_bigsize(cu, cu_size, tpos, tlen))
			break;
		/* Overflow-safe: tpos <= cu_size (guaranteed by
		 * read_bigsize) so cu_size - tpos cannot underflow,
		 * whereas tpos + tlen can wrap for an attacker-supplied
		 * tlen and slip past a `> cu_size` check. */
		if (tlen > std::uint64_t(cu_size - tpos))
			break;
		if (ttype == 55555 && tlen == 8) {
			out.has_inbound_fee = true;
			out.inbound_fee_base_msat =
			    std::int32_t(std::uint32_t(read_be(cu, tpos, 4)));
			out.inbound_fee_proportional_millionths =
			    std::int32_t(std::uint32_t(read_be(cu, tpos + 4, 4)));
		}
		tpos += tlen;
	}
	return true;
}

}}
