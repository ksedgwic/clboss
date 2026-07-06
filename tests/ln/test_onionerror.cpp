#undef NDEBUG
#include"Ln/OnionError.hpp"
#include"Util/Str.hpp"
#include<assert.h>
#include<cstdint>
#include<string>
#include<vector>

namespace {

/* Append a big-endian integer of `nbytes` to `b`.  */
void push_be( std::vector<std::uint8_t>& b
	    , std::uint64_t v
	    , std::size_t nbytes
	    ) {
	for (auto i = std::size_t(0); i < nbytes; ++i)
		b.push_back(std::uint8_t((v >> (8 * (nbytes - 1 - i))) & 0xff));
}

/* Build a 136-byte BOLT 07 channel_update body (no type prefix):
 * 64 sig + 32 chain_hash + 8 scid + 4 timestamp + 1 message_flags
 * + 1 channel_flags + 2 cltv + 8 htlc_min + 4 base + 4 prop
 * + 8 htlc_max, sig..timestamp left zeroed. */
std::vector<std::uint8_t> make_cu_body( bool disabled
				      , std::uint16_t cltv
				      , std::uint64_t htlc_min
				      , std::uint32_t base
				      , std::uint32_t prop
				      , std::uint64_t htlc_max
				      ) {
	auto b = std::vector<std::uint8_t>(110, 0);
	b[109] = disabled ? 0x02 : 0x00;
	push_be(b, cltv, 2);
	push_be(b, htlc_min, 8);
	push_be(b, base, 4);
	push_be(b, prop, 4);
	push_be(b, htlc_max, 8);
	assert(b.size() == 136);
	return b;
}

/* Wrap a channel_update in a BOLT 04 onion failure payload:
 * failcode + `header` zero bytes + 2-byte length + optional
 * 0x0102 type prefix + the update. */
std::string make_onion_hex( std::uint16_t failcode
			  , std::size_t header
			  , std::vector<std::uint8_t> const& cu
			  , bool type_prefix
			  ) {
	auto m = std::vector<std::uint8_t>();
	push_be(m, failcode, 2);
	m.insert(m.end(), header, 0);
	auto full = std::vector<std::uint8_t>();
	if (type_prefix) {
		full.push_back(0x01);
		full.push_back(0x02);
	}
	full.insert(full.end(), cu.begin(), cu.end());
	push_be(m, full.size(), 2);
	m.insert(m.end(), full.begin(), full.end());
	return Util::Str::hexdump(m.data(), m.size());
}

/* Append a bLIP-18 inbound-fee TLV (type 55555) with the given
 * length byte and [i32 base][i32 prop] value (value always 8
 * bytes; a `len` other than 8 makes the TLV malformed-for-us
 * on purpose). */
void push_inbound_tlv( std::vector<std::uint8_t>& cu
		     , std::uint8_t len
		     , std::int32_t base
		     , std::int32_t prop
		     ) {
	/* type 55555 = 0xd903 needs the 0xfd bigsize form. */
	cu.push_back(0xfd);
	push_be(cu, 55555, 2);
	cu.push_back(len);
	push_be(cu, std::uint32_t(base), 4);
	push_be(cu, std::uint32_t(prop), 4);
	for (auto i = std::size_t(8); i < std::size_t(len); ++i)
		cu.push_back(0);
}

}

int main() {
	using Ln::OnionError::ChanUpdate;
	using Ln::OnionError::parse_chan_update;
	using Ln::OnionError::failcode_name;

	/* Happy path: 0x100c (8-byte header), CLN-style 0x0102 type
	 * prefix, no TLVs. */
	{
		auto cu_bytes = make_cu_body(false, 144, 1000, 1234, 567,
					     1000000000);
		auto cu = ChanUpdate();
		assert(parse_chan_update(
			make_onion_hex(0x100c, 8, cu_bytes, true), cu));
		assert(cu.enabled);
		assert(cu.cltv_expiry_delta == 144);
		assert(cu.htlc_minimum_msat == 1000);
		assert(cu.fee_base_msat == 1234);
		assert(cu.fee_proportional_millionths == 567);
		assert(cu.htlc_maximum_msat == 1000000000);
		assert(!cu.has_inbound_fee);
	}

	/* Same update without the type prefix (LND-pre-v0.18 style),
	 * and via a 0x100d failure (4-byte header). */
	{
		auto cu_bytes = make_cu_body(true, 40, 1, 0, 100, 21000000);
		auto cu = ChanUpdate();
		assert(parse_chan_update(
			make_onion_hex(0x100d, 4, cu_bytes, false), cu));
		assert(!cu.enabled);
		assert(cu.cltv_expiry_delta == 40);
		assert(cu.fee_proportional_millionths == 100);
	}

	/* Proportional fee above 100% is rejected: a forwarder-signed
	 * absurd policy must fail the parse (and thus route callers
	 * into their hard-exclusion fallback), not propagate. */
	{
		auto cu_bytes = make_cu_body(false, 144, 0, 0, 1000001, 1);
		auto cu = ChanUpdate();
		assert(!parse_chan_update(
			make_onion_hex(0x100c, 8, cu_bytes, true), cu));
		/* Exactly 100% is still accepted. */
		cu_bytes = make_cu_body(false, 144, 0, 0, 1000000, 1);
		assert(parse_chan_update(
			make_onion_hex(0x100c, 8, cu_bytes, true), cu));
	}

	/* bLIP-18 inbound-fee TLV, including negative (discount)
	 * base: values are signed i32. */
	{
		auto cu_bytes = make_cu_body(false, 144, 0, 0, 0, 1);
		push_inbound_tlv(cu_bytes, 8, -1000, 250);
		auto cu = ChanUpdate();
		assert(parse_chan_update(
			make_onion_hex(0x100b, 8, cu_bytes, true), cu));
		assert(cu.has_inbound_fee);
		assert(cu.inbound_fee_base_msat == -1000);
		assert(cu.inbound_fee_proportional_millionths == 250);
	}

	/* TLV length must be exactly 8: a 9-byte 55555 TLV is not
	 * treated as an inbound fee (but does not fail the parse). */
	{
		auto cu_bytes = make_cu_body(false, 144, 0, 0, 0, 1);
		push_inbound_tlv(cu_bytes, 9, 1000, 250);
		auto cu = ChanUpdate();
		assert(parse_chan_update(
			make_onion_hex(0x100c, 8, cu_bytes, true), cu));
		assert(!cu.has_inbound_fee);
	}

	/* Malicious TLV length that would wrap a naive
	 * `tpos + tlen > cu_size` bounds check: bigsize 0xff with
	 * 0xFFFFFFFFFFFFFFFF.  The parse must neither crash nor
	 * accept the TLV. */
	{
		auto cu_bytes = make_cu_body(false, 144, 0, 0, 0, 1);
		cu_bytes.push_back(0xfd);
		push_be(cu_bytes, 55555, 2);
		cu_bytes.push_back(0xff);
		push_be(cu_bytes, 0xFFFFFFFFFFFFFFFFull, 8);
		/* 8 in-bounds bytes a wrapped check would misread. */
		push_be(cu_bytes, 0x1122334455667788ull, 8);
		auto cu = ChanUpdate();
		assert(parse_chan_update(
			make_onion_hex(0x100c, 8, cu_bytes, true), cu));
		assert(!cu.has_inbound_fee);
	}

	/* Failcodes that carry no channel_update are rejected. */
	{
		auto cu_bytes = make_cu_body(false, 144, 0, 0, 0, 1);
		auto cu = ChanUpdate();
		assert(!parse_chan_update(
			make_onion_hex(0x2002, 0, cu_bytes, true), cu));
	}

	/* Truncated payloads and garbage hex are rejected. */
	{
		auto cu_bytes = make_cu_body(false, 144, 0, 0, 0, 1);
		auto hex = make_onion_hex(0x100c, 8, cu_bytes, true);
		auto cu = ChanUpdate();
		assert(!parse_chan_update(hex.substr(0, hex.size() - 40), cu));
		assert(!parse_chan_update("zznothexzz", cu));
		assert(!parse_chan_update("", cu));
		/* Zero-length channel_update. */
		assert(!parse_chan_update("100c00000000000000000000", cu));
	}

	/* operator== compares the askrene-visible policy only; a
	 * difference confined to the inbound-fee TLV still compares
	 * equal (repeat-update detection semantics). */
	{
		auto mk = [](std::uint32_t prop) {
			auto cu = ChanUpdate();
			cu.enabled = true;
			cu.cltv_expiry_delta = 144;
			cu.htlc_minimum_msat = 1000;
			cu.fee_base_msat = 0;
			cu.fee_proportional_millionths = prop;
			cu.htlc_maximum_msat = 1;
			return cu;
		};
		auto a = mk(100);
		auto b = mk(100);
		assert(a == b);
		b.has_inbound_fee = true;
		b.inbound_fee_base_msat = 5000;
		assert(a == b);
		auto c = mk(101);
		assert(!(a == c));
	}

	assert(std::string(failcode_name(0x100c)) == "FEE_INSUFFICIENT");
	assert(std::string(failcode_name(0x100d)) == "INCORRECT_CLTV_EXPIRY");
	assert(std::string(failcode_name(0xdead)) == "UNKNOWN");

	return 0;
}
