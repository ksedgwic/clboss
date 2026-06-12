#include"Boss/Mod/XRebalancer.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/RebalanceModeProxy.hpp"
#include"Boss/ModG/RpcProxy.hpp"
#include"Boss/Msg/DbResource.hpp"
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
auto const opt_size_factor = std::string("clboss-xrebalance-size-factor");
auto const opt_window_days = std::string("clboss-xrebalance-earnings-window-days");
auto const opt_fill_loc = std::string("clboss-xrebalance-fill-loc");
auto const opt_drain_loc = std::string("clboss-xrebalance-drain-loc");
auto const opt_maxparts = std::string("clboss-xrebalance-maxparts");
auto const opt_focused_frac = std::string("clboss-xrebalance-focused-frac");

auto constexpr default_per_hour = double(12.0);
auto constexpr default_floor = double(50.0);
auto constexpr default_size_factor = double(0.1);
auto constexpr default_window_days = double(90.0);
/* Tier bands (Loc%); match clboss-xrebalance-view defaults.  */
auto constexpr default_fill_band = double(10.0);
auto constexpr default_drain_band = double(90.0);
/* MCF split cap passed to clboss-xmovefunds (askrene getroutes maxparts).  */
auto constexpr default_maxparts = double(10.0);
/* Fraction of cycles run focused (single random target, whole opposite
 * pool) instead of matched-pool; 50/50 so both styles accrue comparable
 * sample counts side by side.  */
auto constexpr default_focused_frac = double(0.5);
/* Within focused cycles, probability the target is a fill channel
 * (deliveries are the priority); the rest target a drain channel.  */
auto constexpr focused_fill_prob = double(0.9);

/* Fill/drain Loc% targets the deficits aim toward (25% / 75%).  */
auto constexpr fill_target_pct = double(25.0);
auto constexpr drain_target_pct = double(75.0);

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
	double size_factor;       /* lo bound, or the fixed value */
	double size_factor_hi;    /* hi bound when size_factor is a lo:hi range */
	double window_days;
	double fill_band;
	double drain_band;
	std::uint32_t maxparts;   /* MCF split cap (integer count) */
	double focused_frac;      /* fraction of cycles run focused */
	bool floor_auto;   /* floor option set to "auto" (sweep) */
	bool size_factor_range;   /* size_factor set as lo:hi (per-cycle random) */
	bool started;

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
		double pct_local;
		std::int64_t tgt_fill_sat;
		std::int64_t tgt_drain_sat;
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
		size_factor = default_size_factor;
		size_factor_hi = default_size_factor;
		window_days = default_window_days;
		fill_band = default_fill_band;
		drain_band = default_drain_band;
		maxparts = std::uint32_t(default_maxparts);
		focused_frac = default_focused_frac;
		floor_auto = false;
		size_factor_range = false;
		started = false;

		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& m) {
			db = m.db;
			return Ev::lift();
		});

		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return manifest_option(opt_per_hour, default_per_hour,
				"Average number of flow-rebalance (xrebalance) "
				"cycles per hour (0 = paused).  Poisson-paced; "
				"only active when the rebalancer mode is "
				"\"flow\".")
			     + manifest_option(opt_floor, default_floor,
				"Route-cost floor (ppm): stop growing the "
				"matched-pool cycle once the marginal joint "
				"NetPpm drops below this.  Sets the derived "
				"amount and the maxfee budget.  Or \"auto\": "
				"each cycle picks a random rung of the derived "
				"floor ladder (sweep).")
			     + manifest_option(opt_size_factor, default_size_factor,
				"Multiplier (> 0) on the derived matched-pool "
				"amount requested per cycle.  <1 requests a "
				"fraction (incremental fills); >1 over-fills "
				"past the band targets -- recoverable, the "
				"channel drifts back -- to make each of the "
				"maxparts parts larger so it amortizes the base "
				"fee and clears the per-part budget.  Or a range "
				"\"lo:hi\" (e.g. 0.5:3.0): each cycle draws a fresh "
				"uniform-random multiplier in [lo,hi], sweeping the "
				"request size so askrene's accumulated route state "
				"does not stale into repeated 205/206 refusals the "
				"way a single fixed value eventually does.")
			     + manifest_option(opt_window_days, default_window_days,
				"Trailing window (days) over which per-channel "
				"NetPpm is measured for cycle selection.")
			     + manifest_option(opt_fill_loc, default_fill_band,
				"Fill-tier band: channels with Loc% <= this are "
				"fill candidates (funds pushed toward them).")
			     + manifest_option(opt_drain_loc, default_drain_band,
				"Drain-tier band: channels with Loc% >= this are "
				"drain candidates (funds pulled from them).")
			     + manifest_option(opt_maxparts, default_maxparts,
				"Max parts (paths) MCF may split a cycle into "
				"(askrene getroutes maxparts).  Lower = fewer, "
				"fatter parts that amortize the base fee and pass "
				"the per-part gate but need more liquidity; "
				"higher = finer splitting, more learning, more "
				"refusals.")
			     + manifest_option(opt_focused_frac, default_focused_frac,
				"Fraction (0..1) of cycles run as focused-"
				"target cycles: pick one channel at random "
				"(90% from the fill pool, 10% from the drain "
				"pool), offer the entire opposite pool, size "
				"the transfer to the target's own deficit, "
				"and price maxfee at the target's NetPpm plus "
				"the minimum NetPpm of the offered pool.  The "
				"remaining cycles run the matched-pool style "
				"(floor ladder / size-factor).");
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			return handle_option(o);
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
		/* size-factor also accepts a "lo:hi" range (e.g. 0.5:3.0):
		 * instead of one fixed multiplier, each cycle draws a fresh
		 * uniform-random value in [lo,hi] (see plan_and_log).  Sweeping
		 * the size keeps the request varying so askrene's accumulated
		 * route state does not stale into repeated 205/206 refusals the
		 * way a fixed value eventually does.  A plain number clears
		 * range mode and falls through to the numeric path below. */
		if (o.name == opt_size_factor) {
			auto s = std::string(o.value);
			auto colon = s.find(':');
			if (colon != std::string::npos) {
				auto lo = double(0.0);
				auto hi = double(0.0);
				try {
					lo = std::stod(s.substr(0, colon));
					hi = std::stod(s.substr(colon + 1));
				} catch (std::exception const&) {
					return Boss::log( bus, Error
							, "XRebalancer: ignoring invalid "
							  "%s range \"%s\"."
							, o.name.c_str(), s.c_str() );
				}
				if (!(lo > 0.0) || !(hi > 0.0))
					return Boss::log( bus, Error
							, "XRebalancer: %s range bounds "
							  "must be > 0; ignoring \"%s\"."
							, o.name.c_str(), s.c_str() );
				if (hi < lo) { auto t = lo; lo = hi; hi = t; }
				size_factor = lo;
				size_factor_hi = hi;
				size_factor_range = true;
				return Boss::log( bus, Info
						, "XRebalancer: %s set to range "
						  "%.3g:%.3g (per-cycle random)."
						, o.name.c_str(), lo, hi );
			}
			size_factor_range = false;
		}
		/* maxparts is an integer count, not a continuous knob, so it
		 * gets dedicated handling: parse, round, floor at 1 (askrene
		 * requires >= 1), and store as an integer.  */
		if (o.name == opt_maxparts) {
			auto s = std::string(o.value);
			auto v = double(0.0);
			try {
				v = std::stod(s);
			} catch (std::exception const&) {
				return Boss::log( bus, Error
						, "XRebalancer: ignoring invalid "
						  "%s value \"%s\"."
						, o.name.c_str(), s.c_str() );
			}
			v = std::round(v);
			if (v < 1.0) v = 1.0;
			maxparts = std::uint32_t(v);
			return Boss::log( bus, Info
					, "XRebalancer: %s set to %u."
					, o.name.c_str(), maxparts );
		}
		double* target = nullptr;
		if (o.name == opt_per_hour)        target = &per_hour;
		else if (o.name == opt_floor)      target = &floor_ppm;
		else if (o.name == opt_size_factor) target = &size_factor;
		else if (o.name == opt_window_days)target = &window_days;
		else if (o.name == opt_fill_loc)   target = &fill_band;
		else if (o.name == opt_drain_loc)  target = &drain_band;
		else if (o.name == opt_focused_frac) target = &focused_frac;
		else return Ev::lift();

		auto s = std::string(o.value);
		auto v = double(0.0);
		try {
			v = std::stod(s);
		} catch (std::exception const&) {
			return Boss::log( bus, Error
					, "XRebalancer: ignoring invalid %s "
					  "value \"%s\"."
					, o.name.c_str(), s.c_str()
					);
		}
		if (o.name == opt_size_factor) {
			if (!(v > 0.0))
				return Boss::log( bus, Error
						, "XRebalancer: %s must be > 0; "
						  "ignoring \"%s\"."
						, o.name.c_str(), s.c_str()
						);
		} else if (o.name == opt_fill_loc || o.name == opt_drain_loc) {
			if (v < 0.0)        v = 0.0;
			else if (v > 100.0) v = 100.0;
		} else if (o.name == opt_focused_frac) {
			if (v < 0.0)      v = 0.0;
			else if (v > 1.0) v = 1.0;
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
				auto pct = double(loc) / double(cap) * 100.0;
				auto tf = cap * std::int64_t(fill_target_pct)
					/ 100 - loc;
				auto td = loc - cap
					* std::int64_t(drain_target_pct) / 100;
				out.push_back(Chan{
					std::string(c["short_channel_id"]),
					Ln::NodeId(std::string(c["peer_id"])),
					online, cap, loc, pct,
					tf > 0 ? tf : 0,
					td > 0 ? td : 0
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
		return mode_proxy.get_mode().then([this](RebalanceMode m) {
			if (m != RebalanceMode::xrebalance)
				return Boss::log( bus, Debug
						, "XRebalancer: idle (mode is "
						  "\"%s\", not \"xrebalance\")."
						, rebalance_mode_to_string(m)
						);
			return run_cycle();
		});
	}

	/* Fetch live balances/online (listpeerchannels), query the windowed
	 * per-node NetPpm, join, derive the matched-pool cycle, execute.  */
	Ev::Io<void> run_cycle() {
		return rpc.command( "listpeerchannels"
				  , Json::Out::empty_object()
				  ).then([this](Jsmn::Object res) {
			return run_cycle_with(std::make_shared<std::vector<Chan>>(
				build_chans(res)));
		});
	}

	Ev::Io<void>
	run_cycle_with(std::shared_ptr<std::vector<Chan>> chans) {
		if (chans->empty())
			return Boss::log( bus, Info
					, "XRebalancer: no channel data, "
					  "skipping cycle." );
		auto cutoff = double(std::time(nullptr))
			    - window_days * 24.0 * 60.0 * 60.0;
		return db.transact().then([this, cutoff, chans](Sqlite3::Tx tx) {
			auto net = std::make_shared<std::map<Ln::NodeId, NetPpm>>();
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
				auto p = NetPpm();
				if (in_f > 0.0) {
					p.has_in = true;
					p.in_net = (in_e - in_x) * 1e6 / in_f;
				}
				if (out_f > 0.0) {
					p.has_out = true;
					p.out_net = (out_e - out_x) * 1e6 / out_f;
				}
				(*net)[node] = p;
			}
			tx.commit();
			return plan_and_log(chans, net);
		});
	}

	/* A pool member: cached channel plus its joined NetPpm on the
	 * relevant side.  */
	struct PoolItem {
		Chan const* ch;
		double ppm;       /* out_net for fill, in_net for drain */
		std::int64_t deficit; /* tgt_fill for fill, tgt_drain for drain */
	};

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
		    ) {
		auto fill = std::vector<PoolItem>();
		auto drain = std::vector<PoolItem>();
		for (auto const& c : *chans) {
			if (!c.online)
				continue;
			auto it = net->find(c.node);
			if (it == net->end())
				continue;
			auto const& np = it->second;
			if (c.pct_local <= fill_band
			 && np.has_out && np.out_net > 0.0
			 && c.tgt_fill_sat > 0)
				fill.push_back(PoolItem{
					&c, np.out_net, c.tgt_fill_sat });
			if (c.pct_local >= drain_band
			 && np.has_in && np.in_net > 0.0
			 && c.tgt_drain_sat > 0)
				drain.push_back(PoolItem{
					&c, np.in_net, c.tgt_drain_sat });
		}
		std::sort(fill.begin(), fill.end(),
			[](PoolItem const& a, PoolItem const& b){
				return a.ppm > b.ppm; });
		std::sort(drain.begin(), drain.end(),
			[](PoolItem const& a, PoolItem const& b){
				return a.ppm > b.ppm; });

		if (fill.empty() || drain.empty())
			return Boss::log( bus, Info
				, "XRebalancer: no cycle -- NO_CANDIDATES "
				  "(fill=%zu drain=%zu; bands fill<=%.1f "
				  "drain>=%.1f, window=%.0fd)."
				, fill.size(), drain.size()
				, fill_band, drain_band, window_days );

		/* Cycle style: focused (single random target, whole opposite
		 * pool) vs matched (joint(N) curve below).  Both styles need
		 * both pools, so the draw comes after the empty check.  */
		if (focused_frac > 0.0) {
			auto dist = std::uniform_real_distribution<double>(
				0.0, 1.0);
			if (dist(Boss::random_engine) < focused_frac)
				return plan_focused(fill, drain);
		}

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

		/* Bold set: channels accumulated to reach best_n on each side.  */
		auto pick = [best_n](std::vector<PoolItem> const& pool){
			auto picks = std::vector<std::string>();
			std::int64_t acc = 0;
			for (auto const& it : pool) {
				if (acc >= best_n)
					break;
				picks.push_back(it.ch->scid);
				acc += it.deficit;
			}
			return picks;
		};
		auto dest_scids = pick(fill);    /* fill = where funds land */
		auto source_scids = pick(drain); /* drain = where funds leave */

		/* size_factor may be a "lo:hi" range; draw a fresh multiplier
		 * each cycle so the requested size sweeps (see handle_option).  */
		auto effective_size_factor = size_factor;
		auto sf_note = std::string();
		if (size_factor_range) {
			auto dist = std::uniform_real_distribution<double>(
				size_factor, size_factor_hi);
			effective_size_factor = dist(Boss::random_engine);
			auto os = std::ostringstream();
			os << " (rand " << size_factor << ":" << size_factor_hi << ")";
			sf_note = os.str();
		}

		auto requested = std::int64_t(
			std::max<double>(1.0, std::llround(double(best_n)
							   * effective_size_factor)));
		auto maxfee = std::uint32_t(std::llround(best_joint));

		return Boss::log( bus, Info, "%s", levels_str.c_str() )
		     + Boss::log( bus, Info
			, "XRebalancer: cycle [matched] floor=%.1f%s window=%.0fd "
			  "-> derived N=%s sat, joint=%.1f ppm "
			  "(fill>=%.1f + drain>=%.1f); size_factor=%.3g%s "
			  "-> request=%s sat (maxfee %u ppm); "
			  "sources=%zu dests=%zu; executing."
			, effective_floor, picked_note.c_str(), window_days
			, Util::Str::group_digits(
				std::int64_t(best_n)).c_str(), best_joint
			, best_fill_ppm, best_drain_ppm
			, effective_size_factor, sf_note.c_str()
			, Util::Str::group_digits(
				std::int64_t(requested)).c_str()
			, (unsigned)maxfee
			, source_scids.size(), dest_scids.size()
			).then([this, source_scids, dest_scids]() {
			return Boss::log( bus, Debug
				, "XRebalancer:   sources=[%s] dests=[%s]"
				, join_scids(source_scids).c_str()
				, join_scids(dest_scids).c_str()
				);
		}).then([this, source_scids, dest_scids, requested, maxfee]() {
			return execute_cycle(source_scids, dest_scids,
					     requested, maxfee);
		});
	}

	/* Focused cycle: one uniformly random target (90% fill / 10%
	 * drain), the entire opposite pool as counterparty, sized to the
	 * target's own deficit, priced at the target's NetPpm plus the
	 * minimum NetPpm of the offered pool -- the conservative joint:
	 * every sat moved earns at least the target's side plus at least
	 * the cheapest offered channel's side.  No curve/ladder/floor and
	 * no size factor: the budget is the target's own economics, and
	 * the variety the size sweep manufactured comes free from drawing
	 * a different target/amount/pool every cycle.  Uniform pick on
	 * purpose: discovery (no starvation traps) plus an unbiased
	 * per-target census, same methodology as the floor-ladder sweep.  */
	Ev::Io<void>
	plan_focused( std::vector<PoolItem> const& fill
		    , std::vector<PoolItem> const& drain
		    ) {
		auto side = std::uniform_real_distribution<double>(
			0.0, 1.0)(Boss::random_engine);
		auto fill_target = side < focused_fill_prob;
		auto const& tpool = fill_target ? fill : drain;
		auto const& opool = fill_target ? drain : fill;
		auto pick = std::uniform_int_distribution<std::size_t>(
			0, tpool.size() - 1)(Boss::random_engine);
		auto const& target = tpool[pick];
		/* Pools are sorted NetPpm-descending, so the minimum
		 * offered NetPpm is the last element.  */
		auto min_offered = opool.back().ppm;
		auto maxfee = std::uint32_t(std::llround(
			target.ppm + min_offered));
		auto requested = target.deficit;
		auto target_scids = std::vector<std::string>{
			target.ch->scid };
		auto other_scids = std::vector<std::string>();
		for (auto const& it : opool)
			other_scids.push_back(it.ch->scid);
		auto source_scids = fill_target ? other_scids : target_scids;
		auto dest_scids = fill_target ? target_scids : other_scids;
		return Boss::log( bus, Info
			, "XRebalancer: cycle [focused %s] target=%s "
			  "window=%.0fd -> request=%s sat (target deficit), "
			  "maxfee=%u ppm (target %.1f + min offered %.1f); "
			  "sources=%zu dests=%zu; executing."
			, fill_target ? "fill" : "drain"
			, target.ch->scid.c_str()
			, window_days
			, Util::Str::group_digits(requested).c_str()
			, (unsigned)maxfee
			, target.ppm, min_offered
			, source_scids.size(), dest_scids.size()
			).then([this, source_scids, dest_scids]() {
			return Boss::log( bus, Debug
				, "XRebalancer:   sources=[%s] dests=[%s]"
				, join_scids(source_scids).c_str()
				, join_scids(dest_scids).c_str()
				);
		}).then([this, source_scids, dest_scids, requested, maxfee]() {
			return execute_cycle(source_scids, dest_scids,
					     requested, maxfee);
		});
	}

	/* Drive the chosen cycle through the existing clboss-xmovefunds
	 * command (reusing its sendpay/waitsendpay/harvest/attribution).
	 * The loop awaits this, so no new cycle starts while one is in
	 * flight (the natural in-flight guard until the abandon/timeout
	 * increment lands).  */
	Ev::Io<void>
	execute_cycle( std::vector<std::string> source_scids
		     , std::vector<std::string> dest_scids
		     , std::int64_t requested_sat
		     , std::uint32_t maxfee_ppm
		     ) {
		auto parms = Json::Out();
		auto obj = parms.start_object();
		auto sa = obj.start_array("source_scid");
		for (auto const& s : source_scids)
			sa.entry(s);
		sa.end_array();
		auto da = obj.start_array("dest_scid");
		for (auto const& s : dest_scids)
			da.entry(s);
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

	static std::string join_scids(std::vector<std::string> const& v) {
		auto os = std::ostringstream();
		auto first = true;
		for (auto const& s : v) {
			if (!first) os << ",";
			first = false;
			os << s;
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
