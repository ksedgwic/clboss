#include"Boss/Mod/SendpayResultMonitor.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ManifestNotification.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Notification.hpp"
#include"Boss/Msg/RpcCommandHook.hpp"
#include"Boss/Msg/SendpayResult.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<inttypes.h>

/*
The primary purpose for this module is to track the result of
payment attempts.

If a payment attempt fails, we blame the direct peer through
which the payment is done.

Now it might seem unfair, since the reason for failing might not
be because of the peer itself.

But we lay the blame on the peer, because the peer "should" know
that it is connected to some bad peers itself, and "should"
prefer to close the channels to those.
*/

namespace Boss { namespace Mod {

class SendpayResultMonitor::Impl {
private:
	S::Bus& bus;
	Sqlite3::Db db;

public:
	Impl() =delete;
	Impl(Impl&&) =delete;

	Impl(S::Bus& bus_) : bus(bus_) { start(); }

private:
	void start() {
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const&_) {
			return bus.raise(Msg::ManifestNotification{
					"sendpay_success"
			       })
			     + bus.raise(Msg::ManifestNotification{
					"sendpay_failure"
			       })
			     ;
		});
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& m) {
			db = m.db;
			return db.transact().then([](Sqlite3::Tx tx) {
				tx.query_execute(R"QRY(
				CREATE TABLE
				       IF NOT EXISTS "SendpayResultMonitor"
				     ( payment_hash TEXT NOT NULL
				     , partid INTEGER NOT NULL
				     , first_hop TEXT NOT NULL
				     , creation REAL NOT NULL
				     );
				CREATE UNIQUE INDEX
				       IF NOT EXISTS "SendpayResultMonitor_idx"
				    ON "SendpayResultMonitor"
				       (payment_hash, partid);

				CREATE TABLE IF NOT EXISTS
				       "SendpayResultMonitor_rl"
				      ( payment_hash TEXT NOT NULL
				      , partid INTEGER NOT NULL
				      , route_len INTEGER NOT NULL
				      , FOREIGN KEY(payment_hash, partid)
					REFERENCES "SendpayResultMonitor"
					(payment_hash, partid)
					ON DELETE CASCADE
				      );
				CREATE UNIQUE INDEX IF NOT EXISTS
				       "SendpayResultMonitor_rlidx"
				    ON "SendpayResultMonitor_rl"
				       (payment_hash, partid);
				)QRY");
				tx.commit();
				return Ev::lift();
			});
		});
		/* Extract the first hop for the payment, from the
		 * RPC command used.
		 */
		bus.subscribe<Msg::RpcCommandHook
			     >([this](Msg::RpcCommandHook const& r) {
			return Boss::concurrent(process_rpc_command(r))
			     ;
		});
		/* Check for `sendpay_failure` or `sendpay_success`.
		 */
		bus.subscribe<Msg::Notification
			     >([this](Msg::Notification const& n) {
			if (n.notification == "sendpay_failure")
				return Boss::concurrent(
					on_sendpay_failure(n.params)
				);
			else if (n.notification == "sendpay_success")
				return Boss::concurrent(
					on_sendpay_success(n.params)
				);
			return Ev::lift();
		});
		/* If a recorded sendpay is not seen by us after 30 days,
		 * delete it and ignore.
		 * Our assumption is that it was resolved, but we were
		 * not able to see it for various reason, such as the
		 * node operator neglecting to start us up.
		 * Our assumption as well is that node operator is not
		 * going to increase `max-locktime-blocks`, default 2
		 * weeks, to past 30 days.
		 */
		bus.subscribe<Msg::Timer10Minutes
			     >([this](Msg::Timer10Minutes const& _) {
			return Boss::concurrent(on_periodic());
		});
	}

	/* Record the arguments of `sendonion` and `sendpay`
	 * commands.
	 */
	Ev::Io<void>
	process_rpc_command(Msg::RpcCommandHook const& r) {
		auto params = std::make_shared<Jsmn::Object>(r.params);
		return Ev::lift().then([this, params]() {
			try {
				auto payload = (*params)["rpc_command"];
				auto m = std::string(payload["method"]);
				auto p = payload["params"];
				if (m == "sendonion")
					return process_sendonion(p);
				else if (m == "sendpay")
					return process_sendpay(p);
			} catch (Jsmn::TypeError const& _) {
				return Boss::log( bus, Error
						, "SendpayResultMonitor: "
						  "Unexpected rpc_command "
						  "payload: %s"
						, Util::stringify(*params)
							.c_str()
						);
			}
			return Ev::lift();
		}).catching<std::runtime_error>([this
					       ](std::runtime_error const& e) {
			/* A database error, typically the file locked
			 * by another process reading it (a backup) for
			 * longer than the busy timeout.  The sendpay goes
			 * unrecorded and its result will be ignored; the
			 * plugin carries on (#335).  */
			return Boss::log( bus, Error
					, "SendpayResultMonitor: sendpay not "
					  "recorded, database error: %s"
					, e.what()
					);
		});
	}
	Ev::Io<void>
	process_sendonion(Jsmn::Object const& params) {
		auto first_hop_j = Jsmn::Object();
		auto payment_hash_j = Jsmn::Object();
		auto partid_j = Jsmn::Object();
		auto shared_secrets_j = Jsmn::Object();
		if (params.is_array() && params.size() >= 3) {
			first_hop_j = params[1];
			payment_hash_j = params[2];
			if (params.size() >= 6)
				partid_j = params[5];
			if (params.size() >= 5)
				shared_secrets_j = params[4];
		} else if (params.is_object()) {
			if (params.has("first_hop"))
				first_hop_j = params["first_hop"];
			if (params.has("payment_hash"))
				payment_hash_j = params["payment_hash"];
			if (params.has("partid"))
				partid_j = params["partid"];
			if (params.has("shared_secrets"))
				shared_secrets_j = params["shared_secrets"];
		}
		auto len = std::unique_ptr<std::size_t>();
		if (shared_secrets_j.is_array())
			len = Util::make_unique<std::size_t>(
				shared_secrets_j.size()
			);
		return record_pending_sendpay_j( payment_hash_j
					       , partid_j
					       , first_hop_j
					       , std::move(len)
					       );
	}
	Ev::Io<void>
	process_sendpay(Jsmn::Object const& params) {
		auto route_j = Jsmn::Object();
		auto payment_hash_j = Jsmn::Object();
		auto partid_j = Jsmn::Object();
		if (params.is_array() && params.size() >= 2) {
			route_j = params[0];
			payment_hash_j = params[1];
			if (params.size() >= 7)
				partid_j = params[6];
		} else if (params.is_object()) {
			if (params.has("route"))
				route_j = params["route"];
			if (params.has("payment_hash"))
				payment_hash_j = params["payment_hash"];
			if (params.has("partid"))
				partid_j = params["partid"];
		}
		if ( !route_j.is_array()
		  || route_j.size() < 1
		   )
			return Ev::lift();
		auto first_hop_j = route_j[0];
		auto len = Util::make_unique<std::size_t>(route_j.size());

		return record_pending_sendpay_j( payment_hash_j
					       , partid_j
					       , first_hop_j
					       , std::move(len)
					       );
	}
	Ev::Io<void>
	record_pending_sendpay_j( Jsmn::Object const& payment_hash_j
				, Jsmn::Object const& partid_j
				, Jsmn::Object const& first_hop_j
				, std::unique_ptr<std::size_t> route_len
				) {
		/* Params come from external, so do not get fooled.  */
		if ( first_hop_j.is_object()
		  && first_hop_j.has("id")
		  && first_hop_j["id"].is_string()
		  && Ln::NodeId::valid_string(std::string(first_hop_j["id"]))
		  && payment_hash_j.is_string()
		  && Sha256::Hash::valid_string(std::string(payment_hash_j))
		  && (partid_j.is_null() || partid_j.is_number())
		   ) {
			auto first_hop = Ln::NodeId(std::string(
				first_hop_j["id"]
			));
			auto payment_hash = Sha256::Hash(std::string(
				payment_hash_j
			));
			auto partid = std::uint64_t();
			if (partid_j.is_null())
				partid = 0;
			else
				partid = std::uint64_t(double(partid_j));
			return record_pending_sendpay( std::move(payment_hash)
						     , partid
						     , std::move(first_hop)
						     , std::move(route_len)
						     );
		}
		return Ev::lift();
	}
	Ev::Io<void>
	record_pending_sendpay( Sha256::Hash payment_hash
			      , std::uint64_t partid
			      , Ln::NodeId first_hop
			      , std::unique_ptr<std::size_t> n_route_len
			      ) {
		/* Promote unique pointer to shared for easier copying.  */
		auto route_len = std::shared_ptr<std::size_t>(
			std::move(n_route_len)
		);
		return db.transact().then([ this
					  , payment_hash
					  , partid
					  , first_hop
					  , route_len
					  ](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			INSERT OR REPLACE
			  INTO "SendpayResultMonitor"
			VALUES( :payment_hash
			      , :partid
			      , :first_hop
			      , :creation
			      )
			)QRY")
				.bind( ":payment_hash"
				     , std::string(payment_hash)
				     )
				.bind(":partid" , partid)
				.bind( ":first_hop"
				     , std::string(first_hop)
				     )
				.bind(":creation", Ev::now())
				.execute()
				;
			if (route_len) {
				tx.query(R"QRY(
				INSERT OR REPLACE
				  INTO "SendpayResultMonitor_rl"
				VALUES( :payment_hash
				      , :partid
				      , :route_len
				      );
				)QRY")
					.bind( ":payment_hash"
					     , std::string(payment_hash)
					     )
					.bind(":partid" , partid)
					.bind(":route_len", *route_len)
					.execute()
					;
			}
			tx.commit();
			return Boss::log( bus, Debug
					, "SendpayResultMonitor: "
					  "Monitoring %s part %" PRIu64
					  " peer %s route_len %s."
					, std::string(payment_hash).c_str()
					, partid
					, std::string(first_hop).c_str()
					, route_len ?
						Util::stringify(*route_len)
							.c_str() :
						"unknown"
					);
		});
	}

	Ev::Io<void> on_sendpay_failure(Jsmn::Object const& params) {
		if (!params.is_object() || !params.has("sendpay_failure"))
			return Ev::lift();
		auto payload = params["sendpay_failure"];
		/* For `sendpay_failure` the data we need is wrapped in
		 * a `data` object.
		 * We also have to extract the `code` integer.
		 */
		if (!payload.is_object() || !payload.has("data"))
			return Ev::lift();
		auto data = payload["data"];
		if (!data.is_object())
			return Ev::lift();

		if (!payload.has("code"))
			return Ev::lift();
		auto code = payload["code"];
		if (!code.is_number())
			return Ev::lift();

		/* We determine if we reached the destination
		 * by checking the erring_index and seeing
		 * if it is the correct path length.
		 * Some errors at the destination, such as
		 * `mpp_timeout`, are not permanent, and would
		 * not trigger `PAY_DESTINATION_PERM_FAIL`.
		 */
		if (!data.has("erring_index"))
			return Ev::lift();
		auto erring_index_j = data["erring_index"];
		if (!erring_index_j.is_number())
			return Ev::lift();
		auto erring_index = std::size_t(double(erring_index_j));
		return on_sendpay_resolve( data
					 , false
					 , erring_index
					 );
	}
	Ev::Io<void> on_sendpay_success(Jsmn::Object const& params) {
		if (!params.is_object() || !params.has("sendpay_success"))
			return Ev::lift();
		auto payload = params["sendpay_success"];
		return on_sendpay_resolve(payload, true, 0);
	}
	Ev::Io<void> on_sendpay_resolve( Jsmn::Object const& data
				       , bool success
				       , std::size_t erring_index
				       ) {
		if (!data.has("payment_hash"))
			return Ev::lift();
		auto payment_hash_s = std::string(data["payment_hash"]);
		if (!Sha256::Hash::valid_string(payment_hash_s))
			return Ev::lift();
		auto payment_hash = Sha256::Hash(payment_hash_s);

		auto partid = std::uint64_t(0);
		if (data.has("partid") && data["partid"].is_number())
			partid = std::uint64_t(double(data["partid"]));

		return db.transact().then([ this
					  , payment_hash
					  , partid
					  , success
					  , erring_index
					  ](Sqlite3::Tx tx) {
			auto first_hop = Ln::NodeId();
			auto creation = double();
			auto route_len = std::unique_ptr<std::size_t>();
			auto found = get_sendpay_data( tx
						     , payment_hash
						     , partid

						     , first_hop
						     , creation
						     , route_len
						     );

			if (!found)
				/* Do nothing.  */
				return Ev::lift();

			auto reached_destination = success;
			if (!success && route_len)
				/* If the erring index is at the destination,
				 * we actually did in fact reach the
				 * destination.
				 * Use <= in case in the future rendezvous
				 * routing somehow gets implemented.
				 * That would mean we would not know the
				 * shared secrets within the postfix, and
				 * as far as we know the route length is
				 * only up to the rendezvous point.
				 */
				if (*route_len <= erring_index)
					reached_destination = true;

			/* Remove it.  */
			tx.query(R"QRY(
			DELETE FROM "SendpayResultMonitor"
			 WHERE payment_hash = :payment_hash
			   AND partid = :partid
			     ;
			)QRY")
				.bind( ":payment_hash"
				     , std::string(payment_hash)
				     )
				.bind(":partid", partid)
				.execute();
			tx.commit();
			/* Report and broadcast.  */
			auto act = Ev::lift();
			act += Boss::log( bus, Debug
					, "SendpayResultMonitor: "
					  "Resolved %s part %" PRIu64
					  " peer %s: %s, %s%s"
					, std::string(payment_hash).c_str()
					, partid
					, std::string(first_hop).c_str()
					, success ? "success" : "failure"
					, reached_destination ?
						"reached destination" :
						"destination not reached"
					, success ? "" :
						( std::string(", ")
						+ "erring_index: "
						+ Util::stringify(erring_index)
						).c_str()
					);
			act += bus.raise(Msg::SendpayResult{
					creation,
					Ev::now(),
					std::move(first_hop),
					std::move(payment_hash),
					partid,
					success,
					reached_destination
			       });
			return act;
		});
	}

	bool get_sendpay_data( Sqlite3::Tx& tx
			     , Sha256::Hash const& payment_hash
			     , std::uint64_t partid
			     /* Data to extract.  */
			     , Ln::NodeId& first_hop
			     , double& creation
			     , std::unique_ptr<std::size_t>& route_len
			     ) {
		auto fetch = tx.query(R"QRY(
		SELECT first_hop, creation
		  FROM "SendpayResultMonitor"
		 WHERE payment_hash = :payment_hash
		   AND partid = :partid
		     ;
		)QRY")
			.bind( ":payment_hash"
			     , std::string(payment_hash)
			     )
			.bind(":partid", partid)
			.execute();

		auto found = false;
		for (auto& r : fetch) {
			first_hop = Ln::NodeId(
				r.get<std::string>(0)
			);
			creation = r.get<double>(1);
			found = true;
		}

		if (!found)
			return false;

		auto fetch2 = tx.query(R"QRY(
		SELECT route_len
		  FROM "SendpayResultMonitor_rl"
		 WHERE payment_hash = :payment_hash
		   AND partid = :partid
		     ;
		)QRY")
			.bind( ":payment_hash"
			     , std::string(payment_hash)
			     )
			.bind(":partid", partid)
			.execute();
		/* This could return empty, i.e. from old database.  */
		for (auto& r : fetch2)
			route_len = Util::make_unique<std::size_t>(
				r.get<std::size_t>(0)
			);

		return found;
	}

	Ev::Io<void> on_periodic() {
		return db.transact().then([](Sqlite3::Tx tx) {
			/* Just drop rows whose creation is more
			 * than a month ago.
			 * TODO: maybe a better solution would be to
			 * get `delay` from first hop and add some margin.
			 */
			tx.query(R"QRY(
			DELETE FROM "SendpayResultMonitor"
			 WHERE creation + (30 * 24 * 3600) < :current_time
			     ;
			)QRY")
				.bind(":current_time", Ev::now())
				.execute();
			tx.commit();
			return Ev::lift();
		});
	}
};

SendpayResultMonitor::SendpayResultMonitor(SendpayResultMonitor&&) =default;
SendpayResultMonitor::~SendpayResultMonitor() =default;

SendpayResultMonitor::SendpayResultMonitor
	(S::Bus& bus) : pimpl(Util::make_unique<Impl>(bus)) { }

}}
