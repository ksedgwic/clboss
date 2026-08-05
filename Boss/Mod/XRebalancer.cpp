#include"Boss/Mod/XRebalancer.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/RebalanceModeProxy.hpp"
#include"Boss/ModG/RpcProxy.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/DemandObserved.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
#include"Boss/RebalanceMode.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Boss/random_engine.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include"Util/make_unique.hpp"
#include<algorithm>
#include<cmath>
#include<ctime>
#include<limits>
#include<map>
#include<random>
#include<sstream>
#include<string>
#include<vector>

namespace {

auto const opt_per_hour = std::string("clboss-xrebalance-per-hour");
auto const opt_floor = std::string("clboss-xrebalance-route-cost-floor");
auto const opt_window_days = std::string("clboss-xrebalance-earnings-window-days");
auto const opt_fill_loc = std::string("clboss-xrebalance-fill-loc");
auto const opt_drain_loc = std::string("clboss-xrebalance-drain-loc");
auto const opt_maxparts = std::string("clboss-xrebalance-maxparts");
auto const opt_grant = std::string("clboss-xrebalance-grant");
auto const opt_gain = std::string("clboss-xrebalance-gain");

auto constexpr default_per_hour = double(12.0);
auto constexpr default_floor = double(50.0);
auto constexpr default_window_days = double(90.0);
/* Tier bands (Loc%); match clboss-xrebalance-view defaults.  */
auto constexpr default_fill_band = double(10.0);
auto constexpr default_drain_band = double(90.0);
/* MCF split cap passed to the executor (askrene getroutes maxparts).  */
auto constexpr default_maxparts = double(10.0);
/* The fill/drain deficits aim toward the band edges themselves
 * (fill_band / drain_band): the band both admits a channel and
 * defines where its rebalancing stops.  */

/* Strictness benders, both neutral by default.  grant credits every
 * channeled peer an assumed prior of grant ppm on one capacity-turn
 * of volume; gain multiplies the joined NetPpm.  */
auto constexpr default_grant = double(0.0);
auto constexpr default_gain = double(1.0);

auto constexpr paused_poll_secs = double(60.0);

}

namespace Boss { namespace Mod {

class XRebalancer::Impl {
private:
	S::Bus& bus;
	Waiter& waiter;
	ModG::RebalanceModeProxy mode_proxy;
	ModG::RpcProxy rpc;
	Sqlite3::Db db;

	double per_hour;
	double floor_ppm;
	double window_days;
	double fill_band;
	double drain_band;
	std::uint32_t maxparts;   /* MCF split cap (integer count) */
	double grant_ppm;         /* assumed prior rate (ppm of a capacity-turn) */
	double gain;              /* NetPpm multiplier */
	bool floor_auto;   /* floor option set to "auto" (sweep) */
	/* Mode xrebalance2: execute through the external xrebalance
	 * plugin instead of clboss-xmovefunds.  Captured at cycle start;
	 * cycles are serialized by the awaited loop, so one flag
	 * suffices.  */
	bool use_plugin;
	bool started;
	/* True while a cycle (matched or demand) runs.  The Poisson
	 * loop and demand triggers exclude each other through it, and
	 * a demand trigger arriving while it is set is discarded --
	 * traffic recurrence re-arms real demand, so there is no
	 * queue.  Both cycle paths clear it behind a catch-all, so an
	 * exception cannot leave it wedged.  */
	bool in_flight;

	/* One row per CHANNELD_NORMAL channel, built live from
	 * listpeerchannels each cycle (balances and online status must be
	 * current, not a cached snapshot).  NetPpm is joined per-node from
	 * the EarningsTracker table.  Amounts in sat (the view works in sat).
	 */
	struct Chan {
		std::string scid;
		Ln::NodeId node;
		bool online;
		std::int64_t cap_sat;
		std::int64_t local_sat;
	};

	/* Per-node windowed NetPpm; absent (has_* false) when no forwards in
	 * that direction over the window -> excluded from the pool.  */
	struct NetPpm {
		bool has_in = false;  double in_net = 0.0;
		bool has_out = false; double out_net = 0.0;
	};

	void start() {
		per_hour = default_per_hour;
		floor_ppm = default_floor;
		window_days = default_window_days;
		fill_band = default_fill_band;
		drain_band = default_drain_band;
		maxparts = std::uint32_t(default_maxparts);
		grant_ppm = default_grant;
		gain = default_gain;
		floor_auto = false;
		use_plugin = false;
		started = false;
		in_flight = false;

		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& m) {
			db = m.db;
			return Ev::lift();
		});

		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return manifest_option(opt_per_hour, default_per_hour,
				"Average number of flow-rebalance (xrebalance) "
				"cycles per hour (0 = pause matched cycles; "
				"demand-triggered cycles still run).  "
				"Poisson-paced; only active when the "
				"rebalancer mode is \"xrebalance\" or \"xrebalance2\".")
			     + manifest_option(opt_floor, default_floor,
				"Route-cost floor (ppm): stop growing the "
				"matched-pool cycle once the marginal joint "
				"NetPpm drops below this.  Sets the derived "
				"amount and the maxfee budget.  Or \"auto\": "
				"each cycle picks a random rung of the derived "
				"floor ladder (sweep).")
			     + manifest_option(opt_window_days, default_window_days,
				"Trailing window (days) over which per-channel "
				"NetPpm is measured for cycle selection.")
			     + manifest_option(opt_fill_loc, default_fill_band,
				"Fill-tier band: channels with Loc% <= this are "
				"fill candidates (funds pushed toward them), and "
				"this is also the Loc% their fills aim for.")
			     + manifest_option(opt_drain_loc, default_drain_band,
				"Drain-tier band: channels with Loc% >= this are "
				"drain candidates (funds pulled from them), and "
				"this is also the Loc% their drains aim for.")
			     + manifest_option(opt_maxparts, default_maxparts,
				"Max parts (paths) MCF may split a cycle into "
				"(askrene getroutes maxparts).  Lower = fewer, "
				"fatter parts that amortize the base fee and pass "
				"the per-part gate but need more liquidity; "
				"higher = finer splitting, more learning, more "
				"refusals.")
			     + manifest_option(opt_grant, default_grant,
				"Assumed prior earnings rate (ppm), credited "
				"to every channeled peer on both sides as if "
				"it had already earned that rate on one "
				"capacity-turn of volume.  Admits peers with "
				"no track record at exactly this rate; "
				"expenditures spend the credit down (subsidy "
				"per peer bounded by grant x capacity) and "
				"real volume dilutes it toward the measured "
				"rate.  0 = record-only (default).")
			     + manifest_option(opt_gain, default_gain,
				"Multiplier (> 0) on the joined NetPpm, both "
				"sides, before candidacy, floor, and maxfee "
				"pricing.  >1 accepts routes costing up to "
				"gain x the measured earnings rate; 1 = "
				"strict (default).");
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			return handle_option(o);
		});


		bus.subscribe<Msg::DemandObserved
			     >([this](Msg::DemandObserved const& m) {
			return handle_demand(m);
		});

		bus.subscribe<Msg::Init
			     >([this](Msg::Init const& _) {
			if (started)
				return Ev::lift();
			started = true;
			return Boss::log( bus, Info
					, "XRebalancer: driver started "
					  "(%.2f cycles/hr avg)."
					, per_hour
					).then([this]() {
				return Boss::concurrent(loop());
			});
		});
	}

	Ev::Io<void> manifest_option( std::string const& name
				    , double dflt
				    , std::string desc
				    ) {
		return bus.raise(Msg::ManifestOption{
			name, Msg::OptionType_String,
			Json::Out::direct(dflt), std::move(desc),
			true /* dynamic */
		});
	}

	Ev::Io<void> handle_option(Msg::Option const& o) {
		/* The floor option also accepts "auto": instead of a fixed
		 * value, each cycle picks a random rung of the derived floor
		 * ladder, sweeping the whole ladder over many cycles. */
		if (o.name == opt_floor && std::string(o.value) == "auto") {
			floor_auto = true;
			return Boss::log( bus, Info
				, "XRebalancer: %s set to \"auto\" "
				  "(per-cycle random sweep of the floor ladder)."
				, o.name.c_str() );
		}
		if (o.name == opt_floor)
			floor_auto = false;
		/* maxparts is an integer count, not a continuous knob, so it
		 * gets dedicated handling: parse, round, floor at 1 (askrene
		 * requires >= 1), and store as an integer.  */
		if (o.name == opt_maxparts) {
			auto s = std::string(o.value);
			auto v = std::nan("");
			try {
				v = std::stod(s);
			} catch (std::exception const&) { }
			/* stod accepts "nan"/"inf", and casting a
			 * non-finite or out-of-range double to uint32 is
			 * undefined -- fold those into the invalid path
			 * and clamp the survivors before the cast.  */
			if (!std::isfinite(v)) {
				o.reject(o.name + ": not a valid number");
				return Boss::log( bus, Error
						, "XRebalancer: ignoring invalid "
						  "%s value \"%s\"."
						, o.name.c_str(), s.c_str() );
			}
			v = std::round(v);
			if (v < 1.0) v = 1.0;
			if (v > 1000000.0) v = 1000000.0;
			maxparts = std::uint32_t(v);
			return Boss::log( bus, Info
					, "XRebalancer: %s set to %u."
					, o.name.c_str(), maxparts );
		}
		double* target = nullptr;
		if (o.name == opt_per_hour)        target = &per_hour;
		else if (o.name == opt_floor)      target = &floor_ppm;
		else if (o.name == opt_window_days)target = &window_days;
		else if (o.name == opt_fill_loc)   target = &fill_band;
		else if (o.name == opt_drain_loc)  target = &drain_band;
		else if (o.name == opt_grant)      target = &grant_ppm;
		else if (o.name == opt_gain)       target = &gain;
		else return Ev::lift();

		auto s = std::string(o.value);
		auto v = std::nan("");
		try {
			v = std::stod(s);
		} catch (std::exception const&) { }
		/* Non-finite folds into the invalid path: stod accepts
		 * "nan"/"inf", and a NaN would slip through every
		 * clamping comparison below (all false) straight into
		 * the stored setting.  */
		if (!std::isfinite(v)) {
			o.reject(o.name + ": not a valid number");
			return Boss::log( bus, Error
					, "XRebalancer: ignoring invalid %s "
					  "value \"%s\"."
					, o.name.c_str(), s.c_str()
					);
		}
		if (o.name == opt_gain) {
			if (!(v > 0.0)) {
				o.reject(o.name + ": must be > 0");
				return Boss::log( bus, Error
						, "XRebalancer: %s must be > 0; "
						  "ignoring \"%s\"."
						, o.name.c_str(), s.c_str()
						);
			}
		} else if (o.name == opt_fill_loc || o.name == opt_drain_loc) {
			if (v < 0.0)        v = 0.0;
			else if (v > 100.0) v = 100.0;
		} else if (v < 0.0) {
			v = 0.0;
		}
		*target = v;
		return Boss::log( bus, Info
				, "XRebalancer: %s set to %.4g."
				, o.name.c_str(), v
				);
	}

	/* Parse a live listpeerchannels result into per-channel rows.  */
	std::vector<Chan> build_chans(Jsmn::Object res) {
		auto out = std::vector<Chan>();
		try {
			if (!res.is_object() || !res.has("channels"))
				return out;
			auto channels = res["channels"];
			if (!channels.is_array())
				return out;
			for (auto i = std::size_t(0); i < channels.size(); ++i) {
				auto c = channels[i];
				if (!c.has("state")
				 || std::string(c["state"]) != "CHANNELD_NORMAL")
					continue;
				if (!c.has("short_channel_id")
				 || !c.has("peer_id")
				 || !c.has("to_us_msat")
				 || !c.has("total_msat"))
					continue;
				auto cap = std::int64_t(
				    Ln::Amount::object(c["total_msat"])
				    .to_msat() / 1000);
				auto loc = std::int64_t(
				    Ln::Amount::object(c["to_us_msat"])
				    .to_msat() / 1000);
				if (cap <= 0)
					continue;
				auto online = c.has("peer_connected")
					   && c["peer_connected"].is_boolean()
					   && bool(c["peer_connected"]);
				out.push_back(Chan{
					std::string(c["short_channel_id"]),
					Ln::NodeId(std::string(c["peer_id"])),
					online, cap, loc
				});
			}
		} catch (std::exception const& e) {
			(void) Boss::concurrent(Boss::log( bus, Error
				, "XRebalancer: failed to parse "
				  "listpeerchannels: %s"
				, e.what()
				));
		}
		return out;
	}

	double next_delay_secs() {
		if (per_hour <= 0.0)
			return paused_poll_secs;
		auto rate_per_sec = per_hour / 3600.0;
		auto dist = std::exponential_distribution<double>(rate_per_sec);
		return dist(Boss::random_engine);
	}

	Ev::Io<void> loop() {
		return Ev::lift().then([this]() {
			return waiter.wait(next_delay_secs());
		}).then([this]() {
			return tick();
		}).then([this]() {
			return loop();
		});
	}

	Ev::Io<void> tick() {
		/* At rate 0 next_delay_secs() degrades to a short poll
		 * so a setconfig re-enable is noticed; the poll itself
		 * must not run a cycle.  Demand cycles are deliberately
		 * unaffected -- clboss-rebalance-mode=off is the full
		 * disable.  */
		if (per_hour <= 0.0)
			return Ev::lift();
		if (in_flight)
			return Boss::log( bus, Debug
					, "XRebalancer: tick skipped, cycle "
					  "in flight." );
		in_flight = true;
		return mode_proxy.get_mode().then([this](RebalanceMode m) {
			if ( m != RebalanceMode::xrebalance
			  && m != RebalanceMode::xrebalance2 )
				return Boss::log( bus, Debug
						, "XRebalancer: idle (mode is "
						  "\"%s\", not \"xrebalance\" "
						  "or \"xrebalance2\")."
						, rebalance_mode_to_string(m)
						);
			use_plugin = (m == RebalanceMode::xrebalance2);
			return run_cycle();
		/* The catch-all before the clear is load-bearing twice
		 * over: an exception on the fail path would skip a bare
		 * .then clear (wedging in_flight for good), and it would
		 * kill the awaiting loop greenthread outright.  */
		}).catching<std::exception>([this](std::exception const& e) {
			return Boss::log( bus, Warn
					, "XRebalancer: cycle error: %s"
					, e.what() );
		}).then([this]() {
			in_flight = false;
			return Ev::lift();
		});
	}

	/* Demand triggers (DemandTracker's htlc_accepted deferrer, via
	 * Msg::DemandObserved) run in the forwarding hook path, so the
	 * synchronous part stays cheap: the busy test and the spawn.
	 * Check and set have no await between them, so concurrent
	 * triggers cannot both pass.  */
	Ev::Io<void> handle_demand(Msg::DemandObserved const& m) {
		if (!started || in_flight)
			return Ev::lift();
		in_flight = true;
		return Boss::concurrent(
			demand_cycle(std::string(m.out_scid)));
	}

	/* A demand-triggered cycle: the same pipeline as a matched one,
	 * routed to the demand plan by the scid argument.  Runs outside
	 * the loop greenthread, so it carries its own mode gate and
	 * catch-all.  */
	Ev::Io<void> demand_cycle(std::string scid) {
		return mode_proxy.get_mode().then([this, scid](RebalanceMode m) {
			if ( m != RebalanceMode::xrebalance
			  && m != RebalanceMode::xrebalance2 )
				return Ev::lift();
			use_plugin = (m == RebalanceMode::xrebalance2);
			return run_cycle(scid);
		}).catching<std::exception>([this](std::exception const& e) {
			return Boss::log( bus, Warn
					, "XRebalancer: demand cycle error: %s"
					, e.what() );
		}).then([this]() {
			in_flight = false;
			return Ev::lift();
		});
	}

	/* Fetch live balances/online (listpeerchannels), query the windowed
	 * per-node NetPpm, join, derive the cycle, execute.  A non-empty
	 * demand_scid routes planning to the demand style (the channel a
	 * forward just exited through); empty runs the matched style.  */
	Ev::Io<void> run_cycle(std::string demand_scid = "") {
		return rpc.command( "listpeerchannels"
				  , Json::Out::empty_object()
				  ).then([this, demand_scid](Jsmn::Object res) {
			return run_cycle_with(std::make_shared<std::vector<Chan>>(
				build_chans(res)), demand_scid);
		});
	}

	Ev::Io<void>
	run_cycle_with( std::shared_ptr<std::vector<Chan>> chans
		      , std::string demand_scid
		      ) {
		if (chans->empty())
			return Boss::log( bus, Info
					, "XRebalancer: no channel data, "
					  "skipping cycle." );
		auto cutoff = double(std::time(nullptr))
			    - window_days * 24.0 * 60.0 * 60.0;
		return db.transact().then([ this, cutoff, chans, demand_scid
					  ](Sqlite3::Tx tx) {
			auto net = std::make_shared<std::map<Ln::NodeId, NetPpm>>();
			/* Per-peer capacity (msat): grant's credit base and
			 * notional volume.  */
			auto cap_msat = std::map<Ln::NodeId, double>();
			for (auto const& c : *chans)
				cap_msat[c.node] += double(c.cap_sat) * 1000.0;
			/* Join one side.  Strict form is (e - x) / f over
			 * f > 0.  With grant, credit the peer as if it had
			 * already earned grant ppm on one capacity-turn:
			 * (e - x + cap*grant/1e6) / (f + cap) -- a fresh
			 * peer reads exactly grant, expenditures spend the
			 * credit down, real volume dilutes it toward the
			 * measured rate.  gain scales the result either
			 * way.  */
			auto joined = [this]( double e, double x, double f
					    , double cm
					    , bool& has, double& ppm
					    ) {
				auto g = grant_ppm > 0.0 ? cm : 0.0;
				if (!(f + g > 0.0))
					return;
				has = true;
				ppm = (e - x + g * grant_ppm / 1e6)
				    * 1e6 / (f + g) * gain;
			};
			auto fetch = tx.query(R"QRY(
			SELECT node,
			       SUM(in_earnings), SUM(in_forwarded),
			       SUM(in_expenditures),
			       SUM(out_earnings), SUM(out_forwarded),
			       SUM(out_expenditures)
			  FROM "EarningsTracker"
			 WHERE time_bucket >= :cutoff
			 GROUP BY node;
			)QRY")
				.bind(":cutoff", cutoff)
				.execute();
			for (auto& r : fetch) {
				auto node = Ln::NodeId(r.get<std::string>(0));
				auto in_e = double(r.get<std::int64_t>(1));
				auto in_f = double(r.get<std::int64_t>(2));
				auto in_x = double(r.get<std::int64_t>(3));
				auto out_e = double(r.get<std::int64_t>(4));
				auto out_f = double(r.get<std::int64_t>(5));
				auto out_x = double(r.get<std::int64_t>(6));
				auto cm = 0.0;
				auto ci = cap_msat.find(node);
				if (ci != cap_msat.end())
					cm = ci->second;
				auto p = NetPpm();
				joined(in_e, in_x, in_f, cm, p.has_in, p.in_net);
				joined(out_e, out_x, out_f, cm,
				       p.has_out, p.out_net);
				(*net)[node] = p;
			}
			/* Channeled peers with no earnings rows at all read
			 * the pure grant rate.  */
			if (grant_ppm > 0.0)
				for (auto const& ce : cap_msat) {
					if (net->count(ce.first))
						continue;
					auto p = NetPpm();
					joined(0.0, 0.0, 0.0, ce.second,
					       p.has_in, p.in_net);
					joined(0.0, 0.0, 0.0, ce.second,
					       p.has_out, p.out_net);
					(*net)[ce.first] = p;
				}
			tx.commit();
			return plan_and_log(chans, net, demand_scid);
		});
	}

	/* Per-peer aggregate over the peer's channels.  The network only
	 * guarantees delivery to the PEER: non-strict forwarding (BOLT 4)
	 * lets it land an incoming HTLC on any parallel channel, so a
	 * per-channel fill deficit against a multi-channel peer can never
	 * be settled and the planner livelocks re-requesting it (observed
	 * live: two-channel peer, three completed fills, zero deficit
	 * movement).  Candidacy, deficits, and progress therefore live at
	 * peer granularity -- the same granularity as NetPpm and the
	 * EarningsTracker -- while chans carries all the peer's channels
	 * for the request lists (sources we control exactly; destinations
	 * the peer resolves anyway).  */
	struct Peer {
		Ln::NodeId node;
		std::vector<Chan> chans;
		std::int64_t cap_sat = 0;
		std::int64_t local_sat = 0;
		bool online = false;
		double pct_local = 0.0;
		std::int64_t tgt_fill_sat = 0;
		std::int64_t tgt_drain_sat = 0;
	};

	/* One request-list entry: a channel plus the most this cycle may
	 * move through it, passed to the xrebalance plugin as its
	 * per-scid max_msat cap.  */
	struct ScidCap {
		std::string scid;
		std::int64_t max_sat;
	};

	/* A pool member: aggregated peer plus its joined NetPpm on the
	 * relevant side, and the peer's deficit distributed over its
	 * channels as per-scid caps.  */
	struct PoolItem {
		Peer const* pr;
		double ppm;       /* out_net for fill, in_net for drain */
		std::int64_t deficit; /* tgt_fill for fill, tgt_drain for drain */
		std::vector<ScidCap> caps;
	};

	/* Caps below this are dropped (and their channel with them):
	 * below askrene's ~1000-sat single-path threshold a cap smaller
	 * than the amount excludes the channel from the solve anyway, so
	 * tiny caps are pure noise in the request.  */
	static constexpr std::int64_t min_cap_sat = 1000;

	/* Distribute a peer-level band deficit over the peer's channels
	 * as per-scid caps: cap_i = deficit * h_i / sum(h), where h_i is
	 * the channel's own headroom past the band edge.  The SUM of the
	 * caps never exceeds the peer's deficit -- per-peer granularity
	 * is the doctrine above, and with xrebalance enforcing the caps
	 * the peer cannot be pushed past its band edge in one cycle no
	 * matter how the MCF concentrates the flow -- while the
	 * weighting points the flow at the peer's skewed channels.  */
	static std::vector<ScidCap>
	distribute_caps( Peer const& p
		       , double band_pct
		       , bool fill_side
		       , std::int64_t deficit_sat
		       ) {
		auto h = std::vector<std::int64_t>(p.chans.size());
		auto total = double(0.0);
		for (auto i = std::size_t(0); i < p.chans.size(); ++i) {
			auto const& c = p.chans[i];
			auto edge = std::int64_t(
				double(c.cap_sat) * band_pct / 100.0);
			auto v = fill_side ? edge - c.local_sat
					   : c.local_sat - edge;
			h[i] = v > 0 ? v : 0;
			total += double(h[i]);
		}
		auto out = std::vector<ScidCap>();
		if (total <= 0.0 || deficit_sat <= 0)
			return out;
		for (auto i = std::size_t(0); i < p.chans.size(); ++i) {
			if (h[i] <= 0)
				continue;
			auto cap = std::int64_t(
				double(deficit_sat) * double(h[i]) / total);
			if (cap < min_cap_sat)
				continue;
			out.push_back(ScidCap{p.chans[i].scid, cap});
		}
		return out;
	}

	/* One point on the joint(N) curve: cumulative matched volume N and
	 * the marginal fill/drain NetPpm (and their sum) admitted at that
	 * depth.  Mirrors the curve clboss-xrebalance-view prints.  */
	struct CurvePoint {
		std::int64_t n;
		double fill_ppm;
		double drain_ppm;
		double joint;
	};

	/* Node-agnostic floor ladder, ported from clboss-xrebalance-view and
	 * kept in sync deliberately -- the view is the reference and the
	 * explanatory artifact.  Floors are log-spaced on the joint (= budget)
	 * axis between the ceiling (top row) and the useful floor (lowest row
	 * where both marginal sides still clear NOISE_PPM net), so the rung
	 * count auto-scales with the node's span.  Targets are snapped to real
	 * curve rows, deduped, and held at least MIN_GAP apart.  Constants are
	 * dimensionless; see the view for the rationale.  */
	std::vector<CurvePoint>
	derive_ladder(std::vector<CurvePoint> const& curve) {
		auto ladder = std::vector<CurvePoint>();
		if (curve.empty())
			return ladder;
		auto constexpr LADDER_RATIO = double(1.6);
		auto constexpr NOISE_PPM = double(10.0);
		auto constexpr MIN_GAP = double(1.25);
		auto ceiling_joint = curve[0].joint;
		auto useful_idx = std::size_t(0);
		for (auto i = std::size_t(0); i < curve.size(); ++i) {
			if (curve[i].fill_ppm >= NOISE_PPM
			 && curve[i].drain_ppm >= NOISE_PPM)
				useful_idx = i;
			else
				break;
		}
		auto useful_joint = curve[useful_idx].joint;
		auto targets = std::vector<double>();
		for (auto t = ceiling_joint; t > useful_joint; t /= LADDER_RATIO)
			targets.push_back(t);
		targets.push_back(useful_joint);
		auto already = [&ladder](std::int64_t n) {
			for (auto const& p : ladder)
				if (p.n == n)
					return true;
			return false;
		};
		auto last_joint = double(-1.0);
		for (auto tgt : targets) {
			/* Snap to the row a floor=tgt would select: the
			 * largest N (within the useful range) whose joint is
			 * still >= tgt.  */
			auto pick = curve[0];
			for (auto i = std::size_t(0); i <= useful_idx; ++i) {
				if (curve[i].joint >= tgt)
					pick = curve[i];
				else
					break;
			}
			if (already(pick.n))
				continue;
			if (last_joint > 0.0 && pick.joint > last_joint / MIN_GAP)
				continue;
			ladder.push_back(pick);
			last_joint = pick.joint;
		}
		if (!already(curve[useful_idx].n))
			ladder.push_back(curve[useful_idx]);
		return ladder;
	}

	Ev::Io<void>
	plan_and_log( std::shared_ptr<std::vector<Chan>> chans
		    , std::shared_ptr<std::map<Ln::NodeId, NetPpm>> net
		    , std::string const& demand_scid
		    ) {
		/* Aggregate channels into peers; deficits aim at the band
		 * edges on the aggregate Loc%.  A peer with one full and
		 * one empty channel nets out balanced and is left alone --
		 * correct, since intra-peer skew is exactly what non-strict
		 * forwarding puts beyond our control.  */
		auto peers = std::vector<Peer>();
		{
			auto by_node = std::map<Ln::NodeId, Peer>();
			for (auto const& c : *chans) {
				auto& p = by_node[c.node];
				p.node = c.node;
				p.chans.push_back(c);
				p.cap_sat += c.cap_sat;
				p.local_sat += c.local_sat;
				p.online = p.online || c.online;
			}
			for (auto& e : by_node) {
				auto& p = e.second;
				if (p.cap_sat <= 0)
					continue;
				p.pct_local = 100.0 * double(p.local_sat)
					    / double(p.cap_sat);
				auto tf = std::int64_t(double(p.cap_sat)
					* fill_band / 100.0) - p.local_sat;
				auto td = p.local_sat - std::int64_t(
					double(p.cap_sat) * drain_band / 100.0);
				p.tgt_fill_sat = tf > 0 ? tf : 0;
				p.tgt_drain_sat = td > 0 ? td : 0;
				peers.push_back(std::move(p));
			}
		}

		auto fill = std::vector<PoolItem>();
		auto drain = std::vector<PoolItem>();
		for (auto const& p : peers) {
			if (!p.online)
				continue;
			auto it = net->find(p.node);
			if (it == net->end())
				continue;
			auto const& np = it->second;
			if (p.pct_local <= fill_band
			 && np.has_out && np.out_net > 0.0
			 && p.tgt_fill_sat > 0) {
				auto caps = distribute_caps(
					p, fill_band, true, p.tgt_fill_sat);
				if (!caps.empty())
					fill.push_back(PoolItem{
						&p, np.out_net,
						p.tgt_fill_sat,
						std::move(caps) });
			}
			/* else: with overlapping bands (fill-loc set above
			 * drain-loc) a peer could qualify for both pools;
			 * fill wins so it cannot be picked against itself.  */
			else if (p.pct_local >= drain_band
			 && np.has_in && np.in_net > 0.0
			 && p.tgt_drain_sat > 0) {
				auto caps = distribute_caps(
					p, drain_band, false, p.tgt_drain_sat);
				if (!caps.empty())
					drain.push_back(PoolItem{
						&p, np.in_net,
						p.tgt_drain_sat,
						std::move(caps) });
			}
		}
		std::sort(fill.begin(), fill.end(),
			[](PoolItem const& a, PoolItem const& b){
				return a.ppm > b.ppm; });
		std::sort(drain.begin(), drain.end(),
			[](PoolItem const& a, PoolItem const& b){
				return a.ppm > b.ppm; });

		if (fill.empty() || drain.empty()) {
			/* Demand evaluations run per forward, so their
			 * no-op outcomes log at Debug; the paced matched
			 * cycle keeps the Info line.  */
			if (!demand_scid.empty())
				return Boss::log( bus, Debug
					, "XRebalancer: demand on %s: no "
					  "cycle -- NO_CANDIDATES (fill=%zu "
					  "drain=%zu)."
					, demand_scid.c_str()
					, fill.size(), drain.size() );
			return Boss::log( bus, Info
				, "XRebalancer: no cycle -- NO_CANDIDATES "
				  "(fill=%zu drain=%zu; bands fill<=%.1f "
				  "drain>=%.1f, window=%.0fd)."
				, fill.size(), drain.size()
				, fill_band, drain_band, window_days );
		}

		if (!demand_scid.empty())
			return plan_demand(fill, drain, demand_scid);

		/* Cumulative deficit + marginal ppm per side.  */
		auto cum = [](std::vector<PoolItem> const& pool){
			auto v = std::vector<std::pair<std::int64_t,double>>();
			std::int64_t acc = 0;
			for (auto const& it : pool) {
				acc += it.deficit;
				v.push_back({acc, it.ppm});
			}
			return v;
		};
		auto fc = cum(fill);
		auto dc = cum(drain);
		auto threshold_at = [](
			std::vector<std::pair<std::int64_t,double>> const& c,
			std::int64_t target) -> double {
			for (auto const& e : c)
				if (target <= e.first)
					return e.second;
			return -1.0; /* exhausted */
		};

		/* Breakpoints: every cumulative volume on either side.  Pick
		 * the largest N whose joint marginal NetPpm >= floor.  joint is
		 * non-increasing in N, so walk ascending and stop on drop.  */
		auto bps = std::vector<std::int64_t>();
		for (auto const& e : fc) bps.push_back(e.first);
		for (auto const& e : dc) bps.push_back(e.first);
		std::sort(bps.begin(), bps.end());
		bps.erase(std::unique(bps.begin(), bps.end()), bps.end());

		/* Full joint(N) curve (every breakpoint), as the view computes
		 * it -- we no longer stop at the floor, so the whole curve is
		 * available for the ladder and for logging.  */
		auto curve = std::vector<CurvePoint>();
		for (auto n : bps) {
			auto f = threshold_at(fc, n);
			auto d = threshold_at(dc, n);
			if (f < 0.0 || d < 0.0)
				break; /* one side exhausted */
			curve.push_back(CurvePoint{ n, f, d, f + d });
		}

		/* Derive the ladder every cycle (cheap) so the levels are
		 * logged and can be watched moving as balances/constraints
		 * shift.  In "auto" mode the cut is a random rung; otherwise
		 * the configured fixed floor.  */
		auto ladder = derive_ladder(curve);
		auto effective_floor = floor_ppm;
		auto picked_note = std::string();
		if (floor_auto && !ladder.empty()) {
			auto dist = std::uniform_int_distribution<std::size_t>(
				0, ladder.size() - 1);
			auto idx = dist(Boss::random_engine);
			effective_floor = ladder[idx].joint;
			auto os = std::ostringstream();
			os << " (auto picked "
			   << (long long)std::llround(effective_floor) << ")";
			picked_note = os.str();
		}

		/* One greppable line per cycle listing the ladder rungs, so the
		 * levels can be tracked over time.  */
		auto levels = std::ostringstream();
		levels << "XRebalancer: floor levels [" << ladder.size()
		       << " rungs]: ";
		for (auto i = std::size_t(0); i < ladder.size(); ++i) {
			if (i) levels << "/";
			levels << (long long)std::llround(ladder[i].joint);
		}
		levels << " ppm" << (floor_auto ? "" : " (fixed floor)");
		auto levels_str = levels.str();

		/* Select the cut: largest curve row whose joint clears the
		 * chosen floor (joint is non-increasing, so stop on drop).  */
		std::int64_t best_n = 0;
		double best_fill_ppm = 0.0, best_drain_ppm = 0.0;
		double best_joint = 0.0;
		for (auto const& pt : curve) {
			if (pt.joint >= effective_floor) {
				best_n = pt.n;
				best_fill_ppm = pt.fill_ppm;
				best_drain_ppm = pt.drain_ppm;
				best_joint = pt.joint;
			} else {
				break;
			}
		}

		if (best_n <= 0)
			return Boss::log( bus, Info, "%s", levels_str.c_str() )
			     + Boss::log( bus, Info
				, "XRebalancer: no viable cycle -- no matched "
				  "volume clears floor %.1f ppm "
				  "(fill=%zu drain=%zu, window=%.0fd)."
				, effective_floor, fill.size(), drain.size()
				, window_days );

		/* Bold set: peers accumulated to reach best_n on each side;
		 * every capped channel of a picked peer joins the request
		 * list.  */
		auto pick = [best_n](std::vector<PoolItem> const& pool){
			auto picks = std::vector<ScidCap>();
			std::int64_t acc = 0;
			for (auto const& it : pool) {
				if (acc >= best_n)
					break;
				picks.insert( picks.end()
					    , it.caps.begin(), it.caps.end());
				acc += it.deficit;
			}
			return picks;
		};
		auto dest_caps = pick(fill);    /* fill = where funds land */
		auto source_caps = pick(drain); /* drain = where funds leave */

		/* Request the full matched volume; the per-channel caps in
		 * the request lists bound what each channel absorbs.  */
		auto requested = best_n;
		auto maxfee = std::uint32_t(std::llround(best_joint));

		return Boss::log( bus, Info, "%s", levels_str.c_str() )
		     + Boss::log( bus, Info
			, "XRebalancer: cycle [matched] floor=%.1f%s window=%.0fd "
			  "-> request=%s sat (matched volume), joint=%.1f ppm "
			  "(fill>=%.1f + drain>=%.1f), maxfee %u ppm; "
			  "sources=%zu dests=%zu; executing."
			, effective_floor, picked_note.c_str(), window_days
			, Util::Str::group_digits(
				std::int64_t(requested)).c_str(), best_joint
			, best_fill_ppm, best_drain_ppm
			, (unsigned)maxfee
			, source_caps.size(), dest_caps.size()
			).then([this, source_caps, dest_caps]() {
			return Boss::log( bus, Debug
				, "XRebalancer:   sources=[%s] dests=[%s]"
				, join_caps(source_caps).c_str()
				, join_caps(dest_caps).c_str()
				);
		}).then([this, source_caps, dest_caps, requested, maxfee]() {
			return execute_cycle(source_caps, dest_caps,
					     requested, maxfee);
		});
	}

	/* Demand cycle: the target is the peer whose channel a forward
	 * just exited through.  Fill-pool membership is the entire
	 * criterion -- demand controls WHEN we rebalance, never who
	 * qualifies, how much, or the price.  Sized to the peer's
	 * deficit to the fill edge.  Sources are the top p% of the
	 * offered pool by NetPpm, and the price is the cheapest source
	 * actually offered, so every sat moved earns at least the
	 * target's side plus at least its source's side no matter the
	 * rung.  p is drawn per cycle -- the same sweep methodology as
	 * the auto floor ladder: a narrow rung offers premium sources
	 * at a rich budget, a wide rung offers most of the pool at a
	 * lean one, and demand re-triggering redraws, so which rung
	 * delivers is learned from the traffic itself.  Pricing at the
	 * pool minimum was tried first and let the most-subsidized
	 * peer (heavy past refill expenditures, near-zero net) cap
	 * every budget.  */
	Ev::Io<void>
	plan_demand( std::vector<PoolItem> const& fill
		   , std::vector<PoolItem> const& drain
		   , std::string const& scid
		   ) {
		auto target = (PoolItem const*) nullptr;
		for (auto const& it : fill) {
			for (auto const& c : it.pr->chans)
				if (c.scid == scid) {
					target = &it;
					break;
				}
			if (target)
				break;
		}
		if (!target)
			return Boss::log( bus, Debug
				, "XRebalancer: demand on %s: peer not a "
				  "fill candidate, no cycle."
				, scid.c_str() );
		static constexpr double rung_pct[] = {20.0, 50.0, 80.0};
		auto rung = rung_pct[std::uniform_int_distribution<
			std::size_t>(0, 2)(Boss::random_engine)];
		/* Pools are sorted NetPpm-descending, so the kept prefix
		 * is the top of the pool and its last element is the
		 * cheapest source actually offered.  */
		auto keep = std::size_t(std::ceil(
			double(drain.size()) * rung / 100.0));
		if (keep < 1)
			keep = 1;
		if (keep > drain.size())
			keep = drain.size();
		auto min_offered = drain[keep - 1].ppm;
		auto maxfee = std::uint32_t(std::llround(
			target->ppm + min_offered));
		auto requested = target->deficit;
		auto dest_caps = target->caps;
		auto source_caps = std::vector<ScidCap>();
		for (auto i = std::size_t(0); i < keep; ++i)
			source_caps.insert( source_caps.end()
					  , drain[i].caps.begin()
					  , drain[i].caps.end());
		return Boss::log( bus, Info
			, "XRebalancer: cycle [demand] trigger=%s target=%s "
			  "window=%.0fd -> request=%s sat (deficit to fill "
			  "edge), rung=top %.0f%% -> maxfee=%u ppm (target "
			  "%.1f + min offered %.1f); sources=%zu dests=%zu; "
			  "executing."
			, scid.c_str()
			, join_caps(target->caps).c_str()
			, window_days
			, Util::Str::group_digits(requested).c_str()
			, rung
			, (unsigned)maxfee
			, target->ppm, min_offered
			, source_caps.size(), dest_caps.size()
			).then([this, source_caps, dest_caps]() {
			return Boss::log( bus, Debug
				, "XRebalancer:   sources=[%s] dests=[%s]"
				, join_caps(source_caps).c_str()
				, join_caps(dest_caps).c_str()
				);
		}).then([this, source_caps, dest_caps, requested, maxfee]() {
			return execute_cycle(source_caps, dest_caps,
					     requested, maxfee);
		});
	}

	/* Drive the chosen cycle through the executor the mode selects:
	 * the in-clboss clboss-xmovefunds command (mode xrebalance,
	 * reusing its sendpay/waitsendpay/harvest/attribution), or the
	 * external xrebalance plugin (mode xrebalance2).  The loop
	 * awaits this, so no new cycle starts while one is in flight
	 * (the natural in-flight guard until the abandon/timeout
	 * increment lands).  */
	Ev::Io<void>
	execute_cycle( std::vector<ScidCap> source_caps
		     , std::vector<ScidCap> dest_caps
		     , std::int64_t requested_sat
		     , std::uint32_t maxfee_ppm
		     ) {
		if (use_plugin)
			return execute_cycle_plugin( std::move(source_caps)
						   , std::move(dest_caps)
						   , requested_sat
						   , maxfee_ppm
						   );
		/* clboss-xmovefunds has no per-scid cap concept; it gets
		 * the bare scids as before.  */
		auto parms = Json::Out();
		auto obj = parms.start_object();
		auto sa = obj.start_array("source_scid");
		for (auto const& s : source_caps)
			sa.entry(s.scid);
		sa.end_array();
		auto da = obj.start_array("dest_scid");
		for (auto const& s : dest_caps)
			da.entry(s.scid);
		da.end_array();
		obj.field("amount_msat",
			  std::uint64_t(requested_sat) * 1000);
		obj.field("maxfee_ppm", maxfee_ppm);
		obj.field("maxparts", maxparts);
		obj.field("execute", true);
		obj.end_object();
		return rpc.command("clboss-xmovefunds", std::move(parms))
		.then([this](Jsmn::Object res) {
			return log_result(res);
		}).catching<RpcError>([this](RpcError const& e) {
			/* Expected outcome on a tight/walled corridor (e.g.
			 * getroutes 206): log one clean line, not the
			 * BacktraceException's what().  No funds moved.  */
			return Boss::log( bus, Info
				, "XRebalancer: xmovefunds did not execute: %s"
				, rpc_error_summary(e).c_str() );
		}).catching<std::exception>([this](std::exception const& e) {
			return Boss::log( bus, Warn
				, "XRebalancer: xmovefunds error: %s"
				, e.what() );
		});
	}

	/* xrebalance2: the same cycle, executed by the external
	 * xrebalance plugin (layer-splitting on stock askrene).  The
	 * plugin owns constraint knowledge and failure feedback;
	 * earnings attribution arrives separately via its
	 * xrebalance_part notifications (XRebalancePartMonitor), never
	 * from this response.  */
	Ev::Io<void>
	execute_cycle_plugin( std::vector<ScidCap> source_caps
			    , std::vector<ScidCap> dest_caps
			    , std::int64_t requested_sat
			    , std::uint32_t maxfee_ppm
			    ) {
		/* Object-form entries carry the per-scid caps; the plugin
		 * bounds each channel at min(cap, live liquidity) inside
		 * one solve, so no peer overshoots its band edge however
		 * the MCF concentrates the flow.  */
		auto parms = Json::Out();
		auto obj = parms.start_object();
		auto sa = obj.start_array("sources");
		for (auto const& s : source_caps) {
			auto so = sa.start_object();
			so.field("scid", s.scid);
			so.field("max_msat",
				 std::uint64_t(s.max_sat) * 1000);
			so.end_object();
		}
		sa.end_array();
		auto da = obj.start_array("destinations");
		for (auto const& s : dest_caps) {
			auto d = da.start_object();
			d.field("scid", s.scid);
			d.field("max_msat",
				std::uint64_t(s.max_sat) * 1000);
			d.end_object();
		}
		da.end_array();
		obj.field("amount_msat",
			  std::uint64_t(requested_sat) * 1000);
		obj.field("maxfee_ppm", std::uint64_t(maxfee_ppm));
		obj.field("maxparts", maxparts);
		obj.end_object();
		return rpc.command("xrebalance", std::move(parms))
		.then([this](Jsmn::Object res) {
			return log_plugin_result(std::move(res));
		}).catching<RpcError>([this](RpcError const& e) {
			/* Also the plugin-not-loaded case ("Unknown
			 * command"): one clean line per cycle, retried
			 * next cycle.  */
			return Boss::log( bus, Info
				, "XRebalancer: xrebalance plugin did not "
				  "execute: %s"
				, rpc_error_summary(e).c_str() );
		}).catching<std::exception>([this](std::exception const& e) {
			return Boss::log( bus, Warn
				, "XRebalancer: xrebalance plugin error: %s"
				, e.what() );
		});
	}

	/* One-line summary of an RpcError's JSON-RPC message, with the
	 * embedded multi-line error JSON collapsed to a single line.  */
	static std::string rpc_error_summary(RpcError const& e) {
		auto msg = std::string("unknown error");
		if (e.error.is_object() && e.error.has("message")
		 && e.error["message"].is_string())
			msg = std::string(e.error["message"]);
		for (auto& ch : msg)
			if (ch == '\n' || ch == '\t')
				ch = ' ';
		return msg;
	}

	/* Log one summary line for the transfer plus one line per part,
	 * all under the "XRebalancer:" prefix so a single grep tells the
	 * whole story.  The per-part / chokepoint detail comes from the
	 * xmovefunds response (results[]/errors[]); nothing is re-derived. */
	Ev::Io<void> log_result(Jsmn::Object res) {
		/* The xmovefunds reply wraps the per-payment summary
		 * (parts/delivered/fee/results/errors) under "execution";
		 * the top level carries status/source_scids/amount/askrene. */
		auto exec = (res.is_object() && res.has("execution"))
			  ? res["execution"] : res;
		auto num = [&exec](char const* k) -> double {
			if (exec.is_object() && exec.has(k)) {
				auto v = exec[k];
				if (v.is_number())
					return double(v);
			}
			return -1.0;
		};
		auto delivered = num("delivered_msat");
		auto fee = num("fee_total_msat");
		auto ppm = std::string();
		if (delivered > 0.0) {
			auto os = std::ostringstream();
			os << " (" << std::llround(fee * 1e6 / delivered)
			   << " ppm)";
			ppm = os.str();
		}
		/* Chokepoint: among the failed parts, surface the one that got
		 * CLOSEST to delivery -- the smallest from_target magnitude --
		 * because that frontier (how near the best attempt came, and
		 * the node that walled it) is the informative number, not
		 * whichever part happens to carry the lowest partid.  Parts
		 * with no parseable from_target (non-204 fallbacks) sort last;
		 * if none parse we keep the first.  The failcode rides along in
		 * the error string, so a 0x100c fee-wall vs 0x1007 liquidity-
		 * wall frontier stays distinguishable.  */
		auto reason = std::string();
		if (exec.is_object() && exec.has("errors")
		 && exec["errors"].is_array() && exec["errors"].size() > 0) {
			auto errs = exec["errors"];
			/* "from_target=N" -> N; sentinel max if absent.  */
			auto from_target_mag = [](std::string const& s) -> long {
				auto key = std::string("from_target=");
				auto pos = s.find(key);
				if (pos == std::string::npos)
					return std::numeric_limits<long>::max();
				pos += key.size();
				auto n = 0L;
				auto any = false;
				while (pos < s.size()
				    && s[pos] >= '0' && s[pos] <= '9') {
					n = n * 10 + (s[pos] - '0');
					++pos;
					any = true;
				}
				return any ? n : std::numeric_limits<long>::max();
			};
			auto best_i = std::size_t(0);
			auto best = std::numeric_limits<long>::max();
			for (auto i = std::size_t(0); i < errs.size(); ++i) {
				auto m = from_target_mag(
					std::string(errs[i]));
				if (m < best) {
					best = m;
					best_i = i;
				}
			}
			auto e = std::string(errs[best_i]);
			for (auto& ch : e)
				if (ch == '\n' || ch == '\t')
					ch = ' ';
			reason = "; reason: " + e;
			if (errs.size() > 1)
				reason += " [closest of "
					+ std::to_string(errs.size()) + "]";
		}
		if (delivered > 0.0)
			/* Full or partial delivery: settled/total parts and the
			 * economics; reason is present only on a partial.  */
			return Boss::log( bus, Info
				, "XRebalancer: transfer done: %.0f/%.0f parts, "
				  "delivered %s msat, fee %s msat%s%s."
				, num("parts_complete"), num("parts")
				, Util::Str::group_digits(
					std::int64_t(std::llround(
						delivered))).c_str()
				, Util::Str::group_digits(
					std::int64_t(std::llround(
						fee))).c_str()
				, ppm.c_str(), reason.c_str() );
		/* Nothing delivered: a clean failure -- show the part count
		 * attempted and the chokepoint, not three zeros.  */
		return Boss::log( bus, Info
			, "XRebalancer: transfer failed: %.0f part(s)%s."
			, num("parts"), reason.c_str() );
	}

	/* Plugin-response counterpart of log_result.  The xrebalance
	 * reply is flat (no "execution" wrapper); parts[] carry status
	 * strings plus failure geometry (hops_short / erring_scidd /
	 * failcode), and stragglers show as "pending" (their results
	 * stream via xrebalance_part notifications).  The label is the
	 * plugin's request id; logging it links this line to the
	 * plugin's own "req <id>" lines.  */
	Ev::Io<void> log_plugin_result(Jsmn::Object res) {
		auto num = [&res](char const* k) -> double {
			if (res.is_object() && res.has(k)) {
				auto v = res[k];
				if (v.is_number())
					return double(v);
			}
			return -1.0;
		};
		auto str = [&res](char const* k) -> std::string {
			if (res.is_object() && res.has(k)) {
				auto v = res[k];
				if (v.is_string())
					return std::string(v);
			}
			return "";
		};
		auto delivered = num("delivered_msat");
		auto fee = num("fee_msat");
		auto label = str("label");
		auto req = label.empty() ? std::string()
			 : " [req " + label + "]";
		/* The plugin clamps the ask to what the channels can
		 * carry under their caps (less fee headroom) and reports
		 * the clamp; surface it so a capped cycle is legible.  */
		auto amount = num("amount_msat");
		auto effective = num("effective_amount_msat");
		auto capped = std::string();
		if (effective >= 0.0 && amount > 0.0 && effective < amount) {
			auto os = std::ostringstream();
			os << " (ask "
			   << Util::Str::group_digits(std::int64_t(
				std::llround(amount)))
			   << " capped to "
			   << Util::Str::group_digits(std::int64_t(
				std::llround(effective)))
			   << " msat)";
			capped = os.str();
		}
		auto ppm = std::string();
		if (delivered > 0.0) {
			auto os = std::ostringstream();
			os << " (" << std::llround(fee * 1e6 / delivered)
			   << " ppm)";
			ppm = os.str();
		}
		/* Part census, plus the chokepoint: among failed parts,
		 * the one that got closest to delivery (smallest
		 * hops_short) is the informative frontier.  */
		auto parts_total = std::size_t(0);
		auto parts_complete = std::size_t(0);
		auto parts_pending = std::size_t(0);
		auto parts_failed = std::size_t(0);
		auto reason = std::string();
		auto best_short = std::numeric_limits<double>::max();
		if (res.is_object() && res.has("parts")
		 && res["parts"].is_array()) {
			auto parts = res["parts"];
			parts_total = parts.size();
			for (auto i = std::size_t(0); i < parts.size(); ++i) {
				auto p = parts[i];
				if (!p.is_object() || !p.has("status")
				 || !p["status"].is_string())
					continue;
				auto st = std::string(p["status"]);
				if (st == "complete") {
					++parts_complete;
					continue;
				}
				if (st == "pending") {
					++parts_pending;
					continue;
				}
				++parts_failed;
				auto has_short = p.has("hops_short")
					      && p["hops_short"].is_number();
				auto hs = has_short
					? double(p["hops_short"])
					: std::numeric_limits<double>::max();
				if (!reason.empty() && hs >= best_short)
					continue;
				best_short = hs;
				auto os = std::ostringstream();
				os << "; closest failure:";
				if (has_short)
					os << " " << std::llround(hs)
					   << " hops short";
				if (p.has("erring_scidd")
				 && p["erring_scidd"].is_string())
					os << " at "
					   << std::string(p["erring_scidd"]);
				if (p.has("failcode")
				 && p["failcode"].is_number())
					os << " (failcode 0x" << std::hex
					   << std::llround(
						double(p["failcode"]))
					   << std::dec << ")";
				reason = os.str();
			}
		}
		if (parts_failed > 1)
			reason += " [closest of "
				+ std::to_string(parts_failed) + "]";
		auto pending_note = std::string();
		if (parts_pending > 0) {
			auto os = std::ostringstream();
			os << " (" << parts_pending
			   << " pending, settling in background)";
			pending_note = os.str();
		}
		auto detail = str("detail");
		auto detail_note = detail.empty() ? std::string()
				 : "; detail: " + detail;
		if (delivered > 0.0)
			return Boss::log( bus, Info
				, "XRebalancer: transfer done%s: %zu/%zu "
				  "parts, delivered %s msat, fee %s "
				  "msat%s%s%s%s."
				, req.c_str()
				, parts_complete, parts_total
				, Util::Str::group_digits(std::int64_t(
					std::llround(delivered))).c_str()
				, Util::Str::group_digits(std::int64_t(
					std::llround(fee))).c_str()
				, ppm.c_str(), capped.c_str()
				, reason.c_str()
				, pending_note.c_str() );
		return Boss::log( bus, Info
			, "XRebalancer: transfer failed%s: %zu part(s)%s%s%s%s."
			, req.c_str(), parts_total, capped.c_str()
			, reason.c_str()
			, detail_note.c_str(), pending_note.c_str() );
	}

	/* "scid:cap_sat" per entry, so the debug line shows where this
	 * cycle is allowed to move how much.  */
	static std::string join_caps(std::vector<ScidCap> const& v) {
		auto os = std::ostringstream();
		auto first = true;
		for (auto const& s : v) {
			if (!first) os << ",";
			first = false;
			os << s.scid << ":" << s.max_sat;
		}
		return os.str();
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl(S::Bus& bus_, Waiter& waiter_)
		: bus(bus_), waiter(waiter_), mode_proxy(bus_), rpc(bus_) {
		start();
	}
};

XRebalancer::XRebalancer(XRebalancer&&) =default;
XRebalancer::~XRebalancer() =default;

XRebalancer::XRebalancer(S::Bus& bus, Boss::Mod::Waiter& waiter)
	: pimpl(Util::make_unique<Impl>(bus, waiter)) { }

}}
