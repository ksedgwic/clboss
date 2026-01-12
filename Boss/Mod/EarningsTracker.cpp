#include"Boss/Mod/EarningsTracker.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RequestEarningsInfo.hpp"
#include"Boss/Msg/RequestMoveFunds.hpp"
#include"Boss/Msg/ResponseEarningsInfo.hpp"
#include"Boss/Msg/ResponseMoveFunds.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/concurrent.hpp"
#include"Ev/Io.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"

#include<cmath>
#include<map>
#include<iostream>
#include<unordered_set>

namespace {

struct EarningsData {
	uint64_t in_earnings = 0;
	uint64_t in_forwarded = 0;
	uint64_t in_expenditures = 0;
	uint64_t in_rebalanced = 0;
	uint64_t out_earnings = 0;
	uint64_t out_forwarded = 0;
	uint64_t out_expenditures = 0;
	uint64_t out_rebalanced = 0;

	// Unmarshal from database fetch
	static EarningsData from_row(Sqlite3::Row& r, size_t& ndx) {
		return {
			r.get<uint64_t>(ndx++),
			r.get<uint64_t>(ndx++),
			r.get<uint64_t>(ndx++),
			r.get<uint64_t>(ndx++),
			r.get<uint64_t>(ndx++),
			r.get<uint64_t>(ndx++),
			r.get<uint64_t>(ndx++),
			r.get<uint64_t>(ndx++)
		};
	}

	// Operator+= for accumulation
	EarningsData& operator+=(const EarningsData& other) {
		in_earnings += other.in_earnings;
		in_forwarded += other.in_forwarded;
		in_expenditures += other.in_expenditures;
		in_rebalanced += other.in_rebalanced;
		out_earnings += other.out_earnings;
		out_forwarded += other.out_forwarded;
		out_expenditures += other.out_expenditures;
		out_rebalanced += other.out_rebalanced;
		return *this;
	}

	template <typename T>
	void to_json(Json::Detail::Object<T>& obj) const {
		obj
			.field("in_earnings", in_earnings)
			.field("in_forwarded", in_forwarded)
			.field("in_expenditures", in_expenditures)
			.field("in_rebalanced", in_rebalanced)
			.field("out_earnings", out_earnings)
			.field("out_forwarded", out_forwarded)
			.field("out_expenditures", out_expenditures)
			.field("out_rebalanced", out_rebalanced);
	}
};

}

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
				     return forward_fee(f.in_id, f.out_id, f.fee, f.amount);
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
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return bus.raise(Msg::ManifestCommand{
				"clboss-recent-earnings", "[days]",
				"Show offchain_earnings_tracker data for the most recent {days} "
				"(default 14 days).",
				false
			}) + bus.raise(Msg::ManifestCommand{
				"clboss-earnings-history", "[nodeid]",
				"Show earnings and expenditure history for {nodeid} "
				"(default all nodes). Use nodeid=all to list per-node buckets.",
				false
			});
		});
		bus.subscribe<Msg::CommandRequest>([this](Msg::CommandRequest const& req) {
			auto id = req.id;
			auto paramfail = [this, id]() {
				return bus.raise(Msg::CommandFail{
						id, -32602,
						"Parameter failure",
						Json::Out::empty_object()
					});
			};
			if (req.command == "clboss-recent-earnings") {
				auto days = double(14.0);
				auto days_j = Jsmn::Object();
				auto params = req.params;
				if (params.is_object()) {
					if (params.size() > 1)
						return paramfail();
					if (params.size() == 1) {
						if (!params.has("days"))
							return paramfail();
						days_j = params["days"];
					}
				} else if (params.is_array()) {
					if (params.size() > 1)
						return paramfail();
					if (params.size() == 1)
						days_j = params[0];
				}
				if (!days_j.is_null()) {
					if (!days_j.is_number())
						return paramfail();
					days = (double) days_j;
				}
				return db.transact().then([this, id, days](Sqlite3::Tx tx) {
					auto report = recent_earnings_report(tx, days);
					tx.commit();
					return bus.raise(Msg::CommandResponse{
							id, report
						});
				});
			} else if (req.command == "clboss-earnings-history") {
				auto nodeid = std::string("");
				auto nodeid_j = Jsmn::Object();
				auto by_node = false;
				auto params = req.params;
				if (params.is_object()) {
					if (params.size() > 1)
						return paramfail();
					if (params.size() == 1) {
						if (!params.has("nodeid"))
							return paramfail();
						nodeid_j = params["nodeid"];
					}
				} else if (params.is_array()) {
					if (params.size() > 1)
						return paramfail();
					if (params.size() == 1)
						nodeid_j = params[0];
				}
				if (!nodeid_j.is_null()) {
					if (!nodeid_j.is_string())
						return paramfail();
					nodeid = (std::string) nodeid_j;
				}
				if (nodeid == "all") {
					by_node = true;
					nodeid.clear();
				}
				return db.transact().then([this, id, nodeid, by_node](Sqlite3::Tx tx) {
					auto report = earnings_history_report(tx, nodeid, by_node);
					tx.commit();
					return bus.raise(Msg::CommandResponse{
							id, report
						});
				});
			}
			return Ev::lift();
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
			// NOTE - we can't just alter table here because we
			// are changing the primary key.

			// If we already have a bucket schema we're done
			if (have_bucket_table(tx)) {
				add_missing_columns(tx);
				tx.commit();
				return Ev::lift();
			}

			// Create the bucket table w/ a temp name
			tx.query_execute(R"QRY(
			CREATE TABLE IF NOT EXISTS EarningsTracker_New
			     ( node TEXT NOT NULL
			     , time_bucket REAL NOT NULL
			     , in_earnings INTEGER NOT NULL
			     , in_forwarded INTEGER NOT NULL
			     , in_expenditures INTEGER NOT NULL
			     , in_rebalanced INTEGER NOT NULL
			     , out_earnings INTEGER NOT NULL
			     , out_forwarded INTEGER NOT NULL
			     , out_expenditures INTEGER NOT NULL
			     , out_rebalanced INTEGER NOT NULL
			     , PRIMARY KEY (node, time_bucket)
			     );
			)QRY");

			// If we have a legacy table migrate the data and lose the old table
			if (have_legacy_table(tx)) {
				tx.query_execute(R"QRY(
				INSERT INTO EarningsTracker_New
				    ( node, time_bucket, in_earnings, in_expenditures
				    , out_earnings, out_expenditures, in_forwarded
				    , in_rebalanced, out_forwarded, out_rebalanced)
				SELECT node, 0, in_earnings, in_expenditures
				    , out_earnings, out_expenditures, 0, 0, 0, 0
				FROM EarningsTracker;
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

		// These statements revert the schema to before time buckets:
		/*
		CREATE TABLE IF NOT EXISTS EarningsTracker_Old
		     ( node TEXT PRIMARY KEY
		     , in_earnings INTEGER NOT NULL
		     , in_expenditures INTEGER NOT NULL
		     , out_earnings INTEGER NOT NULL
		     , out_expenditures INTEGER NOT NULL
 		     );

		INSERT INTO EarningsTracker_Old
		    ( node, in_earnings, in_expenditures
		    , out_earnings, out_expenditures)
		SELECT node
		    , SUM(in_earnings), SUM(in_expenditures)
		    , SUM(out_earnings), SUM(out_expenditures)
		FROM EarningsTracker
		GROUP BY node;

		DROP TABLE EarningsTracker;

		ALTER TABLE EarningsTracker_Old RENAME TO EarningsTracker;
		*/
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

	static void add_missing_columns(Sqlite3::Tx& tx) {
		std::unordered_set<std::string> existing_columns;
		auto columns_query = tx.query("PRAGMA table_info(EarningsTracker);").execute();
		for (auto& row : columns_query) {
			existing_columns.insert(row.get<std::string>(1));
		}

		// These columns are new.
		std::vector<std::pair<std::string, std::string>> columns_to_check = {
			{"in_forwarded", "INTEGER NOT NULL DEFAULT 0"},
			{"in_rebalanced", "INTEGER NOT NULL DEFAULT 0"},
			{"out_forwarded", "INTEGER NOT NULL DEFAULT 0"},
			{"out_rebalanced", "INTEGER NOT NULL DEFAULT 0"},
		};

		// Add missing columns
		for (const auto& [col_name, col_def] : columns_to_check) {
			if (existing_columns.find(col_name) == existing_columns.end()) {
				std::string add_column_query =
					"ALTER TABLE EarningsTracker ADD COLUMN "
					+ col_name + " " + col_def + ";";
				tx.query_execute(add_column_query);
			}
		}
	}

	/* Ensure the given node has an entry.  */
	void ensure(Sqlite3::Tx& tx, Ln::NodeId const& node, double bucket) {
		tx.query(R"QRY(
		INSERT OR IGNORE
		  INTO "EarningsTracker"
		VALUES(:node, :bucket,
                       0, 0, 0, 0,
                       0, 0, 0, 0);
		)QRY")
			.bind(":node", std::string(node))
			.bind(":bucket", bucket)
			.execute()
			;
	}
	Ev::Io<void> forward_fee( Ln::NodeId const& in
				, Ln::NodeId const& out
				, Ln::Amount fee
				, Ln::Amount amount
				) {
		return db.transact().then([this, in, out, fee, amount
					  ](Sqlite3::Tx tx) {
			auto bucket = bucket_time(get_now());
			ensure(tx, in, bucket);
			ensure(tx, out, bucket);

			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET in_earnings = in_earnings + :fee,
                               in_forwarded = in_forwarded + :amount
			 WHERE node = :node
			   AND time_bucket = :bucket
			     ;
			)QRY")
				.bind(":fee", fee.to_msat())
				.bind(":amount", amount.to_msat())
				.bind(":node", std::string(in))
				.bind(":bucket", bucket)
				.execute()
				;
			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET out_earnings = out_earnings + :fee,
                               out_forwarded = out_forwarded + :amount
			 WHERE node = :node
			   AND time_bucket = :bucket
			     ;
			)QRY")
				.bind(":fee", fee.to_msat())
				.bind(":amount", amount.to_msat())
				.bind(":node", std::string(out))
				.bind(":bucket", bucket)
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
		auto amount = rsp.amount_moved;
		return db.transact().then([this, requester, fee, amount
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
			   SET in_expenditures = in_expenditures + :fee,
                               in_rebalanced = in_rebalanced + :amount
			 WHERE node = :node
			   AND time_bucket = :bucket
			     ;
			)QRY")
				.bind(":node", std::string(pending.source))
				.bind(":bucket", bucket)
				.bind(":fee", fee.to_msat())
				.bind(":amount", amount.to_msat())
				.execute()
				;
			/* Destination gets out-expenditures for same
			 * reason.
			 */
			tx.query(R"QRY(
			UPDATE "EarningsTracker"
			   SET out_expenditures = out_expenditures + :fee,
                               out_rebalanced = out_rebalanced + :amount
			 WHERE node = :node
			   AND time_bucket = :bucket
			     ;
			)QRY")
				.bind( ":node"
				     , std::string(pending.destination)
				     )
				.bind(":bucket", bucket)
				.bind(":fee", fee.to_msat())
				.bind(":amount", amount.to_msat())
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
			auto fetch = tx.query(R"QRY(
			SELECT SUM(in_earnings),
			       SUM(in_forwarded),
			       SUM(in_expenditures),
			       SUM(in_rebalanced),
			       SUM(out_earnings),
			       SUM(out_forwarded),
			       SUM(out_expenditures),
			       SUM(out_rebalanced)
			  FROM "EarningsTracker"
			 WHERE node = :node;
			)QRY")
				.bind(":node", std::string(node))
				.execute()
				;

			EarningsData earnings;
			if (fetch.begin() != fetch.end()) {
				size_t ndx = 0;
				earnings = EarningsData::from_row(*fetch.begin(), ndx);
			}

			tx.commit();
			return bus.raise(Msg::ResponseEarningsInfo{
				requester, node,
				Ln::Amount::msat(earnings.in_earnings),
				Ln::Amount::msat(earnings.in_expenditures),
				Ln::Amount::msat(earnings.out_earnings),
				Ln::Amount::msat(earnings.out_expenditures),
				Ln::Amount::msat(earnings.in_forwarded),
				Ln::Amount::msat(earnings.in_rebalanced),
				Ln::Amount::msat(earnings.out_forwarded),
				Ln::Amount::msat(earnings.out_rebalanced),
			});
		});
	}

	Ev::Io<void> status() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto fetch = tx.query(R"QRY(
        		SELECT node,
        		       SUM(in_earnings) AS total_in_earnings,
        		       SUM(in_forwarded) AS total_in_forwarded,
        		       SUM(in_expenditures) AS total_in_expenditures,
        		       SUM(in_rebalanced) AS total_in_rebalanced,
        		       SUM(out_earnings) AS total_out_earnings,
        		       SUM(out_forwarded) AS total_out_forwarded,
        		       SUM(out_expenditures) AS total_out_expenditures,
        		       SUM(out_rebalanced) AS total_out_rebalanced
        		  FROM "EarningsTracker"
        		 GROUP BY node;
			)QRY").execute();

			EarningsData total_earnings;
			auto out = Json::Out();
			auto obj = out.start_object();
			for (auto& r : fetch) {
				size_t ndx = 0;
				auto node = r.get<std::string>(ndx++);
				auto earnings = EarningsData::from_row(r, ndx);
				auto sub = obj.start_object(node);
				earnings.to_json(sub);
				sub.end_object();
				total_earnings += earnings;
			}
			auto total = obj.start_object("total");
			total_earnings.to_json(total);
			total.end_object();
			obj.end_object();

			tx.commit();

			return bus.raise(Msg::ProvideStatus{
				"offchain_earnings_tracker",
				std::move(out)
			});
		});
	}

	Json::Out recent_earnings_report(Sqlite3::Tx& tx, double days) {
		auto cutoff = bucket_time(get_now()) - (days * 24 * 60 * 60);
		auto fetch = tx.query(R"QRY(
        		SELECT node,
        		       SUM(in_earnings) AS total_in_earnings,
        		       SUM(in_forwarded) AS total_in_forwarded,
        		       SUM(in_expenditures) AS total_in_expenditures,
        		       SUM(in_rebalanced) AS total_in_rebalanced,
        		       SUM(out_earnings) AS total_out_earnings,
        		       SUM(out_forwarded) AS total_out_forwarded,
        		       SUM(out_expenditures) AS total_out_expenditures,
        		       SUM(out_rebalanced) AS total_out_rebalanced
        		  FROM "EarningsTracker"
                         WHERE time_bucket >= :cutoff
        		 GROUP BY node
                         ORDER BY (total_in_earnings - total_in_expenditures +
                                   total_out_earnings - total_out_expenditures) DESC;
			)QRY")
			.bind(":cutoff", cutoff)
			.execute()
			;

		EarningsData total_earnings;
		auto out = Json::Out();
		auto top = out.start_object();
		auto recent = top.start_object("recent");
		for (auto& r : fetch) {
			size_t ndx = 0;
			auto node = r.get<std::string>(ndx++);
			auto earnings = EarningsData::from_row(r, ndx);
			auto sub = recent.start_object(node);
			earnings.to_json(sub);
			sub.end_object();
			total_earnings += earnings;
		}
		recent.end_object();

		auto total = top.start_object("total");
		total_earnings.to_json(total);
		total.end_object();

		top.end_object();
		return out;
	}

	Json::Out earnings_history_report(Sqlite3::Tx& tx, std::string nodeid, bool by_node) {
		std::string sql;
		if (by_node) {
		        sql = R"QRY(
		            SELECT time_bucket,
		                   node,
		                   in_earnings,
		                   in_forwarded,
		                   in_expenditures,
		                   in_rebalanced,
		                   out_earnings,
		                   out_forwarded,
		                   out_expenditures,
		                   out_rebalanced
		              FROM "EarningsTracker"
		        )QRY";
			if (!nodeid.empty())
				sql += " WHERE node = :nodeid";
			sql += " ORDER BY time_bucket, node;";
		} else if (nodeid.empty()) {
		        sql = R"QRY(
		            SELECT time_bucket,
		                   SUM(in_earnings) AS total_in_earnings,
		                   SUM(in_forwarded) AS total_in_forwarded,
		                   SUM(in_expenditures) AS total_in_expenditures,
		                   SUM(in_rebalanced) AS total_in_rebalanced,
		                   SUM(out_earnings) AS total_out_earnings,
		                   SUM(out_forwarded) AS total_out_forwarded,
		                   SUM(out_expenditures) AS total_out_expenditures,
		                   SUM(out_rebalanced) AS total_out_rebalanced
		              FROM "EarningsTracker"
		             GROUP BY time_bucket
		             ORDER BY time_bucket;
		        )QRY";
		} else {
		        sql = R"QRY(
		            SELECT time_bucket,
		                   in_earnings,
		                   in_forwarded,
		                   in_expenditures,
		                   in_rebalanced,
		                   out_earnings,
		                   out_forwarded,
		                   out_expenditures,
		                   out_rebalanced
		              FROM "EarningsTracker"
		             WHERE node = :nodeid
		             ORDER BY time_bucket;
		        )QRY";
		}
		auto query = tx.query(sql.c_str());
		if (!nodeid.empty()) {
			query.bind(":nodeid", nodeid);
		}
		auto fetch = query.execute();

		auto out = Json::Out();
		auto top = out.start_object();
		EarningsData total_earnings;
		auto history = top.start_array("history");
		for (auto& r : fetch) {
			size_t ndx = 0;
			auto bucket_time = r.get<double>(ndx++);
			std::string row_node;
			if (by_node) {
				row_node = r.get<std::string>(ndx++);
			}
			auto earnings = EarningsData::from_row(r, ndx);
			auto sub = history.start_object();
			sub.field("bucket_time", static_cast<std::uint64_t>(bucket_time));
			if (by_node)
				sub.field("node", row_node);
			earnings.to_json(sub);
			sub.end_object();
			total_earnings += earnings;
		}
		history.end_array();
		auto total = top.start_object("total");
		total_earnings.to_json(total);
		total.end_object();

		top.end_object();
		return out;
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
