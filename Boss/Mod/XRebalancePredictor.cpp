#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/Mod/XRebalancePredictor.hpp"
#include"Boss/ModG/RebalanceModeProxy.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/OptionType.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/XRebalanceLayerAged.hpp"
#include"Boss/RebalanceMode.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/Amount.hpp"
#include"Ln/Scid.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include<cinttypes>
#include<cstdint>
#include<map>
#include<string>
#include<utility>
#include<vector>

namespace {

auto const opt_horizon_max =
	std::string("clboss-xrebalance-predict-horizon-max-secs");
auto const opt_horizon_frac =
	std::string("clboss-xrebalance-predict-horizon-frac");
auto const opt_min_samples =
	std::string("clboss-xrebalance-predict-min-samples");
auto const opt_wall_margin =
	std::string("clboss-xrebalance-predict-wall-margin");
auto const opt_floor_factor =
	std::string("clboss-xrebalance-predict-floor-factor");

/* The live defaults deliberately differ from the spot-check
 * defaults (XRebalancePredict::default_params) in two places:
 * horizon-max 0 keeps the predictor OFF until the operator opts in,
 * and floor-factor 0 makes the first enablement walls-only (floors
 * are the riskier half: a too-high floor attracts flow and costs a
 * failed part to self-correct).  */
auto constexpr default_horizon_max = std::uint64_t(0);
auto constexpr default_horizon_frac = double(2.0);
auto constexpr default_min_samples = std::uint64_t(2);
auto constexpr default_wall_margin = double(1.0);
auto constexpr default_floor_factor = double(0.0);

/* Tolerate both a JSON number (startup primitive) and a JSON
 * string (runtime setconfig encoding).  */
bool parse_double(Jsmn::Object const& v, double& out) {
	try {
		if (v.is_number()) {
			out = double(v);
			return true;
		}
		if (v.is_string()) {
			out = std::stod(std::string(v));
			return true;
		}
	} catch (std::exception const&) { }
	return false;
}
bool parse_u64(Jsmn::Object const& v, std::uint64_t& out) {
	try {
		if (v.is_number()) {
			out = std::uint64_t(double(v));
			return true;
		}
		if (v.is_string()) {
			out = std::stoull(std::string(v));
			return true;
		}
	} catch (std::exception const&) { }
	return false;
}

}

namespace Boss { namespace Mod {

XRebalancePredictor::Plan
XRebalancePredictor::plan( std::vector<Row> const& rows
			 , std::uint64_t cutoff
			 , std::uint64_t now
			 , XRebalancePredict::Params const& params
			 ) {
	struct Group {
		std::vector<XRebalancePredict::Bound> bounds;
		/* Newest observation of ANY kind: a node_fail is not
		 * a liquidity bound, but it IS fresh real data, and
		 * candidacy is about whether the routed layer still
		 * carries live evidence for this direction.  */
		std::uint64_t newest = 0;
	};
	auto groups = std::map< std::pair<std::string, std::uint32_t>
			      , Group>();
	for (auto const& row : rows) {
		auto& g = groups[{row.scid, row.dir}];
		if (row.time > g.newest)
			g.newest = row.time;
		auto is_fail = false;
		if (XRebalancePredict::kind_is_bound(row.kind, is_fail))
			g.bounds.push_back(
			    {row.time, is_fail, row.amount_msat});
	}

	auto result = Plan();
	result.directions = groups.size();
	result.candidates = 0;
	for (auto const& e : groups) {
		/* Fresh real data: the routed layer already carries
		 * live evidence; nothing to synthesize.  */
		if (e.second.newest >= cutoff)
			continue;
		++result.candidates;
		auto res = XRebalancePredict::predict(
		    e.second.bounds, now, params);
		/* amount 0 would be a degenerate inform (a wall at 0
		 * is a full exclusion we did not observe; a floor at
		 * 0 is a no-op) -- can arise from margins/factors
		 * scaling a tiny bound down.  Skip.  */
		if (res.wall.would_assert && res.wall.amount_msat > 0)
			result.assertions.push_back(
			    { e.first.first, e.first.second
			    , true, res.wall.amount_msat});
		if (res.floor.would_assert && res.floor.amount_msat > 0)
			result.assertions.push_back(
			    { e.first.first, e.first.second
			    , false, res.floor.amount_msat});
	}
	return result;
}

class XRebalancePredictor::Impl {
private:
	S::Bus& bus;
	std::function<double()> get_now;
	Sqlite3::Db db;
	Boss::Mod::Rpc* rpc;
	ModG::RebalanceModeProxy mode_proxy;

	/* Live (dynamic-option) parameter values.  */
	std::uint64_t horizon_max_secs;
	double horizon_frac;
	std::uint64_t min_samples;
	double wall_margin;
	double floor_factor;

	/* Last-cycle summary, for clboss-status.  */
	std::uint64_t last_run_time;
	std::size_t last_directions;
	std::size_t last_candidates;
	std::size_t last_walls;
	std::size_t last_floors;

	void start() {
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& r) {
			db = r.db;
			return Ev::lift();
		});
		bus.subscribe<Msg::Init
			     >([this](Msg::Init const& init) {
			rpc = &init.rpc;
			return Ev::lift();
		});
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const&) {
			return manifest_int_option( opt_horizon_max
						  , default_horizon_max,
				"Maximum forecast horizon (seconds) of the "
				"xrebalance persistence forecaster, AND its "
				"master switch: 0 (the default) disables "
				"synthetic re-assertion entirely.  When "
				"enabled, after each hourly aging pass the "
				"forecaster re-asserts walls/floors for "
				"channel directions with no live evidence, "
				"for up to min(this, horizon-frac * the "
				"regime's evidence span) past the last "
				"observation.  Since an asserted wall is "
				"never contradicted by routing (the router "
				"will not attempt amounts above it), this "
				"cap IS the wall re-test schedule.  86400 "
				"(24h) is the intended enabled value.")
			     + manifest_double_option( opt_horizon_frac
						     , default_horizon_frac,
				"Forecast horizon as a multiple of the "
				"regime's evidence span (newest - oldest "
				"consistent observation).  2.0: two "
				"observations an hour apart are asserted "
				"for two hours past the newest.")
			     + manifest_int_option( opt_min_samples
						  , default_min_samples,
				"Minimum observations of a side's kind "
				"(failures for walls, successes for "
				"floors) in the current regime before "
				"that side is asserted.")
			     + manifest_double_option( opt_wall_margin
						     , default_wall_margin,
				"Multiplier on asserted wall amounts.  "
				">= 1.0 biases errors high, which "
				"self-corrects (a too-high wall costs a "
				"failed part that writes a fresh real "
				"bound; a too-low wall is sticky until "
				"the horizon).")
			     + manifest_double_option( opt_floor_factor
						     , default_floor_factor,
				"Multiplier on asserted floor amounts; "
				"<= 1.0 is conservative.  0 (the default) "
				"disables floor assertion entirely "
				"(walls-only operation; floors are the "
				"riskier half).");
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			return handle_option(o);
		});
		bus.subscribe<Msg::XRebalanceLayerAged
			     >([this](Msg::XRebalanceLayerAged const& m) {
			if (horizon_max_secs == 0)
				return Ev::lift();
			if (!db || !rpc)
				return Ev::lift();
			auto cutoff = m.cutoff;
			return Boss::concurrent(run(cutoff));
		});
		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const&) {
			return status();
		});
	}

	Ev::Io<void> manifest_int_option( std::string const& name
					, std::uint64_t dflt
					, std::string desc
					) {
		return bus.raise(Msg::ManifestOption{
			name, Msg::OptionType_Int,
			Json::Out::direct(dflt), std::move(desc),
			true /* dynamic */
		});
	}

	Ev::Io<void> manifest_double_option( std::string const& name
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
		if (o.name == opt_horizon_max) {
			auto v = std::uint64_t(0);
			if (!parse_u64(o.value, v))
				return bad_option(o.name);
			horizon_max_secs = v;
			if (horizon_max_secs == 0)
				return Boss::log( bus, Info
						, "XRebalancePredictor: "
						  "disabled (%s = 0)."
						, o.name.c_str());
			return Boss::log( bus, Info
					, "XRebalancePredictor: horizon "
					  "cap = %" PRIu64 " seconds."
					, horizon_max_secs);
		}
		if (o.name == opt_horizon_frac) {
			auto v = double(0.0);
			if (!parse_double(o.value, v) || v <= 0)
				return bad_option(o.name);
			horizon_frac = v;
			return Boss::log( bus, Info
					, "XRebalancePredictor: horizon "
					  "frac = %.3f."
					, horizon_frac);
		}
		if (o.name == opt_min_samples) {
			auto v = std::uint64_t(0);
			if (!parse_u64(o.value, v) || v < 1)
				return bad_option(o.name);
			min_samples = v;
			return Boss::log( bus, Info
					, "XRebalancePredictor: min "
					  "samples = %" PRIu64 "."
					, min_samples);
		}
		if (o.name == opt_wall_margin) {
			auto v = double(0.0);
			if (!parse_double(o.value, v) || v <= 0)
				return bad_option(o.name);
			wall_margin = v;
			return Boss::log( bus, Info
					, "XRebalancePredictor: wall "
					  "margin = %.3f."
					, wall_margin);
		}
		if (o.name == opt_floor_factor) {
			auto v = double(0.0);
			if (!parse_double(o.value, v) || v < 0)
				return bad_option(o.name);
			floor_factor = v;
			if (floor_factor == 0)
				return Boss::log( bus, Info
						, "XRebalancePredictor: "
						  "floors disabled "
						  "(walls-only).");
			return Boss::log( bus, Info
					, "XRebalancePredictor: floor "
					  "factor = %.3f."
					, floor_factor);
		}
		return Ev::lift();
	}

	Ev::Io<void> bad_option(std::string const& name) {
		return Boss::log( bus, Warn
				, "XRebalancePredictor: %s: could not "
				  "parse value; keeping current setting."
				, name.c_str());
	}

	Ev::Io<void> run(std::uint64_t cutoff) {
		return mode_proxy.get_mode().then([this, cutoff
						  ](RebalanceMode m) {
			if (m != RebalanceMode::xrebalance)
				return Boss::log( bus, Debug
						, "XRebalancePredictor: "
						  "mode is not xrebalance; "
						  "skipping cycle.");
			return evaluate(cutoff);
		});
	}

	Ev::Io<void> evaluate(std::uint64_t cutoff) {
		auto rows = std::make_shared<std::vector<Row>>();
		return db.transact().then([rows](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
			SELECT time, scid, dir, kind, amount_msat
			  FROM "XRebalanceHistory"
			 ORDER BY scid, dir, time;
			)QRY").execute();
			for (auto& r : fetch) {
				auto ndx = std::size_t(0);
				auto row = Row();
				row.time = r.get<std::uint64_t>(ndx++);
				row.scid = r.get<std::string>(ndx++);
				row.dir = r.get<std::uint32_t>(ndx++);
				row.kind = r.get<std::string>(ndx++);
				row.amount_msat =
					r.get<std::uint64_t>(ndx++);
				rows->push_back(std::move(row));
			}
			tx.commit();
			return Ev::lift();
		}).then([this, rows, cutoff]() {
			auto params = XRebalancePredict::Params{
				horizon_frac, horizon_max_secs,
				std::size_t(min_samples), wall_margin,
				floor_factor};
			auto now = std::uint64_t(get_now());
			auto result = plan(*rows, cutoff, now, params);

			last_run_time = now;
			last_directions = result.directions;
			last_candidates = result.candidates;
			last_walls = 0;
			last_floors = 0;

			auto act = Ev::lift();
			for (auto const& a : result.assertions) {
				if (a.is_wall) {
					++last_walls;
					act = std::move(act)
					    + Boss::Mod::AskreneLayer::
						inform_channel_constrained(
						*rpc,
						Boss::Mod::AskreneLayer::
						    xrebalance_layer_name,
						Ln::Scid(a.scid), a.dir,
						Ln::Amount::msat(
						    a.amount_msat));
				} else {
					++last_floors;
					act = std::move(act)
					    + Boss::Mod::AskreneLayer::
						inform_channel_unconstrained(
						*rpc,
						Boss::Mod::AskreneLayer::
						    xrebalance_layer_name,
						Ln::Scid(a.scid), a.dir,
						Ln::Amount::msat(
						    a.amount_msat));
				}
			}
			if (result.assertions.empty())
				act = std::move(act)
				    + Boss::log( bus, Debug
					, "XRebalancePredictor: nothing "
					  "to assert (%zu directions, "
					  "%zu candidates)."
					, result.directions
					, result.candidates);
			else
				act = std::move(act)
				    + Boss::log( bus, Info
					, "XRebalancePredictor: asserted "
					  "%zu wall(s), %zu floor(s) "
					  "(%zu directions, %zu "
					  "candidates)."
					, last_walls, last_floors
					, result.directions
					, result.candidates);
			return act;
		});
	}

	Ev::Io<void> status() {
		auto out = Json::Out();
		auto obj = out.start_object();
		obj
			.field("enabled", horizon_max_secs != 0)
			.field("horizon_max_secs", horizon_max_secs)
			.field("horizon_frac", horizon_frac)
			.field("min_samples", min_samples)
			.field("wall_margin", wall_margin)
			.field("floor_factor", floor_factor)
			.field("last_run_time", last_run_time)
			.field( "last_directions"
			      , std::uint64_t(last_directions))
			.field( "last_candidates"
			      , std::uint64_t(last_candidates))
			.field("last_walls", std::uint64_t(last_walls))
			.field("last_floors", std::uint64_t(last_floors))
			;
		obj.end_object();
		return bus.raise(Msg::ProvideStatus{
			"xrebalance_predictor",
			std::move(out)
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
		, rpc(nullptr)
		, mode_proxy(bus_)
		, horizon_max_secs(default_horizon_max)
		, horizon_frac(default_horizon_frac)
		, min_samples(default_min_samples)
		, wall_margin(default_wall_margin)
		, floor_factor(default_floor_factor)
		, last_run_time(0)
		, last_directions(0)
		, last_candidates(0)
		, last_walls(0)
		, last_floors(0) { start(); }
};

XRebalancePredictor::XRebalancePredictor(XRebalancePredictor&&) =default;
XRebalancePredictor::~XRebalancePredictor() =default;

XRebalancePredictor::XRebalancePredictor( S::Bus& bus
					, std::function<double()> get_now_
					)
	: pimpl(Util::make_unique<Impl>(bus, get_now_)) { }

}}
