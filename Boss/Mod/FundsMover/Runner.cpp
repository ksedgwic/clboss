#include"Boss/Mod/AskreneLayer.hpp"
#include"Boss/Mod/AskreneUpdates.hpp"
#include"Boss/Mod/FundsMover/Attempter.hpp"
#include"Boss/Mod/FundsMover/Claimer.hpp"
#include"Boss/Mod/FundsMover/Runner.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/RequestAskreneUpdates.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseAskreneUpdates.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/foreach.hpp"
#include"Ev/now.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Util/stringify.hpp"
#include<math.h>

namespace {

/* Minimum size below which we will no longer split.  */
auto const minimum_split_size = Ln::Amount::msat(100000);

/* Maximum number of attempts in parallel to invoke.  */
auto constexpr maximum_attempts = std::size_t(30);
/* Maximum amount of time we run, in seconds.  */
auto constexpr maximum_time = double(120.0);

/* Golden ratio.  */
auto const gold = (1.0 + sqrt(5.0)) / 2.0;

}

namespace Boss { namespace Mod { namespace FundsMover {

Runner::Runner( S::Bus& bus_
	      , Boss::Mod::Rpc& rpc_
	      , Ln::NodeId self_
	      , Boss::Mod::FundsMover::Claimer& claimer_
	      , Boss::Msg::RequestMoveFunds const& req
	      , std::uint64_t min_prob_ppm_
	      , Boss::ModG::ReqResp< Boss::Msg::RequestAskreneUpdates
				   , Boss::Msg::ResponseAskreneUpdates
				   >& updates_rr_
	      ) : bus(bus_)
		, rpc(rpc_)
		, self(std::move(self_))
		, claimer(claimer_)
		, requester(req.requester)
		, source(req.source)
		, destination(req.destination)
		, amount(req.amount)
		, fee_budget(std::make_shared<Ln::Amount>(req.fee_budget))
		, remaining_amount(std::make_shared<Ln::Amount>(req.amount))
		, orig_budget(req.fee_budget)
		, min_prob_ppm(min_prob_ppm_)
		, start_time(Ev::now())
		, updates_rr(updates_rr_)
		, attempts(0)
		{ }

Ev::Io<void> Runner::start(std::shared_ptr<Runner> const& self) {
	auto act = self->core_run().then([self]() {
		return Ev::lift();
	});
	return Boss::concurrent(act);
}

Ev::Io<void> Runner::core_run() {
	return Ev::lift().then([this]() {
		/* Initialize.  */
		transferred = Ln::Amount::sat(0);
		auto src_pfx = std::string(source).substr(0, 8);
		auto dst_pfx = std::string(destination).substr(0, 8);
		return Boss::log( bus, Debug
				, "FundsMover/Runner: REQ "
				  "src=%s... dst=%s... "
				  "amount=%s fee_budget=%s"
				, src_pfx.c_str()
				, dst_pfx.c_str()
				, std::string(amount).c_str()
				, std::string(orig_budget).c_str()
				);
	}).then([this]() {
		return gather_info();
	});
}
/* Gets information about us and the peers to transfer to/from.  */
Ev::Io<void> Runner::gather_info() {
	return Ev::lift().then([this]() {
		first_scid = nullptr;
		last_scid = nullptr;

		/* Get first_scid.  */
		auto parms = Json::Out()
			.start_object()
				.field("source", std::string(self))
			.end_object()
			;
		return rpc.command("listchannels", std::move(parms));
	}).then([this](Jsmn::Object res) {
		try {
			auto cs = res["channels"];
			for (auto c : cs) {
				auto d = Ln::NodeId(std::string(
					c["destination"]
				));
				if (d != source)
					continue;
				first_scid = Ln::Scid(std::string(
					c["short_channel_id"]
				));
				break;
			}
		} catch (std::exception const& ex) {
			return Boss::log( bus, Error
					, "FundsMover: Unexpected result "
					  "from listchannels: %s: %s"
					, Util::stringify(res).c_str()
					, ex.what()
					);
		}
		return Ev::lift();
	}).then([this]() {

		/* Get last_scid.  */
		auto parms = Json::Out()
			.start_object()
				.field("source", std::string(destination))
			.end_object()
			;
		return rpc.command("listchannels", std::move(parms));
	}).then([this](Jsmn::Object res) {
		try {
			auto cs = res["channels"];
			for (auto c : cs) {
				auto d = Ln::NodeId(std::string(
					c["destination"]
				));
				if (d != self)
					continue;
				last_scid = Ln::Scid(std::string(
					c["short_channel_id"]
				));
				base_fee = Ln::Amount::msat(double(
					c["base_fee_millisatoshi"]
				));
				proportional_fee = std::uint32_t(double(
					c["fee_per_millionth"]
				));
				cltv_delta = std::uint32_t(double(
					c["delay"]
				));
				break;
			}
		} catch (std::exception const& ex) {
			return Boss::log( bus, Error
					, "FundsMover: Unexpected result "
					  "from listchannels: %s: %s"
					, Util::stringify(res).c_str()
					, ex.what()
					);
		}
		return Ev::lift();
	}).then([this]() {

		/* Now check if we could complete all the information.  */
		if (!last_scid && !first_scid)
			return Boss::log( bus, Info
					, "FundsMover: Cannot move %s from "
					  "%s to %s, no channels with them."
					, std::string(amount).c_str()
					, std::string(source).c_str()
					, std::string(destination).c_str()
					);
		if (!last_scid)
			return Boss::log( bus, Info
					, "FundsMover: Cannot move %s from "
					  "%s to %s, no channel with "
					  "destination."
					, std::string(amount).c_str()
					, std::string(source).c_str()
					, std::string(destination).c_str()
					);
		if (!first_scid)
			return Boss::log( bus, Info
					, "FundsMover: Cannot move %s from "
					  "%s to %s, no channel with "
					  "source."
					, std::string(amount).c_str()
					, std::string(source).c_str()
					, std::string(destination).c_str()
					);
		return attempt(amount);
	}).then([this]() {
		return finish();
	});
}

Ev::Io<void> Runner::attempt(Ln::Amount amount) {
	++attempts;
	/* Each attempt projects the still-fresh learned updates into its
	 * OWN private, uuid-named askrene layer.  It is named by only this
	 * attempt's getroutes and removed when the attempt finishes, so --
	 * unlike the old shared wiped layer -- it can never be absent while
	 * another getroutes references it.  The attempt writes updates it
	 * learns mid-run directly into this same layer (so its retries
	 * route around them) and also persists them via AskreneUpdates for
	 * future attempts.  */
	auto updates_layer = std::make_shared<std::string>();
	return updates_rr.execute(Msg::RequestAskreneUpdates{ nullptr }
	).then([this](Msg::ResponseAskreneUpdates data) {
		return AskreneUpdates::open_layer(rpc, data);
	}).then([this, amount, updates_layer](std::string layer) {
		*updates_layer = std::move(layer);
		auto pair = claimer.generate();
		auto preimage = std::move(pair.first);
		auto payment_secret = std::move(pair.second);
		return Attempter::run( bus, rpc, self
				     , std::move(preimage)
				     , std::move(payment_secret)
				     , source
				     , destination
				     , amount
				     , fee_budget
				     , remaining_amount
				     , last_scid
				     , base_fee
				     , proportional_fee
				     , cltv_delta
				     , first_scid
				     /* orig_budget / orig_amount: pass
				      * Runner's per-msat rate snapshot
				      * for the Attempter's absolute
				      * rate cap.  Both are class members
				      * of Runner and don't change after
				      * construction.
				      */
				     , orig_budget
				     , this->amount
				     , min_prob_ppm
				     , *updates_layer
				     );
	}).then([this, updates_layer](bool result) {
		/* Tear the private layer down before post-attempt
		 * bookkeeping (best-effort; it is non-persistent).  */
		return AskreneUpdates::close_layer(rpc, *updates_layer)
		    .then([result]() { return Ev::lift(result); });
	}).then([this, amount](bool result) {
		--attempts;
		if (result) {
			transferred += amount;

			return Boss::log( bus, Debug
					, "FundsMover: successfully moved "
					  "%s from %s to %s."
					, std::string(amount).c_str()
					, std::string(source).c_str()
					, std::string(destination).c_str()
					);
		} else if ( (amount >= minimum_split_size)
			 && (attempts + 2 < maximum_attempts)
			 && ((Ev::now() - start_time) < maximum_time)
			  ) {
			auto a1 = amount / gold;
			auto a2 = amount - a1;
			auto as = std::vector<Ln::Amount>{a1, a2};

			auto act = Ev::lift();
			act += Boss::log( bus, Debug
					, "FundsMover: Splitting %s into "
					  "%s + %s while moving from %s to "
					  "%s."
					, std::string(amount).c_str()
					, std::string(a1).c_str()
					, std::string(a2).c_str()
					, std::string(source).c_str()
					, std::string(destination).c_str()
					);

			auto f = [this](Ln::Amount a) { return attempt(a); };
			return act
			     + Ev::foreach(std::move(f), std::move(as));
			     ;
		}

		/* Give up on this attempt.  */
		return Boss::log( bus, Debug
				, "FundsMover: failed to move %s from "
				  "%s to %s."
				, std::string(amount).c_str()
				, std::string(source).c_str()
				, std::string(destination).c_str()
				);
	});
}

Ev::Io<void> Runner::finish() {
	return Ev::lift().then([this]() {
		auto fee_spent = orig_budget - *fee_budget;
		auto src_pfx = std::string(source).substr(0, 8);
		auto dst_pfx = std::string(destination).substr(0, 8);
		return Boss::log( bus, Debug
				, "FundsMover/Runner: DONE "
				  "src=%s... dst=%s... "
				  "transferred=%s fee_spent=%s "
				  "orig_amount=%s orig_budget=%s"
				, src_pfx.c_str()
				, dst_pfx.c_str()
				, std::string(transferred).c_str()
				, std::string(fee_spent).c_str()
				, std::string(amount).c_str()
				, std::string(orig_budget).c_str()
				);
	}).then([this]() {
		return bus.raise(Msg::ResponseMoveFunds{
			requester, transferred, orig_budget - *fee_budget,
			source, destination
		});
	});
}

}}}
