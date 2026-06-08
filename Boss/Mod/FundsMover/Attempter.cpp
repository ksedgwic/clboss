#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/FundsMover/Attempter.hpp"
#include"Boss/Mod/FundsMover/create_label.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Ev/yield.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Preimage.hpp"
#include"Ln/Scid.hpp"
#include"Sha256/Hash.hpp"
#include"Util/Str.hpp"
#include"Util/stringify.hpp"
#include"Uuid.hpp"
#include<assert.h>
#include<cinttypes>
#include<map>
#include<string>
#include<vector>

namespace {

/* BOLT 04 onion failure code -> short human-readable name.
 * Used in the sendpay-204 log so we can grep for the failcode
 * by name (FEE_INSUFFICIENT etc.) rather than only the hex.
 */
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

/* Parsed channel_update fields fed back into askrene via
 * AskreneLayer::update_channel after a sendpay 204 with an
 * onion-error failcode that carries a channel_update payload.
 * Mirrors the subset of BOLT 07 channel_update fields askrene-
 * update-channel accepts.
 */
struct ChanUpdate {
	bool          enabled;
	std::uint16_t cltv_expiry_delta;
	std::uint64_t htlc_minimum_msat;
	std::uint32_t fee_base_msat;
	std::uint32_t fee_proportional_millionths;
	std::uint64_t htlc_maximum_msat;

	bool operator==(ChanUpdate const& o) const {
		return enabled == o.enabled
		    && cltv_expiry_delta == o.cltv_expiry_delta
		    && htlc_minimum_msat == o.htlc_minimum_msat
		    && fee_base_msat == o.fee_base_msat
		    && fee_proportional_millionths == o.fee_proportional_millionths
		    && htlc_maximum_msat == o.htlc_maximum_msat;
	}
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
 * channel_update wire layout (BOLT 07), 130 bytes after the
 * optional 2-byte 0x0102 type prefix.  We only need the policy
 * fields (offset 13 onwards), so we skip past signature (64),
 * chain_hash (32), short_channel_id (8), and timestamp (4).
 * The 2-byte type prefix is present in CLN-issued
 * channel_updates and absent in LND-pre-v0.18 ones; detect by
 * sniffing the first two bytes.
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
	/* Fixed-layout body is 128 bytes from offset 0 of the
	 * post-prefix channel_update: 64 sig + 32 chain_hash + 8
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
	return true;
}

}

namespace Boss { namespace Mod { namespace FundsMover {

class Attempter::Impl : public std::enable_shared_from_this<Impl> {
private:
	S::Bus& bus;

	Boss::Mod::Rpc& rpc;
	Ln::NodeId self_id;
	Ln::Preimage preimage;
	Ln::Preimage payment_secret;
	Ln::NodeId source;
	Ln::NodeId destination;
	Ln::Amount amount;
	std::shared_ptr<Ln::Amount> fee_budget;
	std::shared_ptr<Ln::Amount> remaining_amount;
	/* Snapshot of the Runner's original budget and original amount at
	 * the time of construction.  Used together to compute the per-
	 * Attempter absolute fee cap (orig_budget * amount / orig_amount):
	 * an Attempter is never allowed to spend more than the Runner's
	 * original per-msat rate scaled to its own amount, regardless of
	 * how the pool's prorata has drifted from sibling reservations or
	 * sibling spends.  Without this cap a late-surviving Attempter can
	 * end up with prorated_fee_budget = full unreserved pool while
	 * remaining_amount has been mostly drained by siblings, producing
	 * effective per-msat rates well above max_fee_ppm (observed at
	 * 1892 ppm in production 2026-05-28 with a 193 ppm Runner rate).
	 * Snapshotted at construction (not shared_ptr) because the
	 * Runner's amount and orig_budget never change after start.
	 */
	Ln::Amount orig_budget;
	Ln::Amount orig_amount;
	/* Details of the last channel from destination to us.  */
	Ln::Scid last_scid;
	Ln::Amount base_fee;
	std::uint32_t proportional_fee;
	std::uint32_t cltv_delta;
	/* Details of the first channel from us to source.  */
	Ln::Scid first_scid;

	bool ok;

	Ln::Amount dest_amount;
	Ln::Amount source_amount;
	std::uint32_t source_delay;

	/* Route from source to destination, not including the
	 * us->source and destination->us hops.  Populated from
	 * getroutes' `path[]` array, with each hop translated into the
	 * sendpay-compatible shape (id, scid, direction, amount_msat,
	 * delay).
	 */
	struct Hop {
		Ln::NodeId id;
		Ln::Scid scid;
		std::uint32_t direction;
		Ln::Amount amount_msat;
		std::uint32_t delay;
	};
	std::vector<Hop> route;

	/* Per-Attempter cache of forwarder-signed channel policies
	 * parsed from BOLT 04 onion errors in this Attempter's retry
	 * chain.  Key is "scid/dir"; value is the most recent
	 * channel_update payload we parsed for that direction.
	 *
	 * Read by apply_policy_overrides() during route translation:
	 * for each hop in askrene's path[], if we have an entry here
	 * we recompute the upstream amount/delay using these
	 * authoritative values (with ceiling rounding for prop fees)
	 * instead of trusting askrene's path[] values, which are
	 * derived from gossmap and not refreshed by our
	 * AskreneLayer::update_channel writes (askrene-getroutes
	 * does not honour the layer's channel_updates entries when
	 * computing per-hop delay; verified empirically in production
	 * 2026-05-27 with bias saturated at -100 and
	 * channel_updates carrying the correct cltv but the
	 * returned path's delay differences still reflecting the
	 * stale gossmap value).
	 *
	 * Written in the 204 handler alongside the askrene-update-
	 * channel call to the persistent clboss layer.  The two
	 * writes share an interpretation: the layer carries the
	 * value forward across plugin lifetimes (and benefits
	 * other CLBOSS subsystems that consult the layer), and the
	 * in-memory cache here is what our own route translation
	 * actually consults each retry.
	 */
	std::map<std::string, ChanUpdate> policy_overrides;

	/* The fee we currently have.  */
	Ln::Amount our_fee;

	/* 8-char correlation tag for log lines, generated fresh per
	 * Attempter at construction time.  Every Attempter log line
	 * is prefixed with [tag] so all events for one run are
	 * greppable from a busy log.
	 */
	std::string attempt_uuid;

	/* Wall-clock timestamps + snapshot of the shared pool state at
	 * the start of core_run().  Used by the start / finish log
	 * lines to expose how budget contention with concurrent sibling
	 * Attempters affects this run's prorated_budget.
	 */
	double start_time;
	Ln::Amount initial_pool_budget;
	Ln::Amount initial_pool_remaining;

	std::string attempt_tag() const {
		return attempt_uuid;
	}

public:
	Impl( S::Bus& bus_
	    , Boss::Mod::Rpc& rpc_
	    , Ln::NodeId self_id_
	    , Ln::Preimage preimage_
	    , Ln::Preimage payment_secret_
	    , Ln::NodeId source_
	    , Ln::NodeId destination_
	    , Ln::Amount amount_
	    , std::shared_ptr<Ln::Amount> fee_budget_
	    , std::shared_ptr<Ln::Amount> remaining_amount_
	    , Ln::Scid last_scid_
	    , Ln::Amount base_fee_
	    , std::uint32_t proportional_fee_
	    , std::uint32_t cltv_delta_
	    , Ln::Scid first_scid_
	    , Ln::Amount orig_budget_
	    , Ln::Amount orig_amount_
	    ) : bus(bus_)
	      , rpc(rpc_)
	      , self_id(std::move(self_id_))
	      , preimage(std::move(preimage_))
	      , payment_secret(std::move(payment_secret_))
	      , source(std::move(source_))
	      , destination(std::move(destination_))
	      , amount(amount_)
	      , fee_budget(std::move(fee_budget_))
	      , remaining_amount(std::move(remaining_amount_))
	      , orig_budget(orig_budget_)
	      , orig_amount(orig_amount_)
	      , last_scid(last_scid_)
	      , base_fee(base_fee_)
	      , proportional_fee(proportional_fee_)
	      , cltv_delta(cltv_delta_)
	      , first_scid(first_scid_)
	      , ok(false)
	      , attempt_uuid(std::string(Uuid::random()).substr(0, 8))
	      { }
	Ev::Io<bool> run() {
		auto self = shared_from_this();
		return self->core_run().then([self]() {
			auto duration_ms = std::uint64_t(
				(Ev::now() - self->start_time) * 1000.0
			);
			char const* outcome = self->ok ? "success" : "give_up";
			return Boss::log( self->bus, Debug
					, "FundsMover[%s]: finish: outcome=%s "
					  "duration_ms=%" PRIu64 " "
					  "pool delta: budget=%s remaining=%s "
					  "(initial budget=%s remaining=%s; "
					  "now budget=%s remaining=%s)"
					, self->attempt_tag().c_str()
					, outcome
					, duration_ms
					, std::string(
						self->initial_pool_budget
						- *self->fee_budget
					  ).c_str()
					, std::string(
						self->initial_pool_remaining
						- *self->remaining_amount
					  ).c_str()
					, std::string(self->initial_pool_budget)
						.c_str()
					, std::string(self->initial_pool_remaining)
						.c_str()
					, std::string(*self->fee_budget)
						.c_str()
					, std::string(*self->remaining_amount)
						.c_str()
					).then([self]() {
				return Ev::lift(self->ok);
			});
		});
	}

private:
	Ev::Io<void> core_run() {
		return Ev::lift().then([this]() {
			/* Snapshot for start/finish logs.  Comparing pool
			 * state at start vs end exposes contention with
			 * concurrent sibling Attempters (Runner's split-
			 * fanout): if start-pool-budget is much smaller
			 * than the original Runner orig_budget, siblings
			 * have optimistically deducted ahead of this run.
			 */
			start_time              = Ev::now();
			initial_pool_budget     = *fee_budget;
			initial_pool_remaining  = *remaining_amount;

			/* dest_amount is the amount destination should
			 * receive on its incoming channel from us.  Old
			 * comment is preserved because the math is the
			 * same -- the legacy code used this in getroute's
			 * amount_msat; we now use it as getroutes'
			 * amount_msat (the amount delivered to the
			 * destination peer of askrene's request).
			 */
			dest_amount = amount + base_fee
				    + (amount * ( double(proportional_fee)
						/ 1000000
						))
				    + Ln::Amount::msat(1)
				    ;

			assert(amount <= *remaining_amount);
			auto prorata = amount / *remaining_amount;
			auto prorated_fee_budget = *fee_budget * prorata;

			auto src_pfx = std::string(source).substr(0, 8);
			auto dst_pfx = std::string(destination).substr(0, 8);
			return Boss::log( bus, Debug
					, "FundsMover[%s]: start: amount=%s "
					  "src=%s... dst=%s...; "
					  "pool *fee_budget=%s *remaining=%s; "
					  "prorata=%.4f prorated_budget=%s"
					, attempt_tag().c_str()
					, std::string(amount).c_str()
					, src_pfx.c_str()
					, dst_pfx.c_str()
					, std::string(*fee_budget).c_str()
					, std::string(*remaining_amount).c_str()
					, prorata
					, std::string(prorated_fee_budget).c_str()
					).then([this]() {
				return getroute();
			});
		});
	}
	Ev::Io<void> getroute() {
		return Ev::yield().then([this]() {
			/* Prorate our share of the global fee budget and
			 * pass it to askrene as a hard maxfee_msat cap.
			 * The prorated budget covers our TOTAL fee
			 * (source_amount - amount), but askrene's
			 * maxfee_msat only constrains the middle-route
			 * portion (source_amount - dest_amount).  The
			 * destination's outgoing-channel fee
			 * (dest_amount - amount) is a fixed cost we
			 * already know, so subtract it from the budget
			 * before handing it to askrene.  If the
			 * destination's last-hop fee alone exceeds our
			 * budget, we cannot proceed; askrene will
			 * return 206 even faster than we would compute
			 * it locally.  No fuzz-ladder retry analogue in
			 * askrene's probability-based cost model.
			 */
			assert(amount <= *remaining_amount);
			auto prorata = amount / *remaining_amount;
			auto prorated_fee_budget = *fee_budget * prorata;
			auto dest_hop_fee = dest_amount - amount;
			auto route_maxfee = prorated_fee_budget > dest_hop_fee
					  ? prorated_fee_budget - dest_hop_fee
					  : Ln::Amount::sat(0);

			auto pj = Json::Out();
			auto obj = pj.start_object();
			obj.field("source", std::string(source));
			obj.field("destination", std::string(destination));
			obj.field("amount_msat", dest_amount.to_msat());
			auto la = obj.start_array("layers");
			la.entry("auto.localchans");
			/* Deliberately NOT including auto.sourcefree.
			 *
			 * auto.sourcefree zeros the askrene-source's
			 * outgoing-channel fees in the routing model.  It
			 * is designed for the normal case where the
			 * askrene-source IS the paying node (= you);
			 * zeroing your own outgoing fees is correct
			 * because you do not pay yourself.
			 *
			 * In FundsMover the askrene-source is the source
			 * PEER (not us), pinned that way so the route
			 * starts at the channel we want to drain.  If we
			 * included auto.sourcefree here, askrene would
			 * treat the source peer's outgoing fees as zero,
			 * route plans against `maxfee_msat = prorated -
			 * dst_hop_fee` would silently exclude the source
			 * peer's real outgoing fee, and the
			 * compute_source_amount add-back would then push
			 * our total `our_fee` to roughly `src_hop_fee +
			 * prorated_budget`, which is always over budget
			 * by exactly src_hop_fee.  Empirically this is
			 * the failure pattern we saw in production starting
			 * the day this code path replaced legacy
			 * getroute+pay: every Attempter hitting `budget
			 * FAIL: our_fee > prorated` even when the network
			 * had viable routes.
			 *
			 * Without auto.sourcefree, askrene's MCF includes
			 * the source peer's real outgoing fee in its cost
			 * accounting and enforces our maxfee_msat against
			 * (src_hop_fee + middle_fees).  The routes askrene
			 * returns therefore fit the budget by construction
			 * once we add the (already-known) dst_hop_fee.
			 * compute_source_amount keeps its add-back math
			 * unchanged: route[0].amount_msat is still the
			 * amount forwarded out of source toward the first
			 * middle, and we still need to send source enough
			 * to cover that plus its real fee.
			 */
			la.entry(Boss::Mod::AskreneLayer::clboss_layer_name);
			la.end_array();
			obj.field("maxfee_msat", route_maxfee.to_msat());
			obj.field("final_cltv", cltv_delta + 14);
			obj.field("maxparts", 1);
			obj.end_object();
			return Boss::log( bus, Debug
					, "FundsMover[%s]: getroutes call: "
					  "amount=%s maxfee_msat=%s final_cltv=%u"
					, attempt_tag().c_str()
					, std::string(dest_amount).c_str()
					, std::string(route_maxfee).c_str()
					, unsigned(cltv_delta + 14)
					).then([this, pj = std::move(pj)
					       ]() mutable {
				return rpc.command( "getroutes"
						  , std::move(pj)
						  );
			});
		}).then([this](Jsmn::Object res) {
			auto prob_ppm = std::int64_t(0);
			try {
				auto routes = res["routes"];
				if (!routes.is_array() || routes.size() == 0)
					throw Jsmn::TypeError();
				auto r0 = routes[0];
				if (r0.has("probability_ppm")
				 && r0["probability_ppm"].is_number())
					prob_ppm = std::int64_t(double(
						r0["probability_ppm"]
					));
				auto path = r0["path"];
				if (!path.is_array() || path.size() == 0)
					throw Jsmn::TypeError();

				/* getroutes path[] hop fields were renamed
				 * in CLN v26.06.  Which set of names
				 * actually appears in the response
				 * depends on the CLN version AND whether
				 * the node runs with developer mode
				 * (which suppresses deprecated outputs):
				 *
				 *   v26.04                  -> old only
				 *   v26.06+ no developer    -> both emitted
				 *   v26.06+ developer=true  -> new only
				 *
				 * Bridge by preferring the new name,
				 * falling back to the old.  TODO: drop
				 * the fallback once CLN v26.04 is no
				 * longer supported and the old names are
				 * removed in v27.06.
				 *
				 * short_channel_id_dir is older (v24.11)
				 * and is emitted unconditionally.
				 */
				route.clear();
				for (auto hop_j : path) {
					Hop hop;
					hop.id = Ln::NodeId(std::string(
						hop_j.has("node_id_out")
							? hop_j["node_id_out"]
							: hop_j["next_node_id"]
					));
					auto sdir = std::string(
						hop_j["short_channel_id_dir"]
					);
					auto slash = sdir.find('/');
					if (slash == std::string::npos)
						throw Jsmn::TypeError();
					hop.scid = Ln::Scid(
						sdir.substr(0, slash)
					);
					hop.direction = std::uint32_t(
						std::stoul(sdir.substr(slash + 1))
					);
					hop.amount_msat = Ln::Amount::object(
						hop_j.has("amount_out_msat")
							? hop_j["amount_out_msat"]
							: hop_j["amount_msat"]
					);
					hop.delay = std::uint32_t(double(
						hop_j.has("cltv_out")
							? hop_j["cltv_out"]
							: hop_j["delay"]
					));
					route.push_back(hop);
				}
			} catch (std::exception const&) {
				/* Broaden catch to std::exception so we
				 * also handle std::invalid_argument and
				 * std::out_of_range from std::stoul on a
				 * malformed short_channel_id_dir tail.
				 * The other accesses in this block only
				 * raise Jsmn::TypeError (subclass of
				 * std::exception), so they remain caught.
				 * Matches the same fix in
				 * ActiveProber.cpp's parse block.
				 */
				return Boss::log( bus, Error
						, "FundsMover[%s]: Unexpected "
						  "getroutes response: %s"
						, attempt_tag().c_str()
						, Util::stringify(res)
							.c_str()
						).then([]() {
					return Ev::lift(false);
				});
			}
			/* CLN's common/sphinx.c:serialize_onionpacket can SIGSEGV
			 * when the cumulative per-hop TLV payloads exceed the
			 * fixed 1300-byte onion buffer.  Observed at 26 hops on
			 * production (twice in 14 hours, 2026-05-28 and 2026-05-29),
			 * both times with the identical sphinx backtrace.
			 *
			 * The cleanest workaround is to reject overly long routes
			 * before we hand them to sendpay.  20 hops gives a
			 * comfortable margin under the observed crash threshold
			 * (askrene's path length plus our two spliced hops would
			 * fit well within sphinx's buffer at this size).  Askrene
			 * doesn't expose a max_hops parameter, so we post-check
			 * the returned route here rather than constraining the
			 * getroutes call upstream.
			 *
			 * Remove once CLN's serialize_onionpacket validates the
			 * payload size and returns an error instead of crashing.
			 */
			auto constexpr max_safe_hops = std::size_t(20);
			if (route.size() > max_safe_hops)
				return Boss::log( bus, Debug
						, "FundsMover[%s]: route too long: "
						  "%zu hops > %zu max; giving up "
						  "(CLN sphinx crash mitigation)"
						, attempt_tag().c_str()
						, route.size()
						, max_safe_hops
						).then([]() {
					return Ev::lift(false);
				});
			auto overrides_applied = apply_policy_overrides();
			auto first_hop = route.empty()
				? std::string("")
				: ( std::string(route[0].scid) + "/"
				  + std::to_string(route[0].direction)
				  );
			return Boss::log( bus, Debug
					, "FundsMover[%s]: route ok: "
					  "prob_ppm=%" PRIi64
					  " hops=%zu first_hop=%s "
					  "policy_overrides=%zu"
					, attempt_tag().c_str()
					, prob_ppm
					, route.size()
					, first_hop.c_str()
					, overrides_applied
					).then([]() {
				return Ev::lift(true);
			});
		}).catching<RpcError>([this](RpcError const& e) {
			/* Errors 205 ("Unable to find a route"), 206
			 * ("Route too expensive"), and any others mean
			 * we cannot proceed.  No retry: askrene already
			 * considered our maxfee_msat budget and the
			 * accumulated failure-feedback layer.
			 */
			return Boss::log( bus, Debug
					, "FundsMover[%s]: getroutes failed "
					  "(%s); giving up attempt to "
					  "move %s from %s to %s."
					, attempt_tag().c_str()
					, Util::stringify(e.error).c_str()
					, std::string(amount).c_str()
					, std::string(source).c_str()
					, std::string(destination).c_str()
					).then([]() {
				return Ev::lift(false);
			});
		}).then([this](bool ok) {
			if (ok)
				return compute_source_amount();
			return Ev::lift();
		});
	}

	/* Recompute per-hop amount and delay across the middle hops
	 * of askrene's path, walking backward from the last hop.
	 * Returns the count of hops for which we had a layer-of-
	 * policy override (for logging).
	 *
	 * Two passes worth of correction happen here:
	 *
	 *   (a) For each hop whose scid/dir we have a parsed
	 *       channel_update for (populated by the 204 handler
	 *       in this Attempter's previous retries), recompute
	 *       the upstream amount and delay from the
	 *       channel_update's authoritative
	 *       cltv_expiry_delta / fee_base / fee_prop, using
	 *       ceiling rounding on the proportional fee.
	 *
	 *   (b) For each hop with no override, keep askrene's
	 *       path values for delay but add 1 msat to the
	 *       upstream amount to absorb the floor-vs-ceiling
	 *       rounding mismatch between askrene (floor) and
	 *       forwarders (ceiling) on prop-fee channels.  This
	 *       cumulates to (route.size()-1) msat over the path,
	 *       small relative to our minimum_split_size.
	 *
	 * Why this exists: empirically (production, 2026-05-27)
	 * askrene-getroutes does NOT honour our layer
	 * channel_updates entries when computing the path[]'s
	 * delay differences -- a layer write with the
	 * forwarder-signed cltv_expiry_delta does not change
	 * subsequent path[i].delay - path[i-1].delay.  Forwarders
	 * meanwhile enforce their currently-signed delta exactly,
	 * so a route built from askrene's stale view fails
	 * with INCORRECT_CLTV_EXPIRY on every retry through that
	 * hop.  Recomputing the per-hop delay locally from our
	 * own policy cache is the fix.
	 *
	 * route[route.size()-1] is left untouched: its amount and
	 * delay are the destination-side targets we asked askrene
	 * to deliver (= destination_peer in the circular-rebalance
	 * splice; the dest_peer -> us hop is appended separately
	 * in make_route()).
	 *
	 * Pure computation, no RPC.  Safe to call after route is
	 * populated by the getroutes parse and before
	 * compute_source_amount() reads route[0].
	 */
	std::size_t apply_policy_overrides() {
		auto applied = std::size_t(0);
		if (route.size() < 2)
			return applied;
		/* Iterate i from route.size()-1 down to 1, computing
		 * route[i-1] from route[i].  The channel "between"
		 * route[i-1] and route[i] is route[i].channel/direction
		 * (the hop forwards INTO that channel), so the policy
		 * lookup keys on route[i].scid/route[i].direction.
		 */
		for (auto i = route.size(); i-- > 1; ) {
			auto key = std::string(route[i].scid) + "/"
				 + std::to_string(route[i].direction);
			auto it = policy_overrides.find(key);
			if (it == policy_overrides.end()) {
				/* No override: keep askrene's amount and
				 * delay for this hop's upstream side, but
				 * bump amount by 1 msat to cover the
				 * floor-vs-ceiling rounding gap.  Forwarders
				 * accept overpayment.
				 */
				route[i - 1].amount_msat = route[i - 1].amount_msat
							 + Ln::Amount::msat(1);
				continue;
			}
			auto const& cu = it->second;
			auto amt_msat = route[i].amount_msat.to_msat();
			/* ceil(amt * prop / 1e6) in integer math.
			 * Overflow is not a concern at our amounts: the
			 * largest amount we route is bounded by
			 * msg.amount and prop is bounded by 1e6, so the
			 * product is at most ~1e7 * 1e6 = 1e13, far
			 * below uint64 max.
			 */
			auto prop_fee_msat =
				( amt_msat * std::uint64_t(cu.fee_proportional_millionths)
				+ std::uint64_t(999999)
				) / std::uint64_t(1000000);
			route[i - 1].amount_msat =
				  route[i].amount_msat
				+ Ln::Amount::msat(cu.fee_base_msat)
				+ Ln::Amount::msat(prop_fee_msat);
			route[i - 1].delay = route[i].delay
					   + std::uint32_t(cu.cltv_expiry_delta);
			++applied;
		}
		return applied;
	}

	/* Compute source_amount (what we send to source) and
	 * source_delay (initial CLTV) from the first hop of the route
	 * plus a listchannels round-trip to learn the source's
	 * outgoing-channel fees.
	 *
	 * Once CLN v26.04 is no longer supported, this can be folded
	 * back into getroute(): getroutes' new path[0].amount_in_msat
	 * and path[0].cltv_in (both added v26.06) give these values
	 * directly without the listchannels call.
	 */
	Ev::Io<void> compute_source_amount() {
		return Ev::lift().then([this]() {
			auto parms = Json::Out()
				.start_object()
					.field( "short_channel_id"
					      , std::string(route[0].scid)
					      )
				.end_object()
				;
			return rpc.command("listchannels", std::move(parms));
		}).then([this](Jsmn::Object res) {
			auto found = false;
			auto src_base_fee = Ln::Amount::sat(0);
			auto src_prop_fee = std::uint32_t(0);
			auto src_cltv_delta = std::uint32_t(0);
			try {
				auto cs = res["channels"];
				for (auto c : cs) {
					auto csrc = Ln::NodeId(std::string(
						c["source"]
					));
					if (csrc != source)
						continue;
					src_base_fee = Ln::Amount::msat(double(
						c["base_fee_millisatoshi"]
					));
					src_prop_fee = std::uint32_t(double(
						c["fee_per_millionth"]
					));
					src_cltv_delta = std::uint32_t(double(
						c["delay"]
					));
					found = true;
				}
			} catch (Jsmn::TypeError const&) {
				return Boss::log( bus, Error
						, "FundsMover[%s]: Unexpected "
						  "listchannels response: %s"
						, attempt_tag().c_str()
						, Util::stringify(res)
							.c_str()
						);
			}
			if (!found)
				return Boss::log( bus, Debug
						, "FundsMover[%s]: listchannels "
						  "did not include the "
						  "source's outgoing channel "
						  "%s; giving up attempt."
						, attempt_tag().c_str()
						, std::string(route[0].scid)
							.c_str()
						);

			source_amount = route[0].amount_msat + src_base_fee
				      + (route[0].amount_msat
					 * ( double(src_prop_fee)
					   / 1000000
					   ))
				      + Ln::Amount::msat(1)
				      ;
			source_delay = route[0].delay + src_cltv_delta;
			our_fee = source_amount - amount;

			/* Fee breakdown for the budget log lines.
			 * our_fee = src_hop_fee + middle_route_cost + dst_hop_fee
			 * where:
			 *   src_hop_fee = source_amount - route[0].amount_msat
			 *     (source's forwarding fee for source -> first
			 *      middle node, computed from listchannels above)
			 *   middle_route_cost = route[0].amount_msat
			 *                     - dest_amount
			 *     (askrene's route cost across the middle path)
			 *   dst_hop_fee = dest_amount - amount
			 *     (destination's forwarding fee for
			 *      destination -> us)
			 */
			auto src_hop_fee  = source_amount - route[0].amount_msat;
			auto mid_cost     = route[0].amount_msat - dest_amount;
			auto dst_hop_fee  = dest_amount - amount;
			auto src_scid_str = std::string(route[0].scid);

			auto src_hop_log = Boss::log( bus, Debug
				, "FundsMover[%s]: source-hop: "
				  "chan=%s base=%s prop=%uppm "
				  "-> source_amount=%s src_hop_fee=%s"
				, attempt_tag().c_str()
				, src_scid_str.c_str()
				, std::string(src_base_fee).c_str()
				, unsigned(src_prop_fee)
				, std::string(source_amount).c_str()
				, std::string(src_hop_fee).c_str()
				);

			/* Defensive: askrene's maxfee_msat constrains
			 * the middle-route portion of our_fee.  If
			 * gossip drift or rounding has produced a
			 * route whose actual fee exceeds our prorated
			 * budget, bail rather than overspend.
			 */
			assert(amount <= *remaining_amount);
			auto prorata = amount / *remaining_amount;
			auto prorated_fee_budget = *fee_budget * prorata;
			/* Absolute rate cap: never spend more than the
			 * Runner's original per-msat rate (orig_budget /
			 * orig_amount) scaled to this Attempter's amount.
			 * Without this, late-surviving Attempters can see
			 * inflated prorated_fee_budget when siblings have
			 * drained *remaining_amount faster than *fee_budget
			 * (sibling reservations against amount without
			 * proportional spending), allowing per-msat rates
			 * far above max_fee_ppm.  Observed in production
			 * 2026-05-28: a Runner with orig rate of 193 ppm
			 * produced a successful Attempter spending at
			 * 1892 ppm because remaining had drained to ~= amount
			 * by the time the budget check ran.
			 *
			 * Both checks fire independently; we take the tighter.
			 * The prorata cap remains useful for short-term
			 * fairness against in-flight siblings; the absolute
			 * cap is the unconditional rate-discipline guard.
			 */
			auto absolute_fee_cap = orig_budget * (amount / orig_amount);
			auto effective_cap = prorated_fee_budget < absolute_fee_cap
					   ? prorated_fee_budget
					   : absolute_fee_cap;
			if (our_fee > effective_cap) {
				auto reason = (our_fee > absolute_fee_cap)
					    ? "absolute"
					    : "prorated";
				return std::move(src_hop_log)
				     + Boss::log( bus, Debug
						, "FundsMover[%s]: budget FAIL: "
						  "our_fee=%s "
						  "(src_hop=%s + middle=%s + "
						  "dst_hop=%s) > %s_cap=%s "
						  "(prorated=%s absolute=%s); "
						  "giving up"
						, attempt_tag().c_str()
						, std::string(our_fee).c_str()
						, std::string(src_hop_fee).c_str()
						, std::string(mid_cost).c_str()
						, std::string(dst_hop_fee).c_str()
						, reason
						, std::string(effective_cap).c_str()
						, std::string(prorated_fee_budget).c_str()
						, std::string(absolute_fee_cap).c_str()
						);
			}

			*fee_budget -= our_fee;
			*remaining_amount -= amount;
			return std::move(src_hop_log)
			     + Boss::log( bus, Debug
					, "FundsMover[%s]: budget OK: "
					  "our_fee=%s "
					  "(src_hop=%s + middle=%s + "
					  "dst_hop=%s) <= cap=%s "
					  "(prorated=%s absolute=%s)"
					, attempt_tag().c_str()
					, std::string(our_fee).c_str()
					, std::string(src_hop_fee).c_str()
					, std::string(mid_cost).c_str()
					, std::string(dst_hop_fee).c_str()
					, std::string(effective_cap).c_str()
					, std::string(prorated_fee_budget).c_str()
					, std::string(absolute_fee_cap).c_str()
					)
			     + sendpay();
		}).catching<RpcError>([this](RpcError const& e) {
			return Boss::log( bus, Debug
					, "FundsMover[%s]: listchannels failed "
					  "(%s); giving up attempt."
					, attempt_tag().c_str()
					, Util::stringify(e.error).c_str()
					);
		});
	}


	Ev::Io<void> sendpay() {
		auto payment_hash = std::make_shared<Sha256::Hash>();
		return Ev::lift().then([this, payment_hash]() {
			*payment_hash = preimage.sha256();
			auto label = create_label(*payment_hash);
			auto parms = Json::Out()
				.start_object()
					.field("route", make_route())
					.field( "payment_hash"
					      , std::string(*payment_hash)
					      )
					.field("label" , label)
					.field( "payment_secret"
					      , std::string(payment_secret)
					      )
				.end_object()
				;
			auto hash_str = std::string(*payment_hash);
			return Boss::log( bus, Debug
					, "FundsMover[%s]: sendpay: "
					  "payment_hash=%s source_amount=%s "
					  "route_hops=%zu"
					, attempt_tag().c_str()
					, hash_str.c_str()
					, std::string(source_amount).c_str()
					, route.size()
					).then([this, parms = std::move(parms)
					       ]() mutable {
				return rpc.command( "sendpay"
						  , std::move(parms)
						  );
			});
		}).then([this, payment_hash](Jsmn::Object _) {
			auto parms = Json::Out()
				.start_object()
					.field( "payment_hash"
					      , std::string(*payment_hash)
					      )
				.end_object()
				;
			return rpc.command("waitsendpay", std::move(parms));
		}).then([this, payment_hash](Jsmn::Object _) {
			/* Success?  Now we can stop.  */
			ok = true;

			/* Positive reinforcement: every middle hop in
			 * this route just proved it can carry at least
			 * the amount it actually carried.  Record those
			 * lower-bound observations into the persistent
			 * clboss askrene layer so future getroutes calls
			 * see a balanced picture, not a monotonically
			 * darkening one of only failure-driven upper
			 * bounds.  Per-hop amount_msat is what that
			 * specific channel carried (the amount shrinks
			 * hop by hop as fees are subtracted along the
			 * route).
			 *
			 * Us->source and destination->us hops are
			 * skipped on the same rationale ActiveProber
			 * applies to its chan0: local channel state is
			 * already authoritative via listpeerchannels /
			 * auto.localchans, so the shared askrene layer
			 * is reserved for cross-subsystem knowledge.
			 *
			 * PR5's askrene-age sweep ages these entries on
			 * the same 24h cadence as the failure entries.
			 */
			auto reinforcement = Ev::lift();
			for (auto const& hop : route) {
				reinforcement = std::move(reinforcement)
					      + Boss::Mod::AskreneLayer::inform_channel_unconstrained(
							rpc,
							Boss::Mod::AskreneLayer::clboss_layer_name,
							hop.scid, hop.direction,
							hop.amount_msat
						);
			}

			auto fee_ppm = amount.to_msat() > 0
				? double(our_fee.to_msat()) * 1000000.0
					/ double(amount.to_msat())
				: 0.0;
			auto src_pfx = std::string(source).substr(0, 8);
			auto dst_pfx = std::string(destination).substr(0, 8);
			return delpay(payment_hash, true)
			     + std::move(reinforcement)
			     + Boss::log( bus, Info
					, "FundsMover[%s]: Moved %s from "
					  "%s..., getting %s to %s..., "
					  "costing us %s "
					  "(our_fee/amount=%.1f ppm)"
					, attempt_tag().c_str()
					, std::string(source_amount).c_str()
					, src_pfx.c_str()
					, std::string(amount).c_str()
					, dst_pfx.c_str()
					, std::string(our_fee).c_str()
					, fee_ppm
					)
			     + Boss::log( bus, Debug
					, "FundsMover[%s]: positive "
					  "reinforcement: wrote min_msat "
					  "for %zu middle hop(s) to clboss "
					  "layer."
					, attempt_tag().c_str()
					, route.size()
					)
			     ;
		}).catching<RpcError>([ this
				      , payment_hash
				      ](RpcError const& e) {
			/* Starting action: delete failing moves.  */
			auto act = delpay(payment_hash, false);

			/* Return our fee to the budget.  */
			*fee_budget += our_fee;
			*remaining_amount += amount;

			/* Figure out the error.  */
			auto code = int();
			auto eidx = std::size_t();
			auto echan = Ln::Scid();
			auto edir = int();
			auto enode = Ln::NodeId();
			auto fail = std::uint16_t();
			auto raw_msg_hex = std::string();
			try {
				auto& error = e.error;
				code = int(double(
					error["code"]
				));
				/* Failure along route.  */
				if (code == 204) {
					auto data = error["data"];
					eidx = std::size_t(double(
						data["erring_index"]
					));
					echan = Ln::Scid(std::string(
						data["erring_channel"]
					));
					edir = int(double(
						data["erring_direction"]
					));
					enode = Ln::NodeId(std::string(
						data["erring_node"]
					));
					fail = std::uint16_t(double(
						data["failcode"]
					));
					/* raw_message is the BOLT 04 onion
					 * failure payload as a hex string;
					 * we parse it later for the
					 * channel_update some failcodes
					 * embed.  Absent in older CLN
					 * versions -- treat as empty.
					 */
					if (data.has("raw_message"))
						raw_msg_hex = std::string(
							data["raw_message"]
						);
				}
			} catch (std::exception const& ex) {
				return std::move(act)
				     + Boss::log( bus, Error
						, "FundsMover[%s]: Attempt: "
						  "Unexpected error from "
						  "%s: %s: %s"
						, attempt_tag().c_str()
						, e.command.c_str()
						, Util::stringify(e.error).c_str()
						, ex.what()
						);
			}

			if (code != 202 && code != 204)
				return std::move(act)
				     + Boss::log( bus, Info
						, "FundsMover[%s]: Attempt: "
						  "Unexpected error code "
						  "%d from %s, error: %s"
						, attempt_tag().c_str()
						, code
						, e.command.c_str()
						, Util::stringify(e.error)
							.c_str()
						);
			/* Unparsable onion with a 1-hop route means the
			 * source or destination node has massive issues,
			 * so cannot advance.  */
			if (code == 202 && route.size() <= 1)
				return std::move(act)
				     + Boss::log( bus, Info
						, "FundsMover[%s]: Attempt: "
						  "Unparsable onion, cannot "
						  "advance further."
						, attempt_tag().c_str()
						);

			/* feedback: action recorded into the persistent
			 * "clboss" askrene layer, so the subsequent
			 * getroute() retry routes around the failure.
			 * Empty if the failure mode means we should
			 * stop entirely.
			 */
			auto feedback = Ev::lift();

			if (code == 204) {
				act += Boss::log( bus, Debug
						, "FundsMover[%s]: sendpay 204: "
						  "erring_idx=%zu "
						  "erring_chan=%s/%d "
						  "erring_node=%s "
						  "failcode=0x%04x (%s)"
						, attempt_tag().c_str()
						, eidx
						, std::string(echan).c_str()
						, edir
						, std::string(enode).c_str()
						, int(fail)
						, failcode_name(fail)
						);

				/* Positive reinforcement from a FAILED attempt:
				 * every middle hop strictly before the failure
				 * point forwarded the HTLC, proving liquidity
				 * >= the amount it carried -- the same
				 * lower-bound claim the success path records.
				 * Without it a wall-heavy node only ever writes
				 * failure-driven upper bounds and the layer
				 * darkens monotonically (the very thing the
				 * success-path comment above avoids).  Mirrors
				 * XMoveFunds's transit observations (the
				 * forwarded hops of failed parts).
				 *
				 * route[k] is at sendpay-route index k+1 (index
				 * 0 is the us->source splice), so it carried iff
				 * k+1 < eidx.  The erring hop route[eidx-1] is
				 * left out: it is constrained below, and for
				 * 0x100c may be excluded -- never reinforce and
				 * exclude the same hop.  Us->source /
				 * destination->us are not in `route`, so local
				 * channels are naturally skipped (auto.localchans
				 * is authoritative).  The destination-failure
				 * case (eidx == route.size()+1) thus reinforces
				 * the whole carried middle route.
				 */
				{
					auto n_reinforced = std::size_t(0);
					for ( auto k = std::size_t(0)
					    ; k + 1 < eidx && k < route.size()
					    ; ++k
					    ) {
						act += Boss::Mod::AskreneLayer::inform_channel_unconstrained(
							rpc,
							Boss::Mod::AskreneLayer::clboss_layer_name,
							route[k].scid, route[k].direction,
							route[k].amount_msat
						);
						++n_reinforced;
					}
					if (n_reinforced > 0)
						act += Boss::log( bus, Debug
								, "FundsMover[%s]: positive "
								  "reinforcement: wrote min_msat "
								  "for %zu carried hop(s) before "
								  "the failure to clboss layer."
								, attempt_tag().c_str()
								, n_reinforced
								);
				}

				if ( eidx == 0
				  || (eidx == 1 && (fail & 0x2000))
				   )
					return std::move(act)
					     + Boss::log( bus, Info
							, "FundsMover[%s]: "
							  "Failed at source, "
							  "cannot advance "
							  "further."
							, attempt_tag().c_str()
							)
					     ;
				if (eidx == route.size() + 1)
					return std::move(act)
					     + Boss::log( bus, Info
							, "FundsMover[%s]: "
							  "Failed at "
							  "destination, "
							  "cannot advance "
							  "further."
							, attempt_tag().c_str()
							)
					     ;
				/* 0x2000 == NODE level error.  */
				if ((fail & 0x2000)) {
					/* Persistent disable_node is correct
					 * for NODE-level failures and is also
					 * consulted by this Attempter's own
					 * subsequent getroutes calls (the
					 * clboss layer is in the layers
					 * array), so no separate transient
					 * write is needed for this case.
					 */
					feedback = Boss::Mod::AskreneLayer::disable_node(
						rpc,
						Boss::Mod::AskreneLayer::clboss_layer_name,
						enode
					)
					+ Boss::log( bus, Debug
						   , "FundsMover[%s]: feedback: "
						     "disable_node %s on clboss"
						   , attempt_tag().c_str()
						   , std::string(enode).c_str()
						   );
				} else {
					/* Non-NODE 204 failure feedback policy.
					 *
					 * All writes land in the persistent clboss
					 * layer, which has time-based aging so
					 * stale corrections drop out without us
					 * having to manage scope.
					 *
					 * Two complementary signals, mutually
					 * exclusive per failure (max_msat=0 makes
					 * askrene refuse the channel regardless of
					 * how correct a refreshed fee in
					 * update_channel would be, so we never
					 * write both for the same failure):
					 *
					 *   (a) askrene-update-channel: when the
					 *       failcode carries a BOLT 07
					 *       channel_update that we can parse,
					 *       overrides the channel-direction's
					 *       fee/cltv/htlc-bounds in the layer
					 *       (xpay-equivalent behaviour from
					 *       cln/plugins/xpay/xpay.c:
					 *       process_channel_update_from_onion_
					 *       error).
					 *
					 *   (b) askrene-inform-channel
					 *       max_msat=0: absolute exclusion of
					 *       this channel-direction.  Used when
					 *       we cannot refresh policy (older
					 *       CLN with no raw_message, malformed
					 *       payload) so the channel is at
					 *       least removed from consideration
					 *       until aging clears it.
					 *
					 *   0x1007 (capacity TCF):
					 *     channel_update payload is policy,
					 *     not capacity; refreshing fee/cltv
					 *     does not address "channel can't
					 *     push this amount right now".  Skip
					 *     update_channel; the persistent
					 *     max_msat=amount write below carries
					 *     the capacity signal.
					 *
					 *   non-0x1007 (0x100b/0x100c/0x100d/0x100e)
					 *   AND raw_message parseable:
					 *     Write update_channel only.  The
					 *     failure was a policy/cltv/htlc-bound
					 *     mismatch and the channel_update tells
					 *     us the corrected values.
					 *
					 *   non-0x1007 AND raw_message absent or
					 *   unparseable: fall back to max_msat=0
					 *   so the channel drops out of routing
					 *   until aging clears the constraint.
					 *
					 * eidx indexes [hop0, route..., hoplast];
					 * we have already returned early for
					 * eidx == 0 and eidx == route.size() + 1,
					 * so eidx is in [1, route.size()] and
					 * route[eidx - 1] describes the failing
					 * channel.
					 */
					auto refreshed_policy = false;
					if ( fail != 0x1007
					  && !raw_msg_hex.empty()
					   ) {
						auto cu = ChanUpdate();
						if (parse_chan_update(raw_msg_hex, cu)) {
							auto key = std::string(echan) + "/"
								 + std::to_string(edir);
							auto prev = policy_overrides.find(key);
							if ( prev != policy_overrides.end()
							  && prev->second == cu
							   ) {
								/* No-info refresh: the
								 * forwarder returned the
								 * same channel_update we
								 * already cached and tried.
								 * The previous retry built
								 * a route using these values
								 * and was still rejected --
								 * the forwarder's enforced
								 * policy diverges from
								 * their signed policy.
								 * Refreshing again will not
								 * change anything; hard-
								 * exclude the channel for
								 * the rest of the clboss
								 * aging window so subsequent
								 * Attempters (this Runner
								 * and concurrent Runners)
								 * route around it.  After
								 * aging, the constraint
								 * expires and we re-test
								 * the channel naturally.
								 *
								 * The inform_channel_
								 * constrained(amount=1)
								 * write sets max_msat<1 in
								 * the clboss layer, which
								 * dominates gossmap via
								 * askrene's min-across-
								 * layers semantic (askrene/
								 * layer.c:1008) and is a
								 * structural capacity bound
								 * in the MCF, not a soft
								 * cost preference (verified
								 * in askrene/child/mcf.c:
								 * linearize_channel).  The
								 * route_query.c:get_
								 * constraints applies this
								 * to every pathfinding
								 * call.
								 *
								 * Observed pattern that
								 * motivates this branch:
								 * Tachyon (02b21730...) on
								 * production 2026-05-27 signed
								 * base=0 prop=0 on
								 * 926646x39x1/1, returned
								 * the identical signed
								 * payload in every 0x100c
								 * response, generated 200+
								 * retries in 20 minutes
								 * before this branch was
								 * added.  Without it the
								 * update_channel write is
								 * a no-op (overwrites
								 * cache with same bytes)
								 * and the route translation
								 * applies 0/0 again,
								 * producing identical
								 * sendpay with identical
								 * rejection.
								 */
								feedback = std::move(feedback)
									 + Boss::Mod::AskreneLayer::inform_channel_constrained(
										rpc,
										Boss::Mod::AskreneLayer::clboss_layer_name,
										echan, edir,
										Ln::Amount::msat(1)
									)
									+ Boss::log( bus, Debug
										   , "FundsMover[%s]: "
										     "feedback: clboss "
										     "max_msat=0 on %s/%d "
										     "(repeat chan_update; "
										     "forwarder enforcement "
										     "diverges from signed "
										     "policy)"
										   , attempt_tag().c_str()
										   , std::string(echan).c_str()
										   , edir
										   );
							} else {
								/* New or changed
								 * channel_update.  Cache it
								 * for apply_policy_overrides
								 * on the next retry, and
								 * mirror to the clboss
								 * layer (for other CLBOSS
								 * subsystems that consult
								 * the layer).
								 */
								policy_overrides[key] = cu;
								feedback = std::move(feedback)
									 + Boss::Mod::AskreneLayer::update_channel(
										rpc,
										Boss::Mod::AskreneLayer::clboss_layer_name,
										echan,
										std::uint32_t(edir),
										cu.enabled,
										Ln::Amount::msat(cu.htlc_minimum_msat),
										Ln::Amount::msat(cu.htlc_maximum_msat),
										Ln::Amount::msat(cu.fee_base_msat),
										cu.fee_proportional_millionths,
										cu.cltv_expiry_delta
									)
									+ Boss::log( bus, Debug
										   , "FundsMover[%s]: "
										     "feedback: clboss "
										     "update_channel %s/%d "
										     "enabled=%d "
										     "base=%umsat prop=%uppm "
										     "cltv=%u "
										     "min=%" PRIu64 "msat "
										     "max=%" PRIu64 "msat"
										   , attempt_tag().c_str()
										   , std::string(echan).c_str()
										   , edir
										   , int(cu.enabled)
										   , unsigned(cu.fee_base_msat)
										   , unsigned(cu.fee_proportional_millionths)
										   , unsigned(cu.cltv_expiry_delta)
										   , cu.htlc_minimum_msat
										   , cu.htlc_maximum_msat
										   );
							}
							refreshed_policy = true;
						}
					}
					if (!refreshed_policy) {
						feedback = std::move(feedback)
							 + Boss::Mod::AskreneLayer::inform_channel_constrained(
								rpc,
								Boss::Mod::AskreneLayer::clboss_layer_name,
								echan, edir,
								Ln::Amount::msat(1)
							)
							+ Boss::log( bus, Debug
								   , "FundsMover[%s]: "
								     "feedback: clboss "
								     "max_msat=0 on %s/%d"
								   , attempt_tag().c_str()
								   , std::string(echan).c_str()
								   , edir
								   );
					}
					/* Capacity signal (only for 0x1007).  A
					 * conditional max_msat=amount constraint
					 * recording "this channel could not push
					 * amount msat just now".  Aged out of the
					 * clboss layer after the configured aging
					 * window so transient capacity dips do not
					 * embed permanently.
					 */
					if (fail == 0x1007) {
						feedback = std::move(feedback)
							 + Boss::Mod::AskreneLayer::inform_channel_constrained(
								rpc,
								Boss::Mod::AskreneLayer::clboss_layer_name,
								echan, edir,
								route[eidx - 1].amount_msat
							)
							+ Boss::log( bus, Debug
								   , "FundsMover[%s]: "
								     "feedback: clboss "
								     "max_msat=%s on %s/%d "
								     "(TEMP_CHAN_FAILURE)"
								   , attempt_tag().c_str()
								   , std::string(
									route[eidx - 1]
										.amount_msat
								     ).c_str()
								   , std::string(echan).c_str()
								   , edir
								   );
					}
				}
			} else {
				/* Unparsable onion (code 202): we cannot pin
				 * the failure to a specific channel.  The
				 * legacy excludes-vector code disabled a
				 * random middle hop locally for the remainder
				 * of the rebalance attempt, but doing the
				 * same via the persistent clboss askrene
				 * layer would blacklist a healthy random node
				 * across all future attempts and restarts
				 * after a single ambiguous failure.  We drop
				 * the write entirely and rely on askrene's
				 * natural route diversity to pick a different
				 * path on retry; the route.size() <= 1
				 * guard above already bails out when there
				 * is no diversity available.
				 */
				act += Boss::log( bus, Debug
						, "FundsMover[%s]: sendpay 202: "
						  "unparsable onion; no per-"
						  "channel feedback, relying "
						  "on askrene route diversity "
						  "for retry."
						, attempt_tag().c_str()
						);
			}

			return std::move(act)
			     + std::move(feedback)
			     + getroute();
		});
	}
	/* Splice the us->source and destination->us hops with the
	 * source->destination middle hops we got from getroutes.
	 */
	Json::Out make_route() {
		auto ret = Json::Out();
		auto arr = ret.start_array();
		arr.entry(make_hop0());
		for (auto const& hop : route) {
			arr.start_object()
					.field("id", std::string(hop.id))
					.field("channel",
					       std::string(hop.scid))
					.field("direction", hop.direction)
					.field("amount_msat",
					       hop.amount_msat.to_msat())
					.field("delay", hop.delay)
					.field("style", "tlv")
				.end_object();
		}
		arr.entry(make_hoplast());
		arr.end_array();
		return ret;
	}
	Json::Out make_hop0() {
		/* Us->source hop.  */
		return Json::Out()
			.start_object()
				.field("id", std::string(source))
				.field("channel", std::string(first_scid))
				.field( "direction"
				      , self_id > source ? 1 : 0
				      )
				.field("amount_msat", source_amount.to_msat())
				.field("delay", source_delay)
				/* This used to be an explicit "style": "legacy",
				 * since we did not want to have to parse listnodes
				 * just to find out if the first peer supports
				 * "tlv".
				 * However, recent C-Lightning versions have
				 * dropped support for legacy completely, so at
				 * this point it is now safer to switch explicitly
				 * to "style": "tlv".
				 * 0.11.x C-Lightning has it optional and requires
				 * it to be "tlv" if specified, but some older
				 * versions have it optional and default to
				 * "legacy".
				 * Unfortunately, some new node software releases
				 * *do not support* "legacy" *at all*, so if our
				 * first peer runs such no-legacy-nope-nada
				 * software, dropping "style" may lead to our
				 * own node using "legacy"-by-default, which
				 * would cause funds movement to fail.
				 * So now we explicitly say "tlv" here.
				 */
				.field("style", "tlv")
			.end_object()
			;
	}
	Json::Out make_hoplast() {
		/* Destination->us hop.  */
		return Json::Out()
			.start_object()
				.field("id", std::string(self_id))
				.field("channel", std::string(last_scid))
				.field( "direction"
				      , destination > self_id ? 1 : 0
				      )
				.field("amount_msat", amount.to_msat())
				.field("delay", 14)
				/* We always support "tlv", at least for now... */
				.field("style", "tlv")
			.end_object()
			;
	}

	Ev::Io<void> delpay( std::shared_ptr<Sha256::Hash> payment_hash
			   , bool success
			   ) {
		return Ev::lift().then([this, payment_hash, success]() {
			auto status = std::string(
				success ? "complete" : "failed"
			);
			auto parms = Json::Out()
				.start_object()
					.field( "payment_hash"
					      , std::string(*payment_hash)
					      )
					.field("status", status)
				.end_object()
				;
			return rpc.command("delpay", std::move(parms));
			/* Do not care if it succeeds or fails or what
			 * it returns.  */
		}).then([](Jsmn::Object _) {
			return Ev::lift();
		}).catching<RpcError>([](RpcError const& _) {
			return Ev::lift();
		});
	}

};

Ev::Io<bool>
Attempter::run( S::Bus& bus
	      , Boss::Mod::Rpc& rpc
	      , Ln::NodeId self
	      /* The preimage should have been pre-arranged to be claimed.  */
	      , Ln::Preimage preimage
	      , Ln::Preimage payment_secret
	      , Ln::NodeId source
	      , Ln::NodeId destination
	      , Ln::Amount amount
	      , std::shared_ptr<Ln::Amount> fee_budget
	      , std::shared_ptr<Ln::Amount> remaining_amount
	      /* Details of the channel from destination to us.  */
	      , Ln::Scid last_scid
	      , Ln::Amount base_fee
	      , std::uint32_t proportional_fee
	      , std::uint32_t cltv_delta
	      /* The channel from us to source.  */
	      , Ln::Scid first_scid
	      /* Snapshot of the Runner's original budget and amount, used
	       * to compute the per-Attempter absolute fee cap (see Impl
	       * member doc above).
	       */
	      , Ln::Amount orig_budget
	      , Ln::Amount orig_amount
	      ) {
	auto impl = std::make_shared<Impl>( bus
					  , rpc
					  , std::move(self)
					  , std::move(preimage)
					  , std::move(payment_secret)
					  , std::move(source)
					  , std::move(destination)
					  , amount
					  , std::move(fee_budget)
					  , std::move(remaining_amount)
					  , last_scid
					  , base_fee
					  , proportional_fee
					  , cltv_delta
					  , first_scid
					  , orig_budget
					  , orig_amount
					  );
	return impl->run();
}

}}}
