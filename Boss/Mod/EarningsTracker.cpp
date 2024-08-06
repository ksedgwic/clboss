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
			// If we already have a bucket schema we're done
			if (have_bucket_table(tx)) {
				tx.commit();
				return Ev::lift();
			}

			// Create the bucket table w/ a temp name
			tx.query_execute(R"QRY(
			CREATE TABLE IF NOT EXISTS EarningsTracker_New
			     ( node TEXT NOT NULL
			     , time_bucket REAL NOT NULL
			     , in_earnings INTEGER NOT NULL
			     , in_expenditures INTEGER NOT NULL
			     , out_earnings INTEGER NOT NULL
			     , out_expenditures INTEGER NOT NULL
			     , PRIMARY KEY (node, time_bucket)
			     );
			)QRY");

			// If we have a legacy table migrate the data and lose the old table
			if (have_legacy_table(tx)) {
				tx.query_execute(R"QRY(
				INSERT INTO EarningsTracker_New
				    ( node, time_bucket, in_earnings, in_expenditures
				    , out_earnings, out_expenditures)
				SELECT node, 0, in_earnings, in_expenditures
				    , out_earnings, out_expenditures FROM EarningsTracker;
				DROP TABLE EarningsTracker;
			        )QRY");
			}

			// Move the new table to the official name and add the indexes
			tx.query_execute(R"QRY(
			ALTER TABLE EarningsTracker_New RENAME TO EarningsTracker;
			CREATE INDEX IF NOT EXISTS
			    idx_earnings_tracker_node_time ON EarningsTracker (node, time_bucket);
			CREATE INDEX IF NOT EXISTS
			    idx_earnings_tracker_time_node ON EarningsTracker (time_bucket, node);
			)QRY");

			tx.commit();
			return Ev::lift();
		});
	}

	static bool have_bucket_table(Sqlite3::Tx& tx) {
		auto fetch = tx.query(R"QRY(
			SELECT 1
			FROM pragma_table_info('EarningsTracker')
			WHERE name='time_bucket';
			)QRY").execute();
		return fetch.begin() != fetch.end();
	}

	static bool have_legacy_table(Sqlite3::Tx& tx) {
		// Do we have a legacy table defined?
		auto fetch = tx.query(R"QRY(
			SELECT 1
			FROM pragma_table_info('EarningsTracker')
			WHERE name='node';
			)QRY").execute();
		return fetch.begin() != fetch.end();
	}

	/* Ensure the given node has an entry.  */
	void ensure(Sqlite3::Tx& tx, Ln::NodeId const& node, double bucket) {
		tx.query(R"QRY(
		INSERT OR IGNORE
		  INTO "EarningsTracker"
		VALUES(:node, :bucket, 0, 0, 0, 0);
		)QRY")
			.bind(":node", std::string(node))
			.bind(":bucket", bucket)
			.execute()
			;
	}
	Ev::Io<void> forward_fee( Ln::NodeId const& in
				, Ln::NodeId const& out
				, Ln::Amount fee
				) {
		return db.transact().then([this, in, out, fee
					  ](Sqlite3::Tx tx) {
			auto bucket = bucket_time(get_now());
			ensure(tx, in, bucket);
			ensure(tx, out, bucket);

			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET in_earnings = in_earnings + :fee
			 WHERE node = :node
			   AND time_bucket = :bucket
			     ;
			)QRY")
				.bind(":node", std::string(in))
				.bind(":bucket", bucket)
				.bind(":fee", fee.to_msat())
				.execute()
				;
			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET out_earnings = out_earnings + :fee
			 WHERE node = :node
			   AND time_bucket = :bucket
			     ;
			)QRY")
				.bind(":node", std::string(out))
				.bind(":bucket", bucket)
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

			auto bucket = bucket_time(get_now());
			ensure(tx, pending.source, bucket);
			ensure(tx, pending.destination, bucket);

			/* Source gets in-expenditures since it gets more
			 * incoming capacity (for more earnings for the
			 * incoming direction).  */
			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET in_expenditures = in_expenditures + :fee
			 WHERE node = :node
			   AND time_bucket = :bucket
			     ;
			)QRY")
				.bind(":node", std::string(pending.source))
				.bind(":bucket", bucket)
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
			   AND time_bucket = :bucket
			     ;
			)QRY")
				.bind( ":node"
				     , std::string(pending.destination)
				     )
				.bind(":bucket", bucket)
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
			SELECT SUM(in_earnings),
			       SUM(in_expenditures),
			       SUM(out_earnings),
			       SUM(out_expenditures)
			  FROM "EarningsTracker"
			 WHERE node = :node;
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
        		SELECT node,
        		       SUM(in_earnings) AS total_in_earnings,
        		       SUM(in_expenditures) AS total_in_expenditures,
        		       SUM(out_earnings) AS total_out_earnings,
        		       SUM(out_expenditures) AS total_out_expenditures
        		  FROM "EarningsTracker"
        		 GROUP BY node; 
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
