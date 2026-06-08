#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Util/stringify.hpp"
#include<assert.h>
#include<cstdint>
#include<map>

namespace Boss { namespace Mod { namespace AskreneLayer {

std::string const clboss_layer_name = "clboss";
std::string const xrebalance_layer_name = "clboss-xrebalance";

namespace {

/* Coalescing state, keyed by "layer|scid-dir|inform-kind" -> the
 * tightest bound emitted in the current time bucket (see InformObs in
 * the header).  Safe as a file-static: the whole plugin runs on a
 * single Ev event-loop thread, so there is no concurrent access. */
std::map<std::string, InformObs> inform_cache;

/* The coalescing bucket length, derived as a fixed fraction
 * (1 / coalesce_window_divisor) of the layer aging window
 * (clboss-classic-layer-age-secs).  Making it a fraction of the aging
 * window means the keep-alive re-emit (once per bucket) always refreshes
 * a constraint well before it can age out, and the aging window is
 * always exactly coalesce_window_divisor buckets long whatever its
 * value -- so the prune and the depth floor are scale-invariant.
 * FundsMover feeds the live aging value via set_aging_window_secs();
 * this default matches aging/12 at the default 12h aging (1h bucket). */
std::uint64_t constexpr coalesce_window_divisor = 12;
double coalesce_window_secs = 43200.0 / double(coalesce_window_divisor);

/* Drop coalescing entries idle past the aging window, so the cache does
 * not grow with the set of channel-dirs seen over the process lifetime.
 * Amortised -- swept once per 4096 emits, not per call. */
void prune_inform_cache(std::uint64_t now_bucket) {
	static std::uint64_t emits = 0;
	if ((++emits & 0xFFF) != 0)
		return;
	/* The aging window is always coalesce_window_divisor buckets, so a
	 * few more than that covers any still-active dir at any aging value. */
	auto constexpr keep_buckets = std::uint64_t(coalesce_window_divisor + 4);
	for (auto it = inform_cache.begin(); it != inform_cache.end(); ) {
		if (it->second.bucket + keep_buckets < now_bucket)
			it = inform_cache.erase(it);
		else
			++it;
	}
}

/* Common machinery for the two inform_channel variants.  askrene
 * accepts inform=succeeded / constrained / unconstrained as the
 * only difference between them; everything else (scid_dir,
 * amount_msat, layer) is identical.
 */
Ev::Io<void>
inform_channel( Boss::Mod::Rpc& rpc
	      , std::string const& layer
	      , Ln::Scid scid
	      , std::uint32_t direction
	      , Ln::Amount amount
	      , char const* inform
	      ) {
	/* askrene only accepts direction 0 or 1 in
	 * short_channel_id_dir.  All callers feed values from
	 * CLN's getroutes/sendpay responses, which are
	 * guaranteed to be 0/1, but guard explicitly: a bad
	 * direction would produce a syntactically valid but
	 * semantically wrong RPC param that askrene rejects, and
	 * the silent-swallow RpcError handler below would drop
	 * the learning update without a trace.
	 */
	assert(direction <= 1);
	if (direction > 1)
		return Ev::lift();
	auto sdir = std::string(scid) + "/" + Util::stringify(direction);

	/* Coalesce redundant writes (see InformObs in the header): keep the
	 * tightest bound per (layer, scid-dir, kind) within one aging-derived
	 * bucket and emit only on a new bucket (keep-alive against the layer
	 * aging) or a tightening.  Dropping a dominated write is lossless --
	 * get_constraints folds the dir down to one tightest [min,max], so the
	 * dropped entry would not have changed any route. */
	auto const bucket = std::uint64_t(Ev::now() / coalesce_window_secs);
	auto const is_lower_bound = (std::string(inform) != "constrained");
	auto const key = layer + "|" + sdir + "|" + inform;
	auto const cache_it = inform_cache.find(key);
	auto const* prior = (cache_it == inform_cache.end())
			  ? nullptr : &cache_it->second;
	if (!inform_coalesce_emit( prior, bucket
				 , std::uint64_t(amount.to_msat())
				 , is_lower_bound
				 ))
		return Ev::lift();
	inform_cache[key] = InformObs{ bucket, std::uint64_t(amount.to_msat()) };
	prune_inform_cache(bucket);

	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
			.field("short_channel_id_dir", sdir)
			.field("amount_msat", amount.to_msat())
			.field("inform", std::string(inform))
		.end_object()
		;
	return rpc.command( "askrene-inform-channel"
			  , std::move(parms)
			  ).then([](Jsmn::Object _) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const&) {
		/* Non-fatal -- if the layer is missing (e.g. layer-
		 * create failed at startup on CLN < v24.11),
		 * subsequent getroutes calls simply will not benefit
		 * from the constraint.  Better to degrade learning
		 * than to crash the caller.
		 */
		return Ev::lift();
	});
}

}

/* The coalescing decision (see InformObs in the header).  Defined out
 * here rather than in the anonymous namespace so the unit test can call
 * it directly; inform_channel reaches it via the header declaration. */
bool
inform_coalesce_emit( InformObs const* prior
		    , std::uint64_t bucket
		    , std::uint64_t amount_msat
		    , bool is_lower_bound
		    ) {
	if (!prior)
		return true;                 /* first observation for this key */
	if (prior->bucket != bucket)
		return true;                 /* new bucket: keep-alive emit */
	/* Same bucket: emit only if this observation tightens the bound. */
	return is_lower_bound ? amount_msat > prior->tightest_msat
			      : amount_msat < prior->tightest_msat;
}

/* Set the coalescing bucket length from the current layer aging window
 * (clboss-classic-layer-age-secs), as aging / coalesce_window_divisor.
 * Called by FundsMover when that option loads or changes, so the
 * coalescing window tracks the aging window live.  Floored at 1s so a
 * pathological aging value can never produce a zero-length bucket. */
void set_aging_window_secs(std::uint64_t aging_secs) {
	auto const w = double(aging_secs) / double(coalesce_window_divisor);
	coalesce_window_secs = (w >= 1.0) ? w : 1.0;
}

Ev::Io<void>
inform_channel_constrained( Boss::Mod::Rpc& rpc
			  , std::string const& layer
			  , Ln::Scid scid
			  , std::uint32_t direction
			  , Ln::Amount amount
			  ) {
	return inform_channel(rpc, layer, scid, direction, amount, "constrained");
}

Ev::Io<void>
inform_channel_unconstrained( Boss::Mod::Rpc& rpc
			    , std::string const& layer
			    , Ln::Scid scid
			    , std::uint32_t direction
			    , Ln::Amount amount
			    ) {
	return inform_channel(rpc, layer, scid, direction, amount, "unconstrained");
}

Ev::Io<void>
update_channel( Boss::Mod::Rpc& rpc
	      , std::string const& layer
	      , Ln::Scid scid
	      , std::uint32_t direction
	      , bool enabled
	      , Ln::Amount htlc_minimum_msat
	      , Ln::Amount htlc_maximum_msat
	      , Ln::Amount fee_base_msat
	      , std::uint32_t fee_proportional_millionths
	      , std::uint16_t cltv_expiry_delta
	      ) {
	/* Same direction-validity guard as inform_channel: askrene
	 * only accepts 0 or 1 in short_channel_id_dir.
	 */
	assert(direction <= 1);
	if (direction > 1)
		return Ev::lift();
	auto sdir = std::string(scid) + "/" + Util::stringify(direction);
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
			.field("short_channel_id_dir", sdir)
			.field("enabled", enabled)
			.field("htlc_minimum_msat", htlc_minimum_msat.to_msat())
			.field("htlc_maximum_msat", htlc_maximum_msat.to_msat())
			.field("fee_base_msat", fee_base_msat.to_msat())
			.field( "fee_proportional_millionths"
			      , fee_proportional_millionths
			      )
			.field( "cltv_expiry_delta"
			      , cltv_expiry_delta
			      )
		.end_object()
		;
	return rpc.command( "askrene-update-channel"
			  , std::move(parms)
			  ).then([](Jsmn::Object _) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const&) {
		return Ev::lift();
	});
}

Ev::Io<bool>
is_node_disabled( Boss::Mod::Rpc& rpc
		, std::string const& layer
		, Ln::NodeId node
		) {
	auto target = std::string(node);
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
		.end_object()
		;
	return rpc.command( "askrene-listlayers"
			  , std::move(parms)
			  ).then([target = std::move(target)
				 ](Jsmn::Object res) {
		try {
			auto layers = res["layers"];
			if (!layers.is_array() || layers.size() == 0)
				return Ev::lift(false);
			auto layer_obj = layers[0];
			if (!layer_obj.has("disabled_nodes"))
				return Ev::lift(false);
			auto disabled = layer_obj["disabled_nodes"];
			if (!disabled.is_array())
				return Ev::lift(false);
			for (auto entry : disabled) {
				if (std::string(entry) == target)
					return Ev::lift(true);
			}
		} catch (std::exception const&) {
			/* Malformed response shape -- fall through to
			 * false so the caller continues without
			 * deduping rather than crashing.
			 */
		}
		return Ev::lift(false);
	}).catching<RpcError>([](RpcError const&) {
		/* Conservative on RPC error: returning false lets
		 * the caller fall through to its disable_node call
		 * (which also swallows RpcError).  Worst case is
		 * an accumulating duplicate, same as the pre-dedup
		 * behaviour.
		 */
		return Ev::lift(false);
	});
}

Ev::Io<void>
disable_node( Boss::Mod::Rpc& rpc
	    , std::string const& layer
	    , Ln::NodeId node
	    ) {
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
			.field("node", std::string(node))
		.end_object()
		;
	return rpc.command( "askrene-disable-node"
			  , std::move(parms)
			  ).then([](Jsmn::Object _) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const&) {
		return Ev::lift();
	});
}

}}}
