#include"Boss/Mod/EarningsTracker.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/concurrent.hpp"
#include"Ev/Io.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"

#include<cmath>
#include<map>

namespace Boss { namespace Mod {

class EarningsTracker::Impl {
private:
	S::Bus& bus;
	std::function<double()> get_now;
	Sqlite3::Db db;

	/* Information of a pending MoveFunds.  */
	struct Pending {
		Ln::NodeId source;
		Ln::NodeId destination;
	};
	/* Maps a requester to the source-destination of the movefunds.  */
	std::map<void*, Pending> pendings;

	void start() {
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& r) {
			db = r.db;
			return init();
		});
		bus.subscribe<Msg::ForwardFee
			     >([this](Msg::ForwardFee const& f) {
			return forward_fee(f.in_id, f.out_id, f.fee);
		});
		bus.subscribe<Msg::RequestMoveFunds
			     >([this](Msg::RequestMoveFunds const& req) {
			return request_move_funds(req);
		});
		bus.subscribe<Msg::ResponseMoveFunds
			     >([this](Msg::ResponseMoveFunds const& rsp) {
			return response_move_funds(rsp);
		});
		bus.subscribe<Msg::RequestEarningsInfo
			     >([this](Msg::RequestEarningsInfo const& req) {
			return Boss::concurrent(request_earnings_info(req));
		});

		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const& _) {
			if (!db)
				return Ev::lift();
			return status();
		});
	}

	Ev::Io<void> init() {
		return db.transact().then([](Sqlite3::Tx tx) {
			tx.query_execute(R"QRY(
			CREATE TABLE IF NOT EXISTS "EarningsTracker"
			     ( node TEXT PRIMARY KEY
			     , in_earnings INTEGER NOT NULL
			     , in_expenditures INTEGER NOT NULL
			     , out_earnings INTEGER NOT NULL
			     , out_expenditures INTEGER NOT NULL
			     );
			)QRY");

			tx.commit();
			return Ev::lift();
		});
	}
	/* Ensure the given node has an entry.  */
	void ensure(Sqlite3::Tx& tx, Ln::NodeId const& node) {
		tx.query(R"QRY(
		INSERT OR IGNORE
		  INTO "EarningsTracker"
		VALUES(:node, 0, 0, 0, 0);
		)QRY")
			.bind(":node", std::string(node))
			.execute()
			;
	}
	Ev::Io<void> forward_fee( Ln::NodeId const& in
				, Ln::NodeId const& out
				, Ln::Amount fee
				) {
		return db.transact().then([this, in, out, fee
					  ](Sqlite3::Tx tx) {
			ensure(tx, in);
			ensure(tx, out);

			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET in_earnings = in_earnings + :fee
			 WHERE node = :node
			     ;
			)QRY")
				.bind(":node", std::string(in))
				.bind(":fee", fee.to_msat())
				.execute()
				;
			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET out_earnings = out_earnings + :fee
			 WHERE node = :node
			     ;
			)QRY")
				.bind(":node", std::string(out))
				.bind(":fee", fee.to_msat())
				.execute()
				;

			tx.commit();
			return Ev::lift();
		});
	}
	Ev::Io<void>
	request_move_funds(Boss::Msg::RequestMoveFunds const& req) {
		auto& pending = pendings[req.requester];
		pending.source = req.source;
		pending.destination = req.destination;
		return Ev::lift();
	}
	Ev::Io<void>
	response_move_funds(Boss::Msg::ResponseMoveFunds const& rsp) {
		auto requester = rsp.requester;
		auto fee = rsp.fee_spent;
		return db.transact().then([this, requester, fee
					  ](Sqlite3::Tx tx) {
			auto it = pendings.find(requester);
			if (it == pendings.end())
				/* Not in our table, huh, weird.  */
				return Ev::lift();
			auto& pending = it->second;

			ensure(tx, pending.source);
			ensure(tx, pending.destination);

			/* Source gets in-expenditures since it gets more
			 * incoming capacity (for more earnings for the
			 * incoming direction).  */
			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET in_expenditures = in_expenditures + :fee
			 WHERE node = :node
			     ;
			)QRY")
				.bind(":node", std::string(pending.source))
				.bind(":fee", fee.to_msat())
				.execute()
				;
			/* Destination gets out-expenditures for same
			 * reason.
			 */
			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET out_expenditures = out_expenditures + :fee
			 WHERE node = :node
			     ;
			)QRY")
				.bind( ":node"
				     , std::string(pending.destination)
				     )
				.bind(":fee", fee.to_msat())
				.execute()
				;

			/* Erase it.  */
			pendings.erase(it);

			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void>
	request_earnings_info(Msg::RequestEarningsInfo const& req) {
		auto requester = req.requester;
		auto node = req.node;
		return db.transact().then([this, requester, node
					  ](Sqlite3::Tx tx) {
			auto in_earnings = Ln::Amount::sat(0);
			auto in_expenditures = Ln::Amount::sat(0);
			auto out_earnings = Ln::Amount::sat(0);
			auto out_expenditures = Ln::Amount::sat(0);

			auto fetch = tx.query(R"QRY(
			SELECT in_earnings, in_expenditures
			     , out_earnings, out_expenditures
			  FROM "EarningsTracker"
			 WHERE node = :node
			     ;
			)QRY")
				.bind(":node", std::string(node))
				.execute()
				;
			for (auto& r : fetch) {
				in_earnings = Ln::Amount::msat(
					r.get<std::uint64_t>(0)
				);
				in_expenditures = Ln::Amount::msat(
					r.get<std::uint64_t>(1)
				);
				out_earnings = Ln::Amount::msat(
					r.get<std::uint64_t>(2)
				);
				out_expenditures = Ln::Amount::msat(
					r.get<std::uint64_t>(3)
				);
			}

			tx.commit();
			return bus.raise(Msg::ResponseEarningsInfo{
				requester, node,
				in_earnings, in_expenditures,
				out_earnings, out_expenditures
			});
		});
	}

	Ev::Io<void> status() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
			SELECT node
			     , in_earnings, in_expenditures
			     , out_earnings, out_expenditures
			  FROM "EarningsTracker"
			     ;
			)QRY").execute();

			uint64_t total_in_earnings = 0;
			uint64_t total_in_expenditures = 0;
			uint64_t total_out_earnings = 0;
			uint64_t total_out_expenditures = 0;

			auto out = Json::Out();
			auto obj = out.start_object();
			for (auto& r : fetch) {
				auto in_earnings = r.get<std::uint64_t>(1);
				auto in_expenditures = r.get<std::uint64_t>(2);
				auto out_earnings = r.get<std::uint64_t>(3);
				auto out_expenditures = r.get<std::uint64_t>(4);
				auto sub = obj.start_object(r.get<std::string>(0));
				sub
					.field("in_earnings", in_earnings)
					.field("in_expenditures", in_expenditures)
					.field("out_earnings", out_earnings)
					.field("out_expenditures", out_expenditures)
					;
				sub.end_object();
				total_in_earnings += in_earnings;
				total_in_expenditures += in_expenditures;
				total_out_earnings += out_earnings;
				total_out_expenditures += out_expenditures;
			}

			auto sub = obj.start_object("total");
			sub
				.field("in_earnings", total_in_earnings)
				.field("in_expenditures", total_in_expenditures)
				.field("out_earnings", total_out_earnings)
				.field("out_expenditures", total_out_expenditures)
				;
			sub.end_object();

			obj.end_object();

			tx.commit();

			return bus.raise(Msg::ProvideStatus{
				"offchain_earnings_tracker",
				std::move(out)
			});
		});
	}

public:
	Impl() =delete;
	Impl(Impl&&) =delete;
	Impl(Impl const&) =delete;

	explicit
	Impl(S::Bus& bus_, std::function<double()> get_now_) : bus(bus_), get_now(std::move(get_now_)) { start(); }
};

EarningsTracker::EarningsTracker(EarningsTracker&&) =default;
EarningsTracker::~EarningsTracker() =default;

EarningsTracker::EarningsTracker(S::Bus& bus, std::function<double()> get_now_ )
	: pimpl(Util::make_unique<Impl>(bus, get_now_)) { }

double EarningsTracker::bucket_time(double input_time) {
	constexpr double seconds_per_day = 24 * 60 * 60;
	double bucket_start = std::floor(input_time / seconds_per_day) * seconds_per_day;
	return bucket_start;
}

}}
