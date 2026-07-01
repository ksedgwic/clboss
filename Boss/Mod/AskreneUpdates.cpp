#include"Boss/Mod/AskreneUpdates.hpp"
#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Msg/AskreneChannelUpdate.hpp"
#include"Boss/Msg/AskreneNodeDisableUpdate.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
#include"Boss/Msg/RequestAskreneUpdates.hpp"
#include"Boss/Msg/ResponseAskreneUpdates.hpp"
#include"Boss/Msg/TimerRandomHourly.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include"Uuid.hpp"
#include<cinttypes>
#include<cstdint>
#include<string>

namespace {

/* How long after its last occurrence a learned update is still projected
 * into the per-request layer.  Separate knobs -- a down node and a
 * re-priced channel may deserve different half-lives.  */
auto constexpr default_node_disable_age_secs = std::uint64_t(3600);  /* 1h */
auto constexpr default_channel_update_age_secs = std::uint64_t(3600); /* 1h */
/* How long a row survives in the log at all -- long, because the log
 * doubles as a mineable history of what got disabled / re-priced.  */
auto constexpr default_retain_secs = std::uint64_t(2592000);         /* 30d */

}

namespace Boss { namespace Mod {

class AskreneUpdates::Impl {
private:
	S::Bus& bus;
	std::function<double()> get_now;
	Sqlite3::Db db;

	std::uint64_t node_disable_age_secs;
	std::uint64_t channel_update_age_secs;
	std::uint64_t retain_secs;

	void start() {
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& r) {
			db = r.db;
			return init();
		});
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const&) {
			return bus.raise(Msg::ManifestOption{
				"clboss-node-disable-age-secs",
				Msg::OptionType_Int,
				Json::Out::direct(default_node_disable_age_secs),
				"How long (seconds) after the most recent "
				"NODE-level routing failure CLBOSS keeps "
				"disabling that node in rebalance route "
				"searches.  Once this elapses with no fresh "
				"failure the node is no longer projected and "
				"becomes routable again.  Dynamic via "
				"`lightning-cli setconfig`.  Default 3600 (1h).",
				/* dynamic = */ true
			}) + bus.raise(Msg::ManifestOption{
				"clboss-channel-update-age-secs",
				Msg::OptionType_Int,
				Json::Out::direct(default_channel_update_age_secs),
				"How long (seconds) after the most recent "
				"failure-learned channel_update CLBOSS keeps "
				"applying that policy override (fees, htlc "
				"bounds, enabled flag) in rebalance route "
				"searches.  Once this elapses with no fresh "
				"update the channel reverts to gossip policy.  "
				"Dynamic via `lightning-cli setconfig`.  "
				"Default 3600 (1h).",
				/* dynamic = */ true
			}) + bus.raise(Msg::ManifestOption{
				"clboss-update-retain-secs",
				Msg::OptionType_Int,
				Json::Out::direct(default_retain_secs),
				"How long (seconds) learned node-disable and "
				"channel-update rows are kept in the CLBOSS "
				"database before pruning.  Independent of the "
				"projection windows above: the log is retained "
				"well past when an update stops being applied, "
				"so it can be mined (which nodes churn, which "
				"channels re-price).  Dynamic via `lightning-cli "
				"setconfig`.  Default 2592000 (30d).",
				/* dynamic = */ true
			});
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name == "clboss-node-disable-age-secs")
				return handle_option( o, node_disable_age_secs
						    , "clboss-node-disable-age-secs");
			if (o.name == "clboss-channel-update-age-secs")
				return handle_option( o, channel_update_age_secs
						    , "clboss-channel-update-age-secs");
			if (o.name == "clboss-update-retain-secs")
				return handle_option( o, retain_secs
						    , "clboss-update-retain-secs");
			return Ev::lift();
		});
		bus.subscribe<Msg::AskreneNodeDisableUpdate
			     >([this](Msg::AskreneNodeDisableUpdate const& m) {
			if (!db)
				return Ev::lift();
			return record_node(m.node);
		});
		bus.subscribe<Msg::AskreneChannelUpdate
			     >([this](Msg::AskreneChannelUpdate const& m) {
			if (!db)
				return Ev::lift();
			return record_channel(m);
		});
		bus.subscribe<Msg::RequestAskreneUpdates
			     >([this](Msg::RequestAskreneUpdates const& req) {
			auto requester = req.requester;
			if (!db)
				return bus.raise(Msg::ResponseAskreneUpdates{
					requester, {}, {}
				});
			return provide(requester);
		});
		bus.subscribe<Msg::TimerRandomHourly
			     >([this](Msg::TimerRandomHourly const&) {
			if (!db)
				return Ev::lift();
			return prune();
		});
	}

	/* Parse and apply one *-age-secs / retain-secs option, tolerating
	 * both the number-at-startup and string-via-setconfig encodings.  */
	Ev::Io<void> handle_option( Msg::Option const& o
				  , std::uint64_t& target
				  , char const* name
				  ) {
		auto secs = std::uint64_t(0);
		try {
			if (o.value.is_number()) {
				secs = std::uint64_t(double(o.value));
			} else if (o.value.is_string()) {
				secs = std::stoull(std::string(o.value));
			} else {
				return Boss::log( bus, Warn
						, "AskreneUpdates: %s: "
						  "unsupported value type; "
						  "keeping %" PRIu64 "."
						, name, target
						);
			}
		} catch (std::exception const& e) {
			return Boss::log( bus, Warn
					, "AskreneUpdates: %s: parse error "
					  "'%s'; keeping %" PRIu64 "."
					, name, e.what(), target
					);
		}
		if (secs == 0)
			return Boss::log( bus, Warn
					, "AskreneUpdates: %s: must be > 0; "
					  "keeping %" PRIu64 "."
					, name, target
					);
		target = secs;
		return Boss::log( bus, Info
				, "AskreneUpdates: %s = %" PRIu64 " seconds."
				, name, target
				);
	}

	Ev::Io<void> init() {
		return db.transact().then([](Sqlite3::Tx tx) {
			tx.query_execute(R"QRY(
			CREATE TABLE IF NOT EXISTS "AskreneNodeDisableUpdates"
			     ( time INTEGER NOT NULL -- unix seconds
			     , node TEXT NOT NULL
			     );
			CREATE INDEX IF NOT EXISTS
			    idx_askrenenodedisableupdates_node_time
			    ON "AskreneNodeDisableUpdates" (node, time);
			CREATE INDEX IF NOT EXISTS
			    idx_askrenenodedisableupdates_time
			    ON "AskreneNodeDisableUpdates" (time);
			CREATE TABLE IF NOT EXISTS "AskreneChannelUpdates"
			     ( time INTEGER NOT NULL -- unix seconds
			     , scid TEXT NOT NULL
			     , dir INTEGER NOT NULL -- askrene direction 0/1
			     , enabled INTEGER NOT NULL
			     , htlc_min_msat INTEGER NOT NULL
			     , htlc_max_msat INTEGER NOT NULL
			     , base_fee_msat INTEGER NOT NULL
			     , prop_fee_ppm INTEGER NOT NULL
			     , cltv_delta INTEGER NOT NULL
			     );
			CREATE INDEX IF NOT EXISTS
			    idx_askrenechannelupdates_scid_dir_time
			    ON "AskreneChannelUpdates" (scid, dir, time);
			CREATE INDEX IF NOT EXISTS
			    idx_askrenechannelupdates_time
			    ON "AskreneChannelUpdates" (time);
			)QRY");
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> record_node(Ln::NodeId node) {
		auto now = std::uint64_t(get_now());
		auto node_s = std::string(node);
		return db.transact().then([now, node_s](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			INSERT INTO "AskreneNodeDisableUpdates"
			VALUES(:time, :node);
			)QRY")
				.bind(":time", now)
				.bind(":node", node_s)
				.execute()
				;
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> record_channel(Msg::AskreneChannelUpdate cu) {
		auto now = std::uint64_t(get_now());
		return db.transact().then([now, cu](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			INSERT INTO "AskreneChannelUpdates"
			VALUES( :time, :scid, :dir, :enabled, :hmin, :hmax
			      , :base, :prop, :cltv);
			)QRY")
				.bind(":time", now)
				.bind(":scid", std::string(cu.scid))
				.bind(":dir", cu.direction)
				.bind(":enabled", cu.enabled)
				.bind(":hmin", cu.htlc_minimum_msat.to_msat())
				.bind(":hmax", cu.htlc_maximum_msat.to_msat())
				.bind(":base", cu.fee_base_msat.to_msat())
				.bind(":prop", cu.fee_proportional_millionths)
				.bind(":cltv", cu.cltv_expiry_delta)
				.execute()
				;
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> provide(void* requester) {
		auto now = std::uint64_t(get_now());
		auto node_cutoff = (now > node_disable_age_secs)
				 ? now - node_disable_age_secs : std::uint64_t(0);
		auto chan_cutoff = (now > channel_update_age_secs)
				 ? now - channel_update_age_secs : std::uint64_t(0);
		return db.transact().then([this, requester, node_cutoff, chan_cutoff
					  ](Sqlite3::Tx tx) {
			auto resp = Msg::ResponseAskreneUpdates{requester, {}, {}};

			auto nq = tx.query(R"QRY(
			SELECT DISTINCT node
			  FROM "AskreneNodeDisableUpdates"
			 WHERE time >= :cutoff;
			)QRY");
			nq.bind(":cutoff", node_cutoff);
			for (auto& r : nq.execute())
				resp.node_disables.push_back(
					Ln::NodeId(r.get<std::string>(0)));

			/* Latest override per (scid, dir) still in window.
			 * sqlite fills the bare columns from the MAX(time)
			 * row of each group.  */
			auto cq = tx.query(R"QRY(
			SELECT scid, dir, enabled, htlc_min_msat, htlc_max_msat
			     , base_fee_msat, prop_fee_ppm, cltv_delta, MAX(time)
			  FROM "AskreneChannelUpdates"
			 WHERE time >= :cutoff
			 GROUP BY scid, dir;
			)QRY");
			cq.bind(":cutoff", chan_cutoff);
			for (auto& r : cq.execute()) {
				auto ndx = 0;
				auto scid = r.get<std::string>(ndx++);
				auto dir = r.get<std::uint32_t>(ndx++);
				auto enabled = r.get<int>(ndx++);
				auto hmin = r.get<std::uint64_t>(ndx++);
				auto hmax = r.get<std::uint64_t>(ndx++);
				auto base = r.get<std::uint64_t>(ndx++);
				auto prop = r.get<std::uint64_t>(ndx++);
				auto cltv = r.get<std::uint64_t>(ndx++);
				resp.channel_updates.push_back(
					Msg::AskreneChannelUpdate{
						Ln::Scid(scid),
						dir,
						enabled != 0,
						Ln::Amount::msat(hmin),
						Ln::Amount::msat(hmax),
						Ln::Amount::msat(base),
						std::uint32_t(prop),
						std::uint16_t(cltv)
					});
			}
			tx.commit();

			return bus.raise(std::move(resp));
		});
	}

	Ev::Io<void> prune() {
		auto now = std::uint64_t(get_now());
		auto cutoff = (now > retain_secs) ? now - retain_secs
						  : std::uint64_t(0);
		return db.transact().then([cutoff](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			DELETE FROM "AskreneNodeDisableUpdates"
			 WHERE time < :cutoff;
			)QRY")
				.bind(":cutoff", cutoff)
				.execute()
				;
			tx.query(R"QRY(
			DELETE FROM "AskreneChannelUpdates"
			 WHERE time < :cutoff;
			)QRY")
				.bind(":cutoff", cutoff)
				.execute()
				;
			tx.commit();
			return Ev::lift();
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl(S::Bus& bus_, std::function<double()> get_now_)
		: bus(bus_)
		, get_now(std::move(get_now_))
		, node_disable_age_secs(default_node_disable_age_secs)
		, channel_update_age_secs(default_channel_update_age_secs)
		, retain_secs(default_retain_secs) { start(); }
};

/* ~~~~ static projection helpers ~~~~ */

Ev::Io<std::string>
AskreneUpdates::open_layer( Boss::Mod::Rpc& rpc
			  , Boss::Msg::ResponseAskreneUpdates const& updates
			  ) {
	auto layer = std::string("clboss-updates-tmp-")
		   + std::string(Uuid::random());
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
			.field("persistent", false)
		.end_object()
		;
	return rpc.command( "askrene-create-layer"
			  , std::move(parms)
			  ).then([&rpc, layer, updates](Jsmn::Object) {
		auto chain = Ev::lift();
		for (auto const& node : updates.node_disables)
			chain = std::move(chain)
			      + Boss::Mod::AskreneLayer::disable_node(
					rpc, layer, node);
		for (auto const& cu : updates.channel_updates)
			chain = std::move(chain)
			      + Boss::Mod::AskreneLayer::update_channel(
					rpc, layer,
					cu.scid, cu.direction, cu.enabled,
					cu.htlc_minimum_msat,
					cu.htlc_maximum_msat,
					cu.fee_base_msat,
					cu.fee_proportional_millionths,
					cu.cltv_expiry_delta);
		return std::move(chain).then([layer]() {
			return Ev::lift(layer);
		});
	}).catching<RpcError>([](RpcError const&) {
		/* create-layer failed (e.g. CLN < v24.11 has no askrene):
		 * return an empty name so the caller omits it from its
		 * getroutes layers array rather than naming a layer that
		 * does not exist -- the exact condition that aborts the
		 * (important) askrene plugin.  Degraded (no update
		 * projection this call), never a crash.  */
		return Ev::lift(std::string());
	});
}

Ev::Io<void>
AskreneUpdates::close_layer( Boss::Mod::Rpc& rpc
			   , std::string const& layer
			   ) {
	auto parms = Json::Out()
		.start_object()
			.field("layer", layer)
		.end_object()
		;
	return rpc.command( "askrene-remove-layer"
			  , std::move(parms)
			  ).then([](Jsmn::Object) {
		return Ev::lift();
	}).catching<RpcError>([](RpcError const&) {
		/* Best-effort: the layer is non-persistent and private to
		 * this finished request; a failed remove leaks nothing that
		 * a restart would not clear.  */
		return Ev::lift();
	});
}

AskreneUpdates::AskreneUpdates(AskreneUpdates&&) =default;
AskreneUpdates::~AskreneUpdates() =default;

AskreneUpdates::AskreneUpdates( S::Bus& bus
			      , std::function<double()> get_now_
			      )
	: pimpl(Util::make_unique<Impl>(bus, get_now_)) { }

}}
