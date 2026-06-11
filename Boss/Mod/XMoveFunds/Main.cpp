#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/XMoveFunds/Claimer.hpp"
#include"Boss/Mod/XMoveFunds/Main.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/Msg/XRebalanceAttribution.hpp"
#include"Boss/Msg/XRebalanceLayerAged.hpp"
#include"Boss/Msg/XRebalanceObservation.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/CommandId.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Preimage.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Sha256/Hash.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include"Uuid.hpp"
#include<algorithm>
#include<chrono>
#include<cinttypes>
#include<ctime>
#include<limits>
#include<memory>
#include<optional>
#include<random>
#include<set>
#include<sstream>
#include<vector>

namespace {

/* JSON-RPC error code we use for malformed parameters.  Matches
 * the JSONRPC2 invalid-params constant used elsewhere in clboss
 * (e.g. Dowser, MoveFundsCommand). */
constexpr int RPC_INVALID_PARAMS = -32602;

/* Parsed channel_update fields fed back into askrene via
 * AskreneLayer::update_channel after a sendpay 204 with an
 * onion-error failcode that carries a channel_update payload.
 * Mirrors the subset of BOLT 07 channel_update fields askrene-
 * update-channel accepts.
 *
 * Duplicated from FundsMover/Attempter.cpp for now -- both
 * sites parse the same wire format with the same field set.
 * Pulling the parser into a shared module (Util/, Ln/, or a
 * new Boss/Mod/ChanUpdate) is a separate cleanup tracked
 * apart from xrebalance work. */
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
};

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

/* Parse a BOLT 04 onion failure payload (the `raw_message` hex
 * from sendpay_failure data) and extract the embedded BOLT 07
 * channel_update fields.  Returns true on success and writes the
 * parsed values into `out`; returns false if the hex is malformed,
 * the failcode does not carry a channel_update, or the payload is
 * truncated.
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
 * channel_update wire layout (BOLT 07), 128 bytes after the
 * optional 2-byte 0x0102 type prefix.  We only need the policy
 * fields (offset 109 onwards in the body), so we skip past
 * signature (64), chain_hash (32), short_channel_id (8),
 * timestamp (4), and message_flags (1).  The 2-byte type prefix
 * is present in CLN-issued channel_updates and absent in
 * LND-pre-v0.18 ones; detect by sniffing the first two bytes.
 */
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
	if (cu_size < 136)
		return false;

	auto channel_flags = cu[109];
	out.enabled                     = !(channel_flags & 0x02);
	out.cltv_expiry_delta           = std::uint16_t(read_be(cu, 110, 2));
	out.htlc_minimum_msat           = read_be(cu, 112, 8);
	out.fee_base_msat               = std::uint32_t(read_be(cu, 120, 4));
	out.fee_proportional_millionths = std::uint32_t(read_be(cu, 124, 4));
	out.htlc_maximum_msat           = read_be(cu, 128, 8);

	/* Scan the trailing TLV stream for bLIP-18 inbound fees
	 * (type 55555): value is [i32 base][i32 prop], both signed. */
	out.has_inbound_fee                    = false;
	out.inbound_fee_base_msat              = 0;
	out.inbound_fee_proportional_millionths = 0;
	auto tpos = std::size_t(136);
	while (tpos < cu_size) {
		auto ttype = std::uint64_t(0);
		auto tlen  = std::uint64_t(0);
		if (!read_bigsize(cu, cu_size, tpos, ttype))
			break;
		if (!read_bigsize(cu, cu_size, tpos, tlen))
			break;
		if (tpos + tlen > cu_size)
			break;
		if (ttype == 55555 && tlen >= 8) {
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

/* Decode either a single scid string or an array of scid strings
 * from a JSON value into a vector.  Throws on type/format error
 * with a message suitable for surfacing in the RPC reply. */
std::vector<Ln::Scid>
parse_scid_list(Jsmn::Object const& j, char const* fieldname) {
	std::vector<Ln::Scid> out;
	auto push_one = [&out, fieldname](Jsmn::Object const& s) {
		if (!s.is_string())
			throw std::runtime_error(
				std::string(fieldname)
				+ " must be a scid string or array of "
				  "scid strings");
		out.emplace_back(std::string(s));
	};
	if (j.is_string()) {
		push_one(j);
	} else if (j.is_array()) {
		for (auto i = std::size_t(0); i < j.size(); ++i)
			push_one(j[i]);
	} else {
		throw std::runtime_error(
			std::string(fieldname)
			+ " must be a scid string or array of scid "
			  "strings");
	}
	if (out.empty())
		throw std::runtime_error(
			std::string(fieldname)
			+ " must be non-empty");
	return out;
}

std::string
join_scids(std::vector<Ln::Scid> const& v) {
	auto os = std::ostringstream();
	auto first = true;
	for (auto const& s : v) {
		if (!first) os << ",";
		os << std::string(s);
		first = false;
	}
	return os.str();
}

/* Render a per-hop amount/fee/cltv breakdown of an askrene route path,
 * one line per hop, for diagnostic logging.  fee = amount_in - amount_out
 * is what WE allocated to that hop; the implied ppm is taken over
 * amount_out.  Lets us see the full fee picture along a route and, on a
 * FEE_INSUFFICIENT, tell an under-allocation (our/askrene bug) apart from
 * an inbound surcharge (the node's, not in this payload). */
std::string
format_route_fees(Jsmn::Object const& path) {
	auto os = std::ostringstream();
	for (auto i = std::size_t(0); i < path.size(); ++i) {
		auto hop = path[i];
		auto scidd = hop.has("short_channel_id_dir")
			   ? std::string(hop["short_channel_id_dir"])
			   : std::string("?");
		auto ain = hop.has("amount_in_msat")
			 ? Ln::Amount::object(hop["amount_in_msat"]).to_msat()
			 : std::uint64_t(0);
		auto aout = hop.has("amount_out_msat")
			  ? Ln::Amount::object(hop["amount_out_msat"]).to_msat()
			  : std::uint64_t(0);
		auto fee = (ain >= aout) ? (ain - aout) : std::uint64_t(0);
		auto ppm = (aout > 0)
			 ? (unsigned long)(double(fee) * 1e6 / double(aout) + 0.5)
			 : 0ul;
		auto cin = hop.has("cltv_in")
			 ? std::uint32_t(double(hop["cltv_in"])) : 0u;
		auto cout = hop.has("cltv_out")
			  ? std::uint32_t(double(hop["cltv_out"])) : 0u;
		os << "  [" << i << "] " << scidd
		   << " in=" << Util::Str::group_digits(ain)
		   << " out=" << Util::Str::group_digits(aout)
		   << " fee=" << Util::Str::group_digits(fee)
		   << "msat(" << ppm << "ppm)"
		   << " cltv " << cin << "->" << cout
		   << " d=" << (cin >= cout ? cin - cout : 0u) << "\n";
	}
	return os.str();
}

/* Parse a JSON value as a u32, accepting either a JSON number or a
 * numeric string.  lightning-cli encodes unquoted CLI values as JSON
 * numbers (so `maxparts=10` arrives as the number 10), while object-
 * form RPC calls sometimes pass them as strings.  Mirrors the
 * permissive shape of Ln::Amount::object. */
std::uint32_t
parse_u32(Jsmn::Object const& o, char const* fieldname) {
	if (o.is_number()) {
		return std::uint32_t(double(o));
	}
	if (o.is_string()) {
		try {
			return std::uint32_t(
				std::stoul(std::string(o)));
		} catch (std::exception const&) {
			throw std::runtime_error(
				std::string(fieldname)
				+ " must be an integer");
		}
	}
	throw std::runtime_error(
		std::string(fieldname) + " must be an integer");
}

}

namespace Boss { namespace Mod { namespace XMoveFunds {

class Main::Impl {
private:
	S::Bus& bus;
	Boss::Mod::Rpc* rpc;
	Ln::NodeId self_id;
	Claimer claimer;
	/* True once create_xrebalance_layer() has resolved (either by
	 * successfully creating/finding the persistent layer, or by
	 * logging a non-fatal RpcError on older CLN).  Gated on by
	 * wait_for_ready() so that command handling never tries to
	 * use the layer before askrene is told about it. */
	bool layer_ready;
	/* Window for periodic askrene-age on the xrebalance layer.
	 * Tunable via the `clboss-xrebalance-age-secs` option (dynamic
	 * -- runtime mutable via `lightning-cli setconfig`).  Default
	 * 3600 mirrors FundsMover's production value.  Operators on
	 * networks with slower flows (signet) typically widen this
	 * via setconfig to keep accumulated capacity knowledge longer
	 * before constraints expire. */
	std::uint64_t aging_window_secs;
	/* For generating MPP groupids -- a u64 random value shared
	 * across all parts of one xmovefunds invocation. */
	std::mt19937_64 rng;

	struct Params {
		std::vector<Ln::Scid> source_scids;
		std::vector<Ln::Scid> dest_scids;
		Ln::Amount amount;
		/* Raw user-supplied caps.  At least one must be present;
		 * `maxfee` below is the binding cap actually passed to
		 * askrene.  Echoed back in the reply when present so the
		 * caller can see exactly what they asked for vs what
		 * ended up binding. */
		std::optional<Ln::Amount> maxfee_msat_in;
		std::optional<std::uint32_t> maxfee_ppm_in;
		/* Effective binding cap: the smaller of
		 * (maxfee_msat_in, amount_msat * maxfee_ppm_in / 1e6)
		 * when both are set, the single value when only one is
		 * set.  This is what gets passed to
		 * askrene-getroutes. */
		Ln::Amount maxfee;
		std::uint32_t maxparts;
		bool execute;
	};

	/* Parse the JSON params object.  Throws on bad input. */
	Params parse_params(Jsmn::Object const& params) {
		auto p = Params();
		p.maxparts = 10;
		/* execute=true is the default on signet -- the manual
		 * command actually executes there.  Caller passes
		 * execute=false to get the plan-only response
		 * (predict-and-compare mode). */
		p.execute = true;

		if (!params.is_object())
			throw std::runtime_error(
				"params must be an object "
				"(named-parameter form required)");

		if (!params.has("source_scid"))
			throw std::runtime_error("source_scid required");
		p.source_scids =
		    parse_scid_list(params["source_scid"], "source_scid");

		if (!params.has("dest_scid"))
			throw std::runtime_error("dest_scid required");
		p.dest_scids =
		    parse_scid_list(params["dest_scid"], "dest_scid");

		if (!params.has("amount_msat"))
			throw std::runtime_error("amount_msat required");
		try {
			p.amount = Ln::Amount::object(params["amount_msat"]);
		} catch (std::exception const&) {
			throw std::runtime_error(
				"amount_msat must be an integer number of "
				"msat (as a JSON number or string)");
		}
		auto amount_msat = std::uint64_t(p.amount.to_msat());
		if (amount_msat == 0)
			throw std::runtime_error("amount_msat must be > 0");

		/* Fee cap: caller must explicitly specify at least one
		 * of `maxfee_msat` (absolute cap) or `maxfee_ppm`
		 * (relative cap as parts-per-million of amount).  No
		 * default -- a silent default would be a real-funds
		 * foot-gun on mainnet and is the wrong primitive
		 * contract for the Layer 2 / Layer 3 callers that will
		 * build on top. */
		if (params.has("maxfee_msat")) {
			try {
				p.maxfee_msat_in = Ln::Amount::object(
				    params["maxfee_msat"]);
			} catch (std::exception const&) {
				throw std::runtime_error(
					"maxfee_msat must be an integer "
					"number of msat");
			}
		}
		if (params.has("maxfee_ppm")) {
			p.maxfee_ppm_in = parse_u32( params["maxfee_ppm"]
						   , "maxfee_ppm"
						   );
		}
		if (!p.maxfee_msat_in && !p.maxfee_ppm_in) {
			throw std::runtime_error(
				"at least one of maxfee_msat or "
				"maxfee_ppm must be specified");
		}
		/* Effective binding cap: take the more restrictive of
		 * the two when both are given.  Computed in msat so the
		 * comparison is direct.  Note this can legitimately
		 * round to 0 for small `amount` * small `ppm`; we let
		 * askrene surface "no usable paths" in that case rather
		 * than silently raise the cap -- the explicit-cap
		 * contract above is the whole point. */
		auto cap_msat = std::numeric_limits<std::uint64_t>::max();
		if (p.maxfee_msat_in) {
			cap_msat = std::min(
			    cap_msat,
			    std::uint64_t(p.maxfee_msat_in->to_msat()));
		}
		if (p.maxfee_ppm_in) {
			auto from_ppm = amount_msat
				      * std::uint64_t(*p.maxfee_ppm_in)
				      / std::uint64_t(1000000);
			cap_msat = std::min(cap_msat, from_ppm);
		}
		p.maxfee = Ln::Amount::msat(cap_msat);

		if (params.has("maxparts")) {
			p.maxparts = parse_u32(params["maxparts"],
					       "maxparts");
			if (p.maxparts == 0)
				throw std::runtime_error(
					"maxparts must be > 0");
		}

		if (params.has("execute")) {
			auto e = params["execute"];
			if (e.is_boolean()) {
				p.execute = bool(e);
			} else if (e.is_string()) {
				auto s = std::string(e);
				if (s == "true") p.execute = true;
				else if (s == "false") p.execute = false;
				else throw std::runtime_error(
					"execute must be a boolean");
			} else {
				throw std::runtime_error(
					"execute must be a boolean");
			}
		}

		return p;
	}

	/* Ensure the persistent xrebalance askrene layer exists.
	 * Called once at startup, fire-and-forget.  Idempotent: when
	 * persistent is true, askrene-create-layer succeeds even if
	 * the layer already exists.  Failures (e.g. CLN < v24.11
	 * where the RPC does not exist, or stock CLN that lacks the
	 * circular-routing patch) are logged but non-fatal --
	 * subsequent xmovefunds calls will surface the underlying
	 * crash if the caller invokes them. */
	Ev::Io<void> create_xrebalance_layer() {
		return Ev::lift().then([this]() {
			auto parms = Json::Out()
				.start_object()
					.field("layer",
					       Boss::Mod::AskreneLayer::
					           xrebalance_layer_name)
					.field("persistent", true)
				.end_object()
				;
			return rpc->command( "askrene-create-layer"
					   , std::move(parms)
					   );
		}).then([this](Jsmn::Object _) {
			layer_ready = true;
			return Boss::log( bus, Debug
					, "XMoveFunds: persistent "
					  "askrene layer '%s' ready"
					, Boss::Mod::AskreneLayer::
					      xrebalance_layer_name
					      .c_str()
					);
		}).catching<RpcError>([this](RpcError const& e) {
			/* Mark ready even on failure: degraded mode
			 * must still allow plan calls to proceed
			 * (their getroutes call will surface a clearer
			 * error than us deadlocking on
			 * wait_for_ready). */
			layer_ready = true;
			return Boss::log( bus, Error
					, "XMoveFunds: askrene-create-"
					  "layer (%s) failed: %s; will "
					  "proceed in degraded mode "
					  "(no persistent learning "
					  "layer)."
					, Boss::Mod::AskreneLayer::
					      xrebalance_layer_name
					      .c_str()
					, Util::stringify(e.error).c_str()
					);
		});
	}

	/* Extract per-part earnings attribution from one successful
	 * waitsendpay result and the matching askrene path, then
	 * raise Msg::XRebalanceAttribution for EarningsTracker.
	 *
	 * Source peer: path[0]["node_id_out"] -- the peer we forwarded
	 * to on the first real hop, i.e. the far end of the source
	 * channel for this part.  Dest peer: path[N-1]["node_id_in"]
	 * (a.k.a. fill_peer) -- the peer that forwarded back to us on
	 * the closing hop.
	 *
	 * Amount moved: waitsendpay.amount_msat (delivered = amount
	 * that returned to us via the closing hop).  Fee spent:
	 * waitsendpay.amount_sent_msat - amount_msat (total paid
	 * across middle hops for this part).
	 *
	 * Defensive on malformed inputs: any missing field skips the
	 * raise (returns Ev::lift()).  In practice the path and
	 * waitsendpay result will both be well-formed on the success
	 * branch; the guard exists so a future shape change does not
	 * crash the success handler. */
	Ev::Io<void>
	raise_attribution(Jsmn::Object askrene_path,
			  Jsmn::Object waitsendpay_result) {
		try {
			if (!askrene_path.is_array()
			 || askrene_path.size() == 0)
				return Ev::lift();
			auto first = askrene_path[std::size_t(0)];
			auto last =
			    askrene_path[askrene_path.size() - 1];
			if (!first.is_object() || !first.has("node_id_out")
			 || !last.is_object()  || !last.has("node_id_in"))
				return Ev::lift();
			if (!waitsendpay_result.is_object()
			 || !waitsendpay_result.has("amount_msat")
			 || !waitsendpay_result.has("amount_sent_msat"))
				return Ev::lift();

			auto src = Ln::NodeId(
			    std::string(first["node_id_out"]));
			auto dst = Ln::NodeId(
			    std::string(last["node_id_in"]));
			auto amount_moved = Ln::Amount::object(
			    waitsendpay_result["amount_msat"]);
			auto amount_sent = Ln::Amount::object(
			    waitsendpay_result["amount_sent_msat"]);
			if (amount_sent < amount_moved)
				return Ev::lift();
			auto fee_spent = amount_sent - amount_moved;

			return bus.raise(Msg::XRebalanceAttribution{
				src, dst, amount_moved, fee_spent
			});
		} catch (std::exception const&) {
			return Ev::lift();
		}
	}

	/* Trim xrebalance-layer constraints older than
	 * aging_window_secs.  Modeled on FundsMover's age_clboss_layer
	 * (Boss/Mod/FundsMover/Main.cpp).  No self-loop guard refresh
	 * here -- xrebalance layer does not carry a self disable_node
	 * entry (its ephemeral per-request masking already excludes
	 * non-source/non-dest us-channels).
	 *
	 * RpcError taxonomy matches FundsMover: JSON-RPC -32601
	 * (method not found) stays Debug for graceful degradation on
	 * CLN < v24.11 where askrene-age is absent; any other code is
	 * promoted to Warn since a sustained aging failure lets stale
	 * pessimism accumulate.
	 *
	 * channel_updates left to refresh-on-failure overwrite: askrene-
	 * age intentionally skips layer->local_updates, so this RPC
	 * only trims `constraints` written by inform_channel_*.  Policy
	 * overrides written via askrene-update-channel refresh
	 * themselves whenever a fresh failure carries a new
	 * channel_update payload (gossmap_local_updatechan merges).
	 */
	Ev::Io<void> age_xrebalance_layer() {
		auto aged_time = std::make_shared<std::uint64_t>(0);
		return Ev::lift().then([this, aged_time]() {
			*aged_time = std::uint64_t(std::time(nullptr));
			/* Clamp: a misconfigured huge age window must not
			 * underflow the cutoff and wipe the whole layer. */
			auto cutoff = aging_window_secs >= *aged_time
				    ? std::uint64_t(0)
				    : *aged_time - aging_window_secs;
			auto parms = Json::Out()
				.start_object()
					.field( "layer"
					      , Boss::Mod::AskreneLayer::
						    xrebalance_layer_name
					      )
					.field("cutoff", cutoff)
				.end_object()
				;
			return rpc->command( "askrene-age"
					   , std::move(parms)
					   );
		}).then([this](Jsmn::Object res) {
			auto removed = std::uint64_t(0);
			if (res.has("num_removed")
			 && res["num_removed"].is_number())
				removed = std::uint64_t(double(res["num_removed"]));
			return Boss::log( bus, Debug
					, "XMoveFunds: askrene-age (%s) "
					  "removed %" PRIu64 " stale entries."
					, Boss::Mod::AskreneLayer::
					      xrebalance_layer_name
					      .c_str()
					, removed
					);
		}).catching<RpcError>([this](RpcError const& e) {
			auto code = int(0);
			if (e.error.has("code") && e.error["code"].is_number())
				code = int(double(e.error["code"]));
			auto is_method_missing = (code == -32601);
			return Boss::log( bus
					, is_method_missing ? Debug : Warn
					, "XMoveFunds: askrene-age (%s) "
					  "failed: %s%s"
					, Boss::Mod::AskreneLayer::
					      xrebalance_layer_name
					      .c_str()
					, Util::stringify(e.error).c_str()
					, is_method_missing
						? " (RPC missing; aging "
						  "unavailable on this CLN)."
						: " (unexpected; stale "
						  "entries will accumulate "
						  "until next successful "
						  "aging pass)."
					);
		}).then([this, aged_time]() {
			/* End-of-expiration-cycle hook: the persistence
			 * forecaster (XRebalancePredictor) re-asserts
			 * durable knowledge for directions the trim just
			 * left without live evidence.  Raised on the
			 * failure path too -- a skipped trim only means
			 * stale entries remain, which is safe for
			 * subscribers.  */
			auto m = Msg::XRebalanceLayerAged{
				*aged_time,
				*aged_time - aging_window_secs};
			return bus.raise(std::move(m));
		});
	}

	/* Gate command handling on startup completion: rpc must have
	 * arrived via Msg::Init, and create_xrebalance_layer() must
	 * have completed (either successfully or via the logged-
	 * RpcError graceful-degradation path). */
	Ev::Io<void> wait_for_ready() {
		return Ev::lift().then([this]() {
			if (!rpc || !layer_ready)
				return Ev::yield() + wait_for_ready();
			return Ev::lift();
		});
	}

	/* Fetch our channels via listpeerchannels.  Returns the
	 * "channels" array. */
	Ev::Io<Jsmn::Object> list_my_channels() {
		auto parms = Json::Out()
			.start_object()
			.end_object()
			;
		return rpc->command( "listpeerchannels"
				   , std::move(parms)
				   ).then([](Jsmn::Object res) {
			return Ev::lift(res["channels"]);
		});
	}

	/* Create a transient (persistent=false) askrene layer.  Used
	 * for the per-request mask state. */
	Ev::Io<void> create_transient_layer(std::string layer) {
		auto parms = Json::Out()
			.start_object()
				.field("layer", layer)
				.field("persistent", false)
			.end_object()
			;
		return rpc->command( "askrene-create-layer"
				   , std::move(parms)
				   ).then([](Jsmn::Object _) {
			return Ev::lift();
		});
	}

	/* Remove a transient askrene layer.  Best-effort: any RpcError
	 * is logged but swallowed because we may be on a cleanup path
	 * after some other failure and the caller has already given
	 * up. */
	Ev::Io<void> remove_layer(std::string layer) {
		auto parms = Json::Out()
			.start_object()
				.field("layer", layer)
			.end_object()
			;
		return rpc->command( "askrene-remove-layer"
				   , std::move(parms)
				   ).then([](Jsmn::Object _) {
			return Ev::lift();
		}).catching<RpcError>([this, layer](RpcError const& e) {
			return Boss::log( bus, Debug
					, "XMoveFunds: askrene-remove-"
					  "layer (%s) failed: %s "
					  "(non-fatal, ignored)"
					, layer.c_str()
					, Util::stringify(e.error).c_str()
					);
		});
	}

	/* Compute the direction (0 or 1) corresponding to "us
	 * sending into this channel" per BOLT 7 canonical ordering:
	 * direction 0 is the lower-id node as sender, direction 1 is
	 * the higher-id node as sender. */
	std::uint32_t us_to_peer_dir(Ln::NodeId const& peer) const {
		return self_id < peer ? 0 : 1;
	}
	std::uint32_t peer_to_us_dir(Ln::NodeId const& peer) const {
		return self_id < peer ? 1 : 0;
	}

	/* For each of our channels, decide whether each direction
	 * should be masked off in the transient layer, and return a
	 * chained Ev::Io<void> that writes all the masks
	 * sequentially. */
	Ev::Io<void>
	write_masks(std::string layer,
		    Jsmn::Object channels,
		    Params const& p) {
		auto source_set = std::set<std::string>();
		for (auto const& s : p.source_scids)
			source_set.insert(std::string(s));
		auto dest_set = std::set<std::string>();
		for (auto const& s : p.dest_scids)
			dest_set.insert(std::string(s));

		auto chain = Ev::lift();
		auto count = std::size_t(0);

		for (auto i = std::size_t(0); i < channels.size(); ++i) {
			auto ch = channels[i];
			if (!ch.has("state")
			    || std::string(ch["state"])
			        != "CHANNELD_NORMAL")
				continue;
			if (!ch.has("short_channel_id")
			    || !ch.has("peer_id"))
				continue;
			auto scid_str =
			    std::string(ch["short_channel_id"]);
			auto scid = Ln::Scid(scid_str);
			auto peer = Ln::NodeId(
			    std::string(ch["peer_id"]));

			auto disable_dir =
			    [this, layer]
			    (Ln::Scid s, std::uint32_t dir) {
				return Boss::Mod::AskreneLayer::
				    update_channel(
					*rpc, layer, s, dir,
					/* enabled = */ false,
					Ln::Amount::msat(0),
					Ln::Amount::msat(0),
					Ln::Amount::msat(0),
					/* fee_prop = */ 0,
					/* cltv = */ 0);
			};

			if (!source_set.count(scid_str)) {
				chain = std::move(chain)
				      + disable_dir(scid,
						    us_to_peer_dir(peer));
				++count;
			}
			if (!dest_set.count(scid_str)) {
				chain = std::move(chain)
				      + disable_dir(scid,
						    peer_to_us_dir(peer));
				++count;
			}
		}

		return std::move(chain)
		     + Boss::log( bus, Debug
				, "XMoveFunds: wrote %zu mask "
				  "entries to transient layer %s"
				, count
				, layer.c_str()
				);
	}

	/* Given the askrene route's last visible hop (whose
	 * node_id_out is the fill peer the cycle terminates at),
	 * locate the dest_scid that connects us to that peer.  Used
	 * to find the channel for the closing hop of the circular
	 * cycle. */
	std::string
	find_fill_scid(Jsmn::Object const& channels,
		       std::set<std::string> const& dest_set,
		       Ln::NodeId const& fill_peer) {
		for (auto i = std::size_t(0); i < channels.size(); ++i) {
			auto ch = channels[i];
			if (!ch.has("short_channel_id")
			    || !ch.has("peer_id"))
				continue;
			auto scid_str =
			    std::string(ch["short_channel_id"]);
			if (!dest_set.count(scid_str))
				continue;
			auto peer = Ln::NodeId(
			    std::string(ch["peer_id"]));
			if (peer == fill_peer)
				return scid_str;
		}
		throw std::runtime_error(
			"could not find a dest_scid matching the "
			"askrene route's last fill peer "
			+ std::string(fill_peer));
	}

	/* Convert an askrene path hop into a sendpay-format hop
	 * object.  See Boss/Mod/FundsMover/Attempter.cpp::make_route
	 * for the field convention. */
	Json::Out
	askrene_hop_to_sendpay(Jsmn::Object const& hop) {
		auto scidd = std::string(hop["short_channel_id_dir"]);
		auto slash = scidd.find('/');
		auto scid_str = scidd.substr(0, slash);
		auto dir = std::uint32_t(
		    std::stoul(scidd.substr(slash + 1)));
		auto amount_out =
		    Ln::Amount::object(hop["amount_out_msat"]);
		auto cltv_out = std::uint32_t(
		    double(hop["cltv_out"]));
		return Json::Out()
			.start_object()
				.field("id",
				       std::string(hop["node_id_out"]))
				.field("channel", scid_str)
				.field("direction", dir)
				.field("amount_msat",
				       amount_out.to_msat())
				.field("delay", cltv_out)
				.field("style", std::string("tlv"))
			.end_object()
			;
	}

	/* Build the full sendpay route array for one askrene route.
	 *
	 * The patched askrene (branch circular-askrene4) splices a
	 * fake destination node (circular_fake_us_in_id) plus mirror
	 * channels onto it before running MCF, then returns the
	 * complete s -> t flow WITHOUT stripping the trailing fake
	 * mirror hop.  The last hop of every circular-mode route is
	 * therefore the fake mirror: synthetic scid, node_id_out =
	 * the fake destination, but amount_in_msat /
	 * amount_out_msat / cltv_in / cltv_out all computed with
	 * fill_peer's actual policy (so amount_in - amount_out =
	 * fill_peer's real fee).
	 *
	 * For each real network hop in path[0..N-2] we just copy as
	 * sendpay format.  For the last hop (path[N-1] = fake
	 * mirror) we REPLACE its identity fields with the caller's
	 * chosen real closing channel + our self_id, but KEEP the
	 * mirror's amount_msat and delay -- those values came out of
	 * MCF accounting for fill_peer's fee and CLTV delta and are
	 * exactly what CLN needs for the closing onion hop.
	 *
	 * The earlier strip-then-append design used route.amount_msat
	 * (= the pre-fee amount fill_peer received) as the closing
	 * hop amount_msat, which meant we offered 0 fee to fill_peer
	 * and got WIRE_FEE_INSUFFICIENT on every retry once fill_peer
	 * was charging anything.  Replacing the mirror in-place fixes
	 * the math without requiring callers to know about the fake
	 * scid or to look up fill_peer's policy themselves. */
	Json::Out
	build_sendpay_route(Jsmn::Object const& askrene_route,
			    std::string const& fill_scid,
			    Ln::NodeId const& fill_peer) {
		auto ret = Json::Out();
		auto arr = ret.start_array();
		auto path = askrene_route["path"];
		auto last_idx = path.size() - 1;
		for (auto i = std::size_t(0); i < path.size(); ++i) {
			if (i == last_idx) {
				/* Replace the fake mirror with the
				 * real closing hop, keeping the
				 * mirror's MCF-computed amounts and
				 * delays. */
				auto hop = path[i];
				auto amount_out = Ln::Amount::object(
				    hop["amount_out_msat"]);
				auto cltv_out = std::uint32_t(
				    double(hop["cltv_out"]));
				arr.start_object()
						.field("id",
						       std::string(self_id))
						.field("channel", fill_scid)
						.field("direction",
						       peer_to_us_dir(
							   fill_peer))
						.field("amount_msat",
						       amount_out.to_msat())
						.field("delay", cltv_out)
						.field("style",
						       std::string("tlv"))
					.end_object();
			} else {
				arr.entry(askrene_hop_to_sendpay(path[i]));
			}
		}
		arr.end_array();
		return ret;
	}

	/* Issue one sendpay for one part of the (possibly MPP)
	 * payment.  partid=0 indicates a non-MPP single-part call;
	 * partid>=1 indicates one part of an MPP group identified
	 * by groupid, with total_msat declaring the sum across all
	 * parts of the group. */
	Ev::Io<Jsmn::Object>
	sendpay_part(Sha256::Hash const& payment_hash,
		     Ln::Preimage const& payment_secret,
		     Json::Out route,
		     std::string const& label,
		     std::uint64_t groupid,
		     std::uint64_t partid,
		     Ln::Amount total_msat) {
		auto parms = Json::Out();
		auto obj = parms.start_object();
		obj.field("route", std::move(route));
		obj.field("payment_hash",
			  std::string(payment_hash));
		obj.field("label", label);
		obj.field("payment_secret",
			  std::string(payment_secret));
		if (partid > 0) {
			obj.field("partid", partid);
			obj.field("groupid", groupid);
			obj.field("amount_msat",
				  total_msat.to_msat());
		}
		obj.end_object();
		return rpc->command("sendpay", std::move(parms));
	}

	/* Wait for one part to terminate (success or hard
	 * failure).  Mirrors the partid/groupid distinction from
	 * sendpay_part. */
	Ev::Io<Jsmn::Object>
	waitsendpay_part(Sha256::Hash const& payment_hash,
			 std::uint64_t partid,
			 std::uint64_t groupid) {
		auto parms = Json::Out();
		auto obj = parms.start_object();
		obj.field("payment_hash",
			  std::string(payment_hash));
		if (partid > 0) {
			obj.field("partid", partid);
			obj.field("groupid", groupid);
		}
		obj.end_object();
		return rpc->command("waitsendpay", std::move(parms));
	}

	/* Best-effort delpay for one failed sendpay part.  Keeps CLN's
	 * payment store from accumulating dead payment_hash entries
	 * across heavy testing sessions -- mirrors FundsMover/
	 * Attempter::delpay (Boss/Mod/FundsMover/Attempter.cpp:1570).
	 *
	 * MPP: pass partid + groupid together so delpay targets just
	 * this part.  Non-MPP (partid=0): pass neither and let CLN
	 * delete the only entry by payment_hash.  Per CLN's delpay
	 * schema you must set both or neither -- mixing them returns
	 * "Must set both partid and groupid, or neither" from
	 * cln/lightningd/pay.c:2344.
	 *
	 * Swallow RpcError: delpay can legitimately fail with
	 * PAY_NO_SUCH_PAYMENT (the part never made it past sendpay so
	 * CLN never recorded it) or hit any transient error, neither
	 * of which we want surfaced -- the rebalance itself is
	 * already failed and reported via err_msgs. */
	Ev::Io<void>
	delpay_part(Sha256::Hash payment_hash,
		    std::uint64_t partid,
		    std::uint64_t groupid) {
		auto parms = Json::Out();
		auto obj = parms.start_object();
		obj.field("payment_hash", std::string(payment_hash));
		obj.field("status", std::string("failed"));
		if (partid > 0) {
			obj.field("partid", partid);
			obj.field("groupid", groupid);
		}
		obj.end_object();
		return rpc->command("delpay", std::move(parms)
		).then([](Jsmn::Object _) {
			return Ev::lift();
		}).catching<RpcError>([](RpcError const&) {
			return Ev::lift();
		});
	}

	/* Inspect a waitsendpay RpcError and append the appropriate
	 * persistent-layer feedback action to `actions`.  No-op if:
	 *   - the error code is not 204 (only sendpay routing
	 *     failures carry erring_* fields);
	 *   - the embedded data is missing or malformed;
	 *   - we fall through to inform_channel_constrained and
	 *     erring_channel is one of our local-channel scids
	 *     (auto.localchans is authoritative for capacity).
	 *
	 * Failcode dispatch:
	 *   - failcode & 0x2000 (NODE-level): append a disable_node
	 *     action on the persistent xrebalance layer.
	 *   - failcode is one of 0x100b/0x100c/0x100d/0x100e and
	 *     raw_message parses as a channel_update: append an
	 *     update_channel action with the refreshed policy
	 *     fields.  This is the FundsMover/Attempter-style
	 *     "channel_update refresh" branch.  Critically, this
	 *     write applies REGARDLESS of whether erring_channel is
	 *     one of our local scids -- the failing direction is
	 *     always the FORWARDER's direction (their outbound to
	 *     the next hop, or to us at the closing hop), which is
	 *     gossip-derived even on our own channels.
	 *     auto.localchans only authoritatively covers our own
	 *     outbound direction; the peer's direction comes from
	 *     gossip and is what gets stale.  Without this branch,
	 *     a fee_insufficient at the closing hop of our circular
	 *     payment loops forever with no learning -- exactly
	 *     what we observed on signet the first time askrene chose
	 *     a route through a forwarder whose published fee was
	 *     out of date.
	 *   - otherwise (channel-level, including 0x1007 TCF):
	 *     append inform_channel_constrained with the
	 *     amount the failing hop was being asked to push.
	 *     Askrene stores this as max_msat = amount - 1, so the
	 *     channel still appears usable for strictly smaller
	 *     payments but is excluded for routes carrying the
	 *     failing amount or more.  This is the same gradient
	 *     signal xpay and FundsMover/Attempter write -- it lets
	 *     askrene's probability estimate distinguish "channel
	 *     can't push 800m" from "channel is dead".
	 *
	 *     The per-hop amount is recovered from the askrene path
	 *     by indexing with erring_index.  CLN's erring_index is
	 *     the 0-based position in the sendpay route, which is the
	 *     askrene path with its fake last hop rewritten in place
	 *     into the real closing hop -- so the sendpay route and
	 *     the askrene path are index-aligned over [0,
	 *     askrene_path.size()).  Position K
	 *     in the askrene path is the K-th forwarding edge;
	 *     amount_in_msat at that hop is what gets pushed INTO
	 *     the failing channel.
	 *
	 *     Fallback to amount=1 (full exclusion) if the path
	 *     lookup fails -- a strictly safer signal than no
	 *     feedback at all.
	 *
	 * Mirrors the simpler half of FundsMover/Attempter.cpp's
	 * 204 handling; we deliberately skip the
	 * channel_update-refresh branch (parse_chan_update +
	 * update_channel with policy fields) for now -- the manual
	 * xmovefunds primitive does not yet retry, so the inform-
	 * constrained path alone is sufficient to make the NEXT
	 * manual invocation pick a different route. */
	/* Concise one-line failure summary for the response's errors[]
	 * (and thus the XRebalancer "reason:" log line, so a single
	 * grep XRebalancer shows how close the part got).  For a 204 it
	 * carries from_target (hops short of delivery, 1 = the closing
	 * hop itself), the failcode and the erring node; anything else
	 * falls back to the stringified error. */
	std::string
	failure_summary( RpcError const& e
		       , Jsmn::Object const& askrene_path
		       ) {
		try {
			auto const& error = e.error;
			if (error.is_object()
			 && error.has("code") && error["code"].is_number()
			 && int(double(error["code"])) == 204
			 && error.has("data")) {
				auto data = error["data"];
				if (data.has("erring_index")
				 && data.has("erring_node")
				 && data.has("failcode")) {
					auto eidx = std::size_t(double(
					    data["erring_index"]));
					/* build_sendpay_route rewrites the fake
					 * last hop in place, so the sendpay route
					 * is exactly askrene_path.size() hops and
					 * the closing (delivery) hop sits at index
					 * size()-1.  from_target=1 is that closing
					 * hop. */
					auto route_hops =
					    askrene_path.size();
					auto from_target =
					    (eidx < route_hops)
					    ? (route_hops - eidx)
					    : std::size_t(0);
					auto os = std::ostringstream();
					os << "204 from_target=" << from_target
					   << " failcode=0x" << std::hex
					   << std::uint16_t(double(
						data["failcode"]))
					   << std::dec << " node="
					   << std::string(data["erring_node"]);
					return os.str();
				}
			}
		} catch (std::exception const&) {
			/* fall through to the verbose form */
		}
		return "waitsendpay: " + Util::stringify(e.error);
	}

	void accumulate_failure_feedback(
	    RpcError const& e,
	    std::set<std::string> const& our_scids,
	    Jsmn::Object const& askrene_path,
	    std::vector<Ev::Io<void>>& actions) {
		try {
			auto const& error = e.error;
			if (!error.has("code") || !error["code"].is_number())
				return;
			auto code = int(double(error["code"]));
			if (code != 204)
				return;
			if (!error.has("data"))
				return;
			auto data = error["data"];
			if (!data.has("erring_channel")
			 || !data.has("erring_direction")
			 || !data.has("erring_node")
			 || !data.has("erring_index")
			 || !data.has("failcode"))
				return;
			auto echan_str =
			    std::string(data["erring_channel"]);
			auto edir = std::uint32_t(double(
			    data["erring_direction"]));
			auto enode = Ln::NodeId(
			    std::string(data["erring_node"]));
			auto eidx = std::size_t(double(
			    data["erring_index"]));
			auto fail = std::uint16_t(double(
			    data["failcode"]));

			/* Concise one-line Info summary per 204 (the full per-hop
			 * table below is logged at Debug to avoid flooding Info on
			 * a probe-heavy rebalance). */
			{
				auto alloc = std::uint64_t(0);
				if (eidx < askrene_path.size()
				 && askrene_path[eidx].has("amount_in_msat")
				 && askrene_path[eidx].has("amount_out_msat")) {
					auto in = Ln::Amount::object(
					    askrene_path[eidx]["amount_in_msat"]).to_msat();
					auto out = Ln::Amount::object(
					    askrene_path[eidx]["amount_out_msat"]).to_msat();
					alloc = (in >= out) ? in - out : std::uint64_t(0);
				}
				/* How many hops short of delivery the failure was:
				 * build_sendpay_route rewrites the fake last hop
				 * of the askrene path in place into the real
				 * closing hop back to us, so the sendpay route is
				 * exactly askrene_path.size() hops and the final
				 * (delivery) hop is at index size()-1.
				 * from_target=1 means that closing hop itself
				 * failed (as close as it gets); 5 means it died 5
				 * hops from completing. */
				auto route_hops = askrene_path.size();
				auto from_target = (eidx < route_hops)
						 ? (route_hops - eidx)
						 : std::size_t(0);
				auto sum = std::ostringstream();
				sum << "XMoveFunds: 204 failcode=0x" << std::hex << fail
				    << std::dec << " erring=" << echan_str << "/" << edir
				    << " node=" << std::string(enode)
				    << " from_target=" << from_target
				    << " alloc_fee="
				    << Util::Str::group_digits(alloc)
				    << "msat";
				ChanUpdate scu;
				if (data.has("raw_message")
				 && eidx < askrene_path.size()
				 && askrene_path[eidx].has("amount_out_msat")
				 && parse_chan_update(
					std::string(data["raw_message"]), scu)) {
					auto out = Ln::Amount::object(
					    askrene_path[eidx]["amount_out_msat"]).to_msat();
					auto req_out = std::uint64_t(scu.fee_base_msat)
					    + std::uint64_t(scu.fee_proportional_millionths)
					      * out / 1000000;
					sum << " required_out="
					    << Util::Str::group_digits(req_out)
					    << "msat";
					if (scu.has_inbound_fee)
						sum << " inbound_ppm="
						    << scu.inbound_fee_proportional_millionths;
				}
				actions.push_back(Boss::log( bus, Info
							   , "%s"
							   , sum.str().c_str()));
			}

			/* Diagnostic: dump the full per-hop fee picture so a
			 * FEE_INSUFFICIENT can be dissected -- allocated fee at
			 * the erring (outgoing) hop vs the required outbound
			 * fee from the embedded channel_update, plus a pointer
			 * at the PRECEDING/incoming hop where any inbound fee
			 * actually lives (it is NOT in this payload). */
			{
				auto os = std::ostringstream();
				os << "XMoveFunds: 204 picture failcode=0x"
				   << std::hex << fail << std::dec
				   << " erring_index=" << eidx
				   << " erring_channel=" << echan_str
				   << "/" << edir
				   << " erring_node=" << std::string(enode)
				   << "\n" << format_route_fees(askrene_path);
				if (eidx < askrene_path.size()) {
					auto h = askrene_path[eidx];
					auto out = h.has("amount_out_msat")
					    ? Ln::Amount::object(
						h["amount_out_msat"]).to_msat()
					    : std::uint64_t(0);
					auto in = h.has("amount_in_msat")
					    ? Ln::Amount::object(
						h["amount_in_msat"]).to_msat()
					    : std::uint64_t(0);
					auto alloc = (in >= out) ? in - out
						   : std::uint64_t(0);
					os << "  erring hop[" << eidx
					   << "] allocated fee="
					   << Util::Str::group_digits(alloc)
					   << "msat\n";
					if (data.has("raw_message")) {
						ChanUpdate dcu;
						if (parse_chan_update(
						    std::string(
							data["raw_message"]),
						    dcu)) {
							auto req_out =
							    std::uint64_t(
								dcu.fee_base_msat)
							  + std::uint64_t(
								dcu.fee_proportional_millionths)
							    * out / 1000000;
							os << "    erring-channel"
							      " policy: out_base="
							   << dcu.fee_base_msat
							   << " out_ppm="
							   << dcu.fee_proportional_millionths
							   << " -> required_out="
							   << Util::Str::
								group_digits(
								    req_out)
							   << "msat";
							if (dcu.has_inbound_fee)
								os << "; inbound_base="
								   << dcu.inbound_fee_base_msat
								   << " inbound_ppm="
								   << dcu.inbound_fee_proportional_millionths;
							os << "\n";
						}
					}
				}
				if (eidx >= 1 && eidx - 1 < askrene_path.size()) {
					auto pre = askrene_path[eidx - 1];
					os << "  preceding/incoming hop["
					   << (eidx - 1) << "] "
					   << (pre.has("short_channel_id_dir")
					       ? std::string(
						   pre["short_channel_id_dir"])
					       : std::string("?"))
					   << " (any inbound fee that bit us"
					      " lives HERE, not in the payload"
					      " above)\n";
				}
				actions.push_back(Boss::log( bus, Debug
							   , "%s"
							   , os.str().c_str()));
			}

			/* Positive reinforcement from a FAILED part: every
			 * hop strictly before the failure point forwarded
			 * the HTLC, proving liquidity >= the carried amount
			 * at that moment -- evidence as strong as a settled
			 * part's (stronger for floors, even: the unwind put
			 * the liquidity back).  Inform unconstrained and
			 * record a Transit observation per such hop.
			 *
			 * For 0x100c the incoming hop route[eidx-1] may
			 * itself be blamed below (inbound-fee PolicyFail
			 * exclusion), so stop one hop short on ALL 0x100c:
			 * never claim "carried fine" and "excluded" about
			 * the same hop from the same part.  The
			 * stale-outbound 0x100c sub-case under-logs that
			 * one hop's transit; acceptable.  */
			{
				auto transit_end =
				    std::min(eidx, askrene_path.size());
				if (fail == 0x100c && transit_end >= 1)
					--transit_end;
				for (auto j = std::size_t(0);
				     j < transit_end; ++j) {
					auto hop = askrene_path[j];
					if (!hop.has("short_channel_id_dir")
					 || !hop.has("amount_out_msat"))
						continue;
					auto scidd = std::string(
					    hop["short_channel_id_dir"]);
					auto slash = scidd.find('/');
					auto scid_str = scidd.substr(0, slash);
					/* auto.localchans owns our own
					 * channels' truth.  */
					if (our_scids.count(scid_str))
						continue;
					auto dir = std::uint32_t(std::stoul(
					    scidd.substr(slash + 1)));
					/* amount_out_msat = what the hop
					 * forwarded; same lower-bound claim
					 * the settled-part branch makes.  */
					auto amt = Ln::Amount::object(
					    hop["amount_out_msat"]);
					actions.push_back(
					    Boss::Mod::AskreneLayer::
						inform_channel_unconstrained(
						*rpc,
						Boss::Mod::AskreneLayer::
						    xrebalance_layer_name,
						Ln::Scid(scid_str), dir,
						amt));
					auto obs = Msg::XRebalanceObservation{
						std::uint64_t(
						    std::time(nullptr)),
						Ln::Scid(scid_str), dir,
						Msg::XRebalanceObservationKind
						    ::Transit,
						amt, 0, Ln::NodeId()};
					actions.push_back(
					    bus.raise(std::move(obs)));
				}
			}

			if (fail & 0x2000) {
				/* NODE-level failure: take the whole forwarder
				 * out of consideration -- but NEVER our own node.
				 * In a circular rebalance the destination is us,
				 * so a node-level failure at the closing hop is
				 * attributed to self; disable_node(self) would
				 * disable ALL our channels as the source (askrene:
				 * "source has disabled N of N channels, capacity
				 * 0msat") and lock out every future rebalance --
				 * permanently, since disabled_nodes does not age.
				 * Mirrors FundsMover's self_id guard. */
				if (std::string(enode) == std::string(self_id))
					return;
				actions.push_back(
				    Boss::Mod::AskreneLayer::disable_node(
					*rpc,
					Boss::Mod::AskreneLayer::
					    xrebalance_layer_name,
					enode));
				auto node_amount = Ln::Amount::msat(1);
				if (eidx < askrene_path.size()
				 && askrene_path[eidx].has("amount_in_msat"))
					node_amount = Ln::Amount::object(
					    askrene_path[eidx]
						["amount_in_msat"]);
				auto obs = Msg::XRebalanceObservation{
					std::uint64_t(std::time(nullptr)),
					Ln::Scid(echan_str), edir,
					Msg::XRebalanceObservationKind
					    ::NodeFail,
					node_amount, fail, enode};
				actions.push_back(bus.raise(std::move(obs)));
				return;
			}

			/* FEE_INSUFFICIENT (0x100c): the required fee at the
			 * erring node is outbound_fee(erring/OUTGOING channel) +
			 * inbound_fee(INCOMING channel).  The error names and
			 * carries the policy for the OUTGOING channel only, so we
			 * must decide which side is actually at fault:
			 *   - if we already paid the outgoing channel's advertised
			 *     outbound fee (allocated >= required_out) yet still
			 *     failed, the shortfall is the INBOUND fee on the
			 *     incoming channel route[eidx-1] -> hard-exclude that
			 *     channel (a max_msat=0 constraint that self-ages, so it
			 *     recovers if the peer later drops the inbound fee);
			 *   - if we underpaid the outbound fee (allocated <
			 *     required_out), it is a stale outbound policy -> refresh
			 *     the outgoing channel from the embedded channel_update;
			 *   - no channel_update to compare -> treat as the inbound
			 *     case (a plain stale-outbound failure normally carries
			 *     the update) and exclude the incoming channel.
			 * NB: the OUTGOING channel's own inbound-fee TLV is NOT the
			 * fee that bit us (that is the incoming channel's), so we no
			 * longer key off it. */
			if (fail == 0x100c) {
				/* fee we allocated at the erring (outgoing) hop */
				auto alloc = std::uint64_t(0);
				if (eidx < askrene_path.size()
				 && askrene_path[eidx].has("amount_in_msat")
				 && askrene_path[eidx].has("amount_out_msat")) {
					auto in = Ln::Amount::object(
					    askrene_path[eidx]["amount_in_msat"]).to_msat();
					auto out = Ln::Amount::object(
					    askrene_path[eidx]["amount_out_msat"]).to_msat();
					alloc = (in >= out) ? in - out : std::uint64_t(0);
				}
				ChanUpdate cu;
				auto have_cu = data.has("raw_message")
				    && eidx < askrene_path.size()
				    && askrene_path[eidx].has("amount_out_msat")
				    && parse_chan_update(
					std::string(data["raw_message"]), cu);
				auto outbound_satisfied = true;
				if (have_cu) {
					auto out = Ln::Amount::object(
					    askrene_path[eidx]["amount_out_msat"]).to_msat();
					auto required_out =
					    std::uint64_t(cu.fee_base_msat)
					  + std::uint64_t(
						cu.fee_proportional_millionths)
					    * out / 1000000;
					outbound_satisfied = (alloc >= required_out);
				}
				/* Inbound-fee case: exclude the INCOMING channel
				 * route[eidx-1], unless it is one of our own channels
				 * (auto.localchans owns those; never self-exclude). */
				if (outbound_satisfied && eidx >= 1
				 && (eidx - 1) < askrene_path.size()
				 && askrene_path[eidx - 1].has("short_channel_id_dir")) {
					auto scidd = std::string(
					    askrene_path[eidx - 1]["short_channel_id_dir"]);
					auto slash = scidd.find('/');
					auto in_scid = scidd.substr(0, slash);
					if (!our_scids.count(in_scid)) {
						auto in_dir = std::uint32_t(std::stoul(
						    scidd.substr(slash + 1)));
						actions.push_back(
						    Boss::Mod::AskreneLayer::
							inform_channel_constrained(
							*rpc,
							Boss::Mod::AskreneLayer::
							    xrebalance_layer_name,
							Ln::Scid(in_scid), in_dir,
							Ln::Amount::msat(1)));
						auto obs = Msg::
						    XRebalanceObservation{
							std::uint64_t(
							    std::time(nullptr)),
							Ln::Scid(in_scid),
							in_dir,
							Msg::
							XRebalanceObservationKind
							    ::PolicyFail,
							Ln::Amount::msat(1),
							fail, enode};
						actions.push_back(
						    bus.raise(std::move(obs)));
						return;
					}
				}
				/* Stale outbound fee: refresh the outgoing channel. */
				if (have_cu) {
					actions.push_back(
					    Boss::Mod::AskreneLayer::update_channel(
						*rpc,
						Boss::Mod::AskreneLayer::
						    xrebalance_layer_name,
						Ln::Scid(echan_str), edir,
						cu.enabled,
						Ln::Amount::msat(cu.htlc_minimum_msat),
						Ln::Amount::msat(cu.htlc_maximum_msat),
						Ln::Amount::msat(cu.fee_base_msat),
						cu.fee_proportional_millionths,
						cu.cltv_expiry_delta));
					return;
				}
				/* else: fall through to the capacity constraint. */
			}

			/* Other policy-carrying failcodes (amount_below_minimum,
			 * incorrect_cltv_expiry, expiry_too_soon) genuinely concern
			 * the OUTGOING channel's published policy -- refresh it from
			 * the embedded channel_update. */
			auto policy_carrying =
			       fail == 0x100b
			    || fail == 0x100d
			    || fail == 0x100e;
			if (policy_carrying && data.has("raw_message")) {
				auto raw =
				    std::string(data["raw_message"]);
				ChanUpdate cu;
				if (parse_chan_update(raw, cu)) {
					actions.push_back(
					    Boss::Mod::AskreneLayer::
						update_channel(
						*rpc,
						Boss::Mod::AskreneLayer::
						    xrebalance_layer_name,
						Ln::Scid(echan_str), edir,
						cu.enabled,
						Ln::Amount::msat(
						    cu.htlc_minimum_msat),
						Ln::Amount::msat(
						    cu.htlc_maximum_msat),
						Ln::Amount::msat(
						    cu.fee_base_msat),
						cu.fee_proportional_millionths,
						cu.cltv_expiry_delta));
					return;
				}
				/* Fall through to inform-constrained
				 * fallback if parse failed. */
			}

			/* Channel-level failure (or unparseable
			 * policy-carrying failure): skip if it is one
			 * of our local channels.  auto.localchans owns
			 * the capacity truth on our own outbound. */
			if (our_scids.count(echan_str))
				return;

			/* Look up the per-hop amount entering the
			 * failing channel.  Fall back to 1 msat (full
			 * exclusion) if anything is unparseable -- a
			 * conservative signal beats no signal. */
			auto constraint_amount = Ln::Amount::msat(1);
			if (eidx < askrene_path.size()
			 && askrene_path[eidx].has("amount_in_msat")) {
				constraint_amount = Ln::Amount::object(
				    askrene_path[eidx]["amount_in_msat"]);
			}

			actions.push_back(
			    Boss::Mod::AskreneLayer::
				inform_channel_constrained(
				    *rpc,
				    Boss::Mod::AskreneLayer::
					xrebalance_layer_name,
				    Ln::Scid(echan_str), edir,
				    constraint_amount));
			auto obs = Msg::XRebalanceObservation{
				std::uint64_t(std::time(nullptr)),
				Ln::Scid(echan_str), edir,
				Msg::XRebalanceObservationKind
				    ::LiquidityFail,
				constraint_amount, fail, enode};
			actions.push_back(bus.raise(std::move(obs)));
		} catch (std::exception const&) {
			/* Best-effort: malformed payload just means
			 * no feedback for this part. */
		}
	}

	/* Construct a sendpay/EarningsTracker label of the shape
	 * "clboss-xrebalance-<unix-ts>" so the tracker can
	 * disambiguate xrebalance-family payments from the older
	 * FundsMover ones (which already use a "clboss"-prefixed
	 * convention). */
	std::string make_label() {
		auto t = std::time(nullptr);
		auto os = std::ostringstream();
		os << "clboss-xrebalance-" << t;
		return os.str();
	}

	/* Build and issue the askrene-getroutes call with
	 * source = destination = self_id (the patched askrene
	 * interprets this as circular self-rebalance routing).
	 * Includes auto.localchans, the persistent xrebalance
	 * layer, and the per-request transient layer. */
	Ev::Io<Jsmn::Object>
	call_getroutes(std::string transient, Params const& p) {
		auto parms = Json::Out();
		auto obj = parms.start_object();
		obj.field("source", std::string(self_id));
		obj.field("destination", std::string(self_id));
		obj.field("amount_msat",
			  std::uint64_t(p.amount.to_msat()));
		auto la = obj.start_array("layers");
		la.entry(std::string("auto.localchans"));
		la.entry(Boss::Mod::AskreneLayer::
			     xrebalance_layer_name);
		la.entry(transient);
		la.end_array();
		obj.field("maxfee_msat",
			  std::uint64_t(p.maxfee.to_msat()));
		obj.field("final_cltv", std::uint32_t(14));
		obj.field("maxparts", p.maxparts);
		obj.end_object();
		return rpc->command("getroutes", std::move(parms));
	}

	/* Drive the sendpay + waitsendpay sequence for one or more
	 * askrene-returned routes (multi-part for MPP).  All parts
	 * share payment_hash + payment_secret + groupid + label;
	 * each part gets a unique partid.  Each route's closing hop
	 * (fill_peer -> self_id) is appended before sendpay is
	 * called.  Returns a JSON object with per-part status. */
	Ev::Io<Json::Out>
	do_execute(std::shared_ptr<Params> p,
		   std::shared_ptr<Jsmn::Object> askrene_response,
		   std::shared_ptr<Jsmn::Object> channels) {
		auto routes = (*askrene_response)["routes"];
		auto num_parts = routes.size();
		auto multi = num_parts > 1;

		auto kp = claimer.generate();
		auto preimage = std::make_shared<Ln::Preimage>(
		    std::move(kp.first));
		auto payment_secret =
		    std::make_shared<Ln::Preimage>(std::move(kp.second));
		auto payment_hash =
		    std::make_shared<Sha256::Hash>(preimage->sha256());
		auto label =
		    std::make_shared<std::string>(make_label());
		/* For multi-part MPP we must pass an explicit groupid to
		 * sendpay (CLN requires it for parts >= 1) and CLN echoes
		 * it back verbatim.  For single-part we let CLN
		 * auto-assign one -- the value depends on what's already
		 * in the payment store for this payment_hash, so the only
		 * truthful answer is whatever sendpay returns.  We seed
		 * groupid_actual with our planned value for multi (which
		 * CLN will honor) and update it from the sendpay response
		 * for single-part. */
		auto groupid_planned = rng();
		auto groupid_actual = std::make_shared<std::uint64_t>(
		    multi ? groupid_planned : 0);
		auto results = std::make_shared<std::vector<Jsmn::Object>>();
		auto err_msgs =
		    std::make_shared<std::vector<std::string>>();

		auto dest_set = std::make_shared<std::set<std::string>>();
		for (auto const& s : p->dest_scids)
			dest_set->insert(std::string(s));

		/* Set of our local-channel scids (sources + dests), used
		 * to suppress feedback writes against our own channels.
		 * Askrene's auto.localchans layer is authoritative for
		 * local-channel state, so constraining/disabling our own
		 * channels in the persistent xrebalance layer would only
		 * poison future routing (askrene min-across-layers
		 * semantic).  Network hops are not in this set. */
		auto our_scids = std::make_shared<std::set<std::string>>();
		for (auto const& s : p->source_scids)
			our_scids->insert(std::string(s));
		for (auto const& s : p->dest_scids)
			our_scids->insert(std::string(s));

		/* Per-part askrene-path network middle hops, captured at
		 * sendpay-build time so the success branch knows which
		 * channels to positively reinforce (inform-unconstrained
		 * with their carried amount).  Stored as
		 * (scid, direction, amount).  Hops whose scid is in
		 * our_scids are filtered out -- that drops the local
		 * us->drain_peer hop at the head of every askrene path. */
		auto per_part_middle = std::make_shared<
		    std::vector<std::vector<
			std::tuple<Ln::Scid,
				   std::uint32_t,
				   Ln::Amount>>>>(num_parts);

		/* Accumulator of Ev::Io<void> feedback actions
		 * (inform_channel_*, disable_node).  Populated by the
		 * waitsendpay .then/.catching lambdas during execution
		 * and drained after the waitsendpay loop completes.
		 * Doing the writes after waitsendpay (rather than
		 * inline) keeps the per-part wait latency unaffected
		 * by the feedback RPCs and avoids interleaving
		 * RPC traffic with payment-critical sendpay/waitsendpay
		 * calls. */
		auto feedback_actions = std::make_shared<
		    std::vector<Ev::Io<void>>>();

		auto chain = Boss::log( bus, Info
				      , "XMoveFunds: executing %zu "
					"part(s); payment_hash=%s "
					"label=%s groupid_planned=%" PRIu64
				      , num_parts
				      , std::string(*payment_hash)
				          .c_str()
				      , label->c_str()
				      , groupid_planned
				      );

		/* Per-part fee discipline.  The whole-payment maxfee only
		 * bounds the AVERAGE fee rate across parts, so MCF can balance
		 * an over-budget part against cheaper ones.  But parts settle
		 * independently here (self-pay, claimed per-HTLC), so an
		 * expensive part that lands while the cheap ones fail would
		 * book a rebalance far above budget -- and even when it would
		 * not settle, sending it burns HTLCs, gossip, and constraint
		 * writes for nothing.  So we refuse up front to send any part
		 * whose own fee rate (fee / delivered) exceeds the budget ppm:
		 * the maxfee still bounds the average, this bounds each part.
		 * Skipped indices stay aligned with routes[]/per_part_middle[]
		 * (we just never sendpay or waitsendpay them). */
		auto budget_ppm = double(std::uint64_t(p->maxfee.to_msat()))
				* 1000000.0
				/ double(std::uint64_t(p->amount.to_msat()));
		auto skipped = std::vector<bool>(num_parts, false);
		auto num_skipped = std::size_t(0);

		for (auto i = std::size_t(0); i < num_parts; ++i) {
			auto route_obj = routes[i];
			auto partid = multi ? (i + 1) : 0;

			/* Build the sendpay route off the askrene
			 * route.  The patched askrene leaves a fake
			 * mirror hop at path[N-1] -- its node_id_in is
			 * the real fill peer (= the last real
			 * forwarder), its node_id_out is the synthetic
			 * circular_fake_us_in_id.  We look up the fill
			 * peer via node_id_in. */
			auto path = route_obj["path"];
			if (path.size() == 0) {
				err_msgs->push_back(
				    "askrene returned an empty path");
				continue;
			}

			/* CLN's common/sphinx.c:serialize_onionpacket can
			 * SIGSEGV when the cumulative per-hop TLV payloads
			 * exceed the fixed 1300-byte onion buffer.  Observed
			 * repeatedly in production at 24-26 hops, always with the
			 * identical sphinx backtrace; recurred 2026-06-12 on a
			 * 24-hop xmovefunds part.
			 *
			 * build_sendpay_route rewrites the fake mirror hop in
			 * place, so the sendpay route is exactly path.size()
			 * hops -- the same count CLN reports as "Sending ...
			 * over N hops".  Skip any part longer than the safe
			 * bound rather than hand it to sendpay and crash CLN;
			 * 20 hops gives a comfortable margin under the lowest
			 * observed crashing length (24).  Mirrors the same
			 * guard in Boss/Mod/FundsMover/Attempter.cpp.
			 *
			 * Remove once CLN's serialize_onionpacket validates the
			 * payload size and returns an error instead of crashing.
			 */
			{
				auto constexpr max_safe_hops =
				    std::size_t(20);
				if (path.size() > max_safe_hops) {
					skipped[i] = true;
					++num_skipped;
					chain = std::move(chain)
					      + Boss::log( bus, Info
						, "XMoveFunds: part %zu "
						  "refused: route too long: "
						  "%zu hops > %zu max "
						  "(CLN sphinx crash "
						  "mitigation)"
						, i, path.size()
						, max_safe_hops );
					continue;
				}
			}

			/* Refuse over-budget parts before sending (see the
			 * per-part fee-discipline note above).  sent = amount
			 * LEAVING us on hop[0] (our own outbound channel) =
			 * CLN's amount_sent_msat; its notional in->out "fee"
			 * is our own channel and not a real cost, so we read
			 * amount_out_msat to match how the reply books the
			 * fee.  delivered = amount out of the closing hop back
			 * to us.  fee = sent - delivered, exactly as the
			 * waitsendpay summary computes it. */
			{
				auto sent_msat = std::uint64_t(
				    Ln::Amount::object(path[std::size_t(0)]
					["amount_out_msat"]).to_msat());
				auto deliv_msat = std::uint64_t(
				    Ln::Amount::object(path[path.size() - 1]
					["amount_out_msat"]).to_msat());
				if (deliv_msat > 0 && sent_msat >= deliv_msat) {
					auto part_ppm =
					    double(sent_msat - deliv_msat)
					    * 1000000.0 / double(deliv_msat);
					if (part_ppm > budget_ppm) {
						skipped[i] = true;
						++num_skipped;
						chain = std::move(chain)
						      + Boss::log( bus, Info
							, "XMoveFunds: part %zu "
							  "refused: %.0f ppm "
							  "exceeds budget %.0f "
							  "ppm (would deliver "
							  "%s msat)"
							, i, part_ppm
							, budget_ppm
							, Util::Str::group_digits(
								deliv_msat)
							    .c_str() );
						continue;
					}
				}
			}

			auto last = path[path.size() - 1];
			auto fill_peer = Ln::NodeId(
			    std::string(last["node_id_in"]));
			std::string fill_scid;
			try {
				fill_scid = find_fill_scid(
				    *channels, *dest_set, fill_peer);
			} catch (std::exception const& ex) {
				err_msgs->push_back(ex.what());
				continue;
			}

			/* build_sendpay_route reads the closing hop's
			 * amount_msat / delay from the fake mirror at
			 * path[N-1] (left in place by the patched
			 * askrene), so no extra params here.  See the
			 * function's doc for why this is correct. */
			auto sendpay_route = build_sendpay_route(
			    route_obj, fill_scid, fill_peer);

			/* Extract this part's network middle hops for
			 * later positive reinforcement.  Iterate the
			 * full askrene path and keep every hop whose
			 * scid is NOT one of our local channels.  For
			 * the typical circular self-pay this drops the
			 * head hop (us->drain_peer) and keeps the rest
			 * of the path through to the last forwarder
			 * arriving at fill_peer.  amount_out_msat is
			 * what each hop forwarded -- that is the
			 * lower-bound capacity claim. */
			for (auto j = std::size_t(0); j < path.size(); ++j) {
				auto hop_j = path[j];
				auto scidd =
				    std::string(hop_j["short_channel_id_dir"]);
				auto slash = scidd.find('/');
				auto scid_str = scidd.substr(0, slash);
				if (our_scids->count(scid_str))
					continue;
				auto dir = std::uint32_t(
				    std::stoul(scidd.substr(slash + 1)));
				auto amt = Ln::Amount::object(
				    hop_j["amount_out_msat"]);
				(*per_part_middle)[i].emplace_back(
				    Ln::Scid(scid_str), dir, amt);
			}

			chain = std::move(chain)
			      + Boss::log( bus, Debug
					 , "XMoveFunds: part %zu sendpay "
					   "route fees:\n%s"
					 , i
					 , format_route_fees(path).c_str())
			      + sendpay_part(
				    *payment_hash,
				    *payment_secret,
				    std::move(sendpay_route),
				    *label, groupid_planned, partid,
				    p->amount)
				.then([groupid_actual, multi]
				      (Jsmn::Object resp) {
					/* Single-part: capture CLN's
					 * auto-assigned groupid so the
					 * outer reply and waitsendpay
					 * use the truthful value. */
					if (!multi
					 && resp.has("groupid")
					 && resp["groupid"].is_number())
						*groupid_actual =
						    std::uint64_t(double(
							resp["groupid"]));
					return Ev::lift();
				}).catching<RpcError>(
				    [err_msgs](RpcError const& e) {
					err_msgs->push_back(
					    "sendpay: "
					    + Util::stringify(
						e.error));
					return Ev::lift();
				});
		}

		/* Wait for every part to terminate.  We do this even
		 * if some sendpays failed up front -- the others may
		 * still be in flight and waiting cleans them up.  The
		 * waitsendpay_part call is wrapped in Ev::lift().then(...)
		 * so *groupid_actual is read at execute time, after the
		 * preceding sendpay_part has had a chance to update it
		 * from CLN's response (single-part path).  Multi-part
		 * just reads back the planned value we seeded.
		 *
		 * The .then (success) and .catching (failure) handlers
		 * accumulate inform_channel_* / disable_node actions
		 * into feedback_actions for the persistent xrebalance
		 * layer.
		 * They are run after all parts have terminated, so the
		 * payment-critical path is not slowed by feedback RPCs
		 * and to preserve a single coherent observation set
		 * across MPP parts. */
		for (auto i = std::size_t(0); i < num_parts; ++i) {
			if (skipped[i])
				continue;
			auto partid = multi ? (i + 1) : 0;
			chain = std::move(chain)
			      + Ev::lift().then(
				    [this, payment_hash, partid, i,
				     groupid_actual, results, err_msgs,
				     per_part_middle, our_scids,
				     askrene_response,
				     feedback_actions]
				    () {
					return waitsendpay_part(
						   *payment_hash, partid,
						   *groupid_actual)
					    .then([results, i,
						   per_part_middle,
						   feedback_actions,
						   askrene_response, this]
						  (Jsmn::Object r) {
						results->push_back(r);
						/* Positive reinforcement:
						 * every middle hop carried
						 * its amount on this part. */
						for (auto const& hop
						   : (*per_part_middle)[i]) {
							feedback_actions
							    ->push_back(
							    Boss::Mod::
							    AskreneLayer::
							    inform_channel_unconstrained(
								*rpc,
								Boss::Mod::
								AskreneLayer::
								xrebalance_layer_name,
								std::get<0>(hop),
								std::get<1>(hop),
								std::get<2>(hop)));
							auto obs = Msg::
							    XRebalanceObservation{
								std::uint64_t(
								    std::time(
									nullptr)),
								std::get<0>(hop),
								std::get<1>(hop),
								Msg::
								XRebalanceObservationKind
								    ::Success,
								std::get<2>(hop),
								0, Ln::NodeId()};
							feedback_actions
							    ->push_back(
							    bus.raise(
								std::move(obs)));
						}
						/* Per-part earnings
						 * attribution.  The askrene
						 * path identifies which
						 * source/dest peer this part
						 * actually used (MCF flow
						 * split means different parts
						 * of one xmovefunds invocation
						 * can land on different
						 * source/dest pairs).
						 * EarningsTracker subscribes
						 * to the resulting message and
						 * applies the same symmetric
						 * DB update it already runs on
						 * Msg::ResponseMoveFunds.
						 * Defensive on malformed JSON:
						 * skip silently if the path
						 * or waitsendpay result lacks
						 * the expected fields. */
						auto askrene_path =
						    (*askrene_response)
							["routes"][i]["path"];
						return raise_attribution(
						    askrene_path, r);
					    }).catching<RpcError>(
						[results, err_msgs,
						 our_scids,
						 askrene_response, i,
						 feedback_actions,
						 payment_hash, partid,
						 groupid_actual, this]
						(RpcError const& e) {
						results->push_back(
						    Jsmn::Object());
						/* Negative reinforcement:
						 * parse erring_channel /
						 * erring_node and write a
						 * constraint to the
						 * persistent layer so the
						 * next getroutes call steers
						 * around it.  The per-hop
						 * amount used as the
						 * constraint is recovered
						 * from this part's askrene
						 * path. */
						auto askrene_path =
						    (*askrene_response)
							["routes"][i]["path"];
						err_msgs->push_back(
						    failure_summary(
							e, askrene_path));
						accumulate_failure_feedback(
						    e, *our_scids,
						    askrene_path,
						    *feedback_actions);
						/* Tidy CLN's payment store
						 * so dead payment_hash
						 * entries do not pile up
						 * across heavy testing
						 * sessions.  Best-effort:
						 * the rebalance failure is
						 * already reported via
						 * err_msgs. */
						return delpay_part(
						    *payment_hash, partid,
						    *groupid_actual);
					    });
				});
		}

		/* Run the feedback actions accumulated above before
		 * assembling the reply.  At chain-build time the vector
		 * is empty; the wrapping Ev::lift().then(...) defers
		 * iteration until execute time, after waitsendpay has
		 * populated it. */
		chain = std::move(chain) + Ev::lift().then(
		    [feedback_actions, this]() -> Ev::Io<void> {
			if (feedback_actions->empty())
				return Ev::lift();
			auto fb = Ev::lift();
			for (auto& act : *feedback_actions) {
				fb = std::move(fb) + std::move(act);
			}
			return std::move(fb)
			     + Boss::log( bus, Debug
					, "XMoveFunds: wrote %zu "
					  "feedback entries to "
					  "clboss-xrebalance layer"
					, feedback_actions->size()
					);
		    });

		return std::move(chain).then(
		    [payment_hash, preimage, label,
		     groupid_actual, num_parts, num_skipped,
		     results, err_msgs]() {
			auto out = Json::Out();
			auto obj = out.start_object();
			obj.field("payment_hash",
				  std::string(*payment_hash));
			obj.field("preimage",
				  std::string(*preimage));
			obj.field("label", *label);
			obj.field("groupid", *groupid_actual);
			/* "parts" is the number actually sent; over-budget
			 * parts refused before sending are reported
			 * separately so "parts_complete / parts" stays a
			 * truthful success ratio over what was attempted. */
			obj.field("parts", num_parts - num_skipped);
			if (num_skipped > 0)
				obj.field("parts_skipped", num_skipped);

			/* Per-call summary across successful parts:
			 *   delivered_msat  = sum r.amount_msat for parts
			 *                     with status == "complete"
			 *   fee_total_msat  = sum (amount_sent_msat -
			 *                          amount_msat) over the
			 *                     same set
			 *   parts_complete  = count of parts that settled
			 *   fee_ppm         = fee_total_msat * 1e6 /
			 *                     delivered_msat  (omitted if
			 *                     delivered_msat == 0)
			 *
			 * Derivable from results[] but provided in-line
			 * so operators can read the result without
			 * post-processing -- and so a caller using
			 * jq/awk against the reply gets the same numbers
			 * EarningsTracker will record via the per-part
			 * Msg::XRebalanceAttribution path.
			 *
			 * Defensive: a failed part has a Jsmn::Object()
			 * placeholder in results (no status field); we
			 * filter on status == "complete" and require both
			 * amount fields before summing, so malformed or
			 * absent entries fall out cleanly. */
			auto delivered_msat = std::uint64_t(0);
			auto sent_msat = std::uint64_t(0);
			auto parts_complete = std::uint32_t(0);
			for (auto const& r : *results) {
				if (!r.is_object()
				 || !r.has("status")
				 || !r["status"].is_string()
				 || std::string(r["status"]) != "complete"
				 || !r.has("amount_msat")
				 || !r.has("amount_sent_msat"))
					continue;
				try {
					auto am = Ln::Amount::object(
					    r["amount_msat"]);
					auto as = Ln::Amount::object(
					    r["amount_sent_msat"]);
					if (as < am)
						continue;
					delivered_msat += am.to_msat();
					sent_msat      += as.to_msat();
					parts_complete += 1;
				} catch (std::exception const&) {
					continue;
				}
			}
			auto fee_total_msat = sent_msat - delivered_msat;
			obj.field("parts_complete", parts_complete);
			obj.field("delivered_msat", delivered_msat);
			obj.field("fee_total_msat", fee_total_msat);
			if (delivered_msat > 0) {
				auto fee_ppm = std::uint64_t(
				    fee_total_msat * std::uint64_t(1000000)
				    / delivered_msat);
				obj.field("fee_ppm", fee_ppm);
			}

			{
				auto arr = obj.start_array("results");
				for (auto const& r : *results)
					arr.entry(r);
				arr.end_array();
			}
			if (!err_msgs->empty()) {
				auto arr = obj.start_array("errors");
				for (auto const& m : *err_msgs)
					arr.entry(m);
				arr.end_array();
			}
			obj.end_object();
			return Ev::lift(std::move(out));
		});
	}

	/* Per-request flow.  Builds + uses a uuid-suffixed transient
	 * layer, calls getroutes, optionally runs sendpay, and
	 * returns the response.  The transient layer is removed
	 * before returning (success or failure). */
	Ev::Io<void>
	do_plan(std::shared_ptr<Params> p, Ln::CommandId id) {
		auto transient =
		    Boss::Mod::AskreneLayer::xrebalance_layer_name
		    + "-tmp-"
		    + std::string(Uuid::random());
		auto routes = std::make_shared<Jsmn::Object>();
		auto channels = std::make_shared<Jsmn::Object>();
		auto exec_result = std::make_shared<Json::Out>();
		auto exec_done = std::make_shared<bool>(false);
		auto err_msg = std::make_shared<std::string>();
		auto err_code = std::make_shared<int>(0);

		return create_transient_layer(transient
		).then([this, p, transient]() {
			return list_my_channels();
		}).then([this, p, transient, channels]
			(Jsmn::Object c) {
			*channels = c;
			return write_masks(transient, c, *p);
		}).then([this, p, transient]() {
			return call_getroutes(transient, *p);
		}).then([routes](Jsmn::Object r) {
			*routes = r;
			return Ev::lift();
		}).catching<RpcError>(
		    [err_msg, err_code](RpcError const& e) {
			*err_code = -32603;
			*err_msg = Util::stringify(e.error);
			return Ev::lift();
		}).then([this, p, routes, channels, exec_result,
			 exec_done, err_code]() {
			if (*err_code != 0 || !p->execute)
				return Ev::lift();
			return do_execute(p, routes, channels)
				.then([exec_result, exec_done]
				    (Json::Out r) {
					*exec_result = std::move(r);
					*exec_done = true;
					return Ev::lift();
				});
		}).then([this, transient]() {
			return remove_layer(transient);
		}).then([this, p, id, routes, channels, exec_result,
			 exec_done, err_msg, err_code]() {
			if (*err_code != 0) {
				return bus.raise(Msg::CommandFail{
					id, *err_code,
					"getroutes failed: " + *err_msg,
					Json::Out::empty_object()
				});
			}

			auto plan = Json::Out();
			auto obj = plan.start_object();
			obj.field("status",
				  std::string(*exec_done
					      ? "executed"
					      : p->execute
						  ? "execute_skipped"
						  : "planned"));
			{
				auto arr =
				    obj.start_array("source_scids");
				for (auto const& s : p->source_scids)
					arr.entry(std::string(s));
				arr.end_array();
			}
			{
				auto arr =
				    obj.start_array("dest_scids");
				for (auto const& s : p->dest_scids)
					arr.entry(std::string(s));
				arr.end_array();
			}
			obj.field("amount_msat",
				  std::uint64_t(
				      p->amount.to_msat()));
			/* Echo whatever caps the caller actually passed,
			 * then the binding value that ended up in
			 * effect.  When only one cap is specified the
			 * effective value equals it; when both are
			 * specified the smaller binds and the caller
			 * can see which one was the constraint. */
			if (p->maxfee_msat_in) {
				obj.field("maxfee_msat",
					  std::uint64_t(
					      p->maxfee_msat_in->to_msat()));
			}
			if (p->maxfee_ppm_in) {
				obj.field("maxfee_ppm",
					  *p->maxfee_ppm_in);
			}
			obj.field("maxfee_effective_msat",
				  std::uint64_t(
				      p->maxfee.to_msat()));
			obj.field("maxparts", p->maxparts);
			obj.field("execute", p->execute);
			/* Echo the askrene response in full so the
			 * caller (and the spike harness) can inspect
			 * the planned routes, per-hop amounts, and
			 * probabilities. */
			obj.field("askrene", *routes);
			if (*exec_done) {
				obj.field("execution",
					  std::move(*exec_result));
			}
			obj.end_object();
			return bus.raise(Msg::CommandResponse{
				id, std::move(plan)
			});
		});
	}

	Ev::Io<void> run_command(Jsmn::Object params, Ln::CommandId id) {
		auto p = std::make_shared<Params>();
		try {
			*p = parse_params(params);
		} catch (std::exception const& ex) {
			return bus.raise(Msg::CommandFail{
				id, RPC_INVALID_PARAMS,
				ex.what(),
				Json::Out::empty_object()
			});
		}

		return Boss::log( bus, Info
				, "XMoveFunds: planning %s -> %s, "
				  "amount=%s msat, "
				  "maxfee=%s msat, "
				  "maxparts=%" PRIu32 ", execute=%s"
				, join_scids(p->source_scids).c_str()
				, join_scids(p->dest_scids).c_str()
				, Util::Str::group_digits(std::uint64_t(
					p->amount.to_msat())).c_str()
				, Util::Str::group_digits(std::uint64_t(
					p->maxfee.to_msat())).c_str()
				, p->maxparts
				, p->execute ? "true" : "false"
				)
		     + wait_for_ready()
		     + do_plan(p, id);
	}

public:
	Impl(S::Bus& bus_)
		: bus(bus_)
		, rpc(nullptr)
		, claimer(bus_)
		, layer_ready(false)
		, aging_window_secs(3600)
		, rng(static_cast<std::uint64_t>(
		      std::chrono::system_clock::now()
		          .time_since_epoch().count())) {
		bus.subscribe<Msg::Init>([this](Msg::Init const& init) {
			rpc = &init.rpc;
			self_id = init.self_id;
			return Boss::concurrent(create_xrebalance_layer());
		});
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const&) {
			return bus.raise(Msg::ManifestCommand{
				"clboss-xmovefunds",
				"source_scid(s) dest_scid(s) amount_msat "
				"[maxfee_msat] [maxfee_ppm] "
				"[maxparts] [execute]",
				"Manually move funds in a circular "
				"self-payment via askrene.  Each of "
				"source_scid and dest_scid may be either "
				"a single scid string (e.g. "
				"\"305607x10x0\") or a JSON array of "
				"scid strings (e.g. "
				"[\"305607x10x0\",\"305121x18x2\"]); "
				"the masking layer enables the us->peer "
				"direction of every listed source and "
				"the peer->us direction of every listed "
				"dest, then askrene's MCF distributes "
				"the flow.  At least one of maxfee_msat "
				"(absolute cap, msat) or maxfee_ppm "
				"(relative cap, parts-per-million of "
				"amount_msat) is required; both may be "
				"specified and the more restrictive of "
				"the two binds.  maxparts defaults to "
				"10.  execute defaults to true (the route "
				"is actually sent via sendpay); pass "
				"execute=false for a plan-only response "
				"that returns the askrene plan without "
				"sending.  Lowest-level primitive used by "
				"the xrebalance algorithm; the caller "
				"specifies the explicit channel set.",
				false
			}) + bus.raise(Msg::ManifestOption{
				"clboss-xrebalance-age-secs",
				Msg::OptionType_Int,
				Json::Out::direct(aging_window_secs),
				"Cutoff (seconds) for periodic askrene-age "
				"on the persistent clboss-xrebalance "
				"layer.  Constraints older than this are "
				"trimmed once per TimerRandomHourly tick "
				"so stale capacity pessimism does not "
				"accumulate forever.  Dynamic: settable "
				"at runtime via `lightning-cli setconfig "
				"clboss-xrebalance-age-secs <secs>`.  "
				"Default 3600 (1h); operators on slower "
				"networks (signet) typically widen this.",
				/* dynamic = */ true
			});
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != "clboss-xrebalance-age-secs")
				return Ev::lift();
			/* At startup lightningd sends Int options as a
			 * JSON number primitive (Initiator forwards the
			 * value verbatim from the init request); at
			 * runtime lightningd's setconfig path encodes
			 * the value as a JSON string (see
			 * cln/lightningd/plugin.c
			 * plugin_set_dynamic_opt).  Tolerate both. */
			/* Signed so a negative value is rejected below
			 * rather than wrapping to a huge unsigned. */
			long long secs = 0;
			try {
				if (o.value.is_number()) {
					secs = static_cast<long long>(double(o.value));
				} else if (o.value.is_string()) {
					secs = std::stoll(std::string(o.value));
				} else {
					return Boss::log( bus, Warn
							, "XMoveFunds: "
							  "clboss-xrebalance-"
							  "age-secs: "
							  "unsupported value "
							  "type; keeping "
							  "%" PRIu64 "."
							, aging_window_secs
							);
				}
			} catch (std::exception const& e) {
				return Boss::log( bus, Warn
						, "XMoveFunds: clboss-"
						  "xrebalance-age-secs: "
						  "parse error '%s'; "
						  "keeping %" PRIu64 "."
						, e.what()
						, aging_window_secs
						);
			}
			if (secs <= 0) {
				return Boss::log( bus, Warn
						, "XMoveFunds: clboss-"
						  "xrebalance-age-secs: "
						  "must be > 0; keeping "
						  "%" PRIu64 "."
						, aging_window_secs
						);
			}
			aging_window_secs = std::uint64_t(secs);
			return Boss::log( bus, Info
					, "XMoveFunds: xrebalance layer "
					  "aging window = %" PRIu64
					  " seconds"
					, aging_window_secs
					);
		});
		bus.subscribe<Msg::TimerRandomHourly
			     >([this](Msg::TimerRandomHourly const&) {
			return wait_for_ready().then([this]() {
				return age_xrebalance_layer();
			});
		});
		bus.subscribe<Msg::CommandRequest
			     >([this](Msg::CommandRequest const& m) {
			if (m.command != "clboss-xmovefunds")
				return Ev::lift();
			return run_command(m.params, m.id);
		});
	}
};

Main::Main(Main&&) =default;
Main::~Main() =default;
Main::Main(S::Bus& bus_) : pimpl(Util::make_unique<Impl>(bus_)) { }

}}}
