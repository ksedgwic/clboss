#include"Boss/Mod/PeerStatistician.hpp"
#include"Boss/Msg/CommandFail.hpp"
#include"Boss/Msg/CommandRequest.hpp"
#include"Boss/Msg/CommandResponse.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/ForwardFee.hpp"
#include"Boss/Msg/InternetOnline.hpp"
#include"Boss/Msg/ListpeersAnalyzedResult.hpp"
#include"Boss/Msg/ManifestCommand.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/RequestPeerStatistics.hpp"
#include"Boss/Msg/ResponsePeerStatistics.hpp"
#include"Boss/Msg/SendpayResult.hpp"
#include"Boss/Msg/TimerRandomDaily.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include<vector>

/*
An important consideration in designing metrics for
whether a peer is bad or not, is to ensure that a
clever hacker cannot costlessly game those metrics
to hurt their competitors, or artificially improve
their own.

This eliminates one particular class of data:

* Payment forwards that failed.

This is because this metric is costlessly gameable.

If I use, for example, the metric "successful
forwards divided by total forwards", then the "total
forwards" is the sum of "failed forwards" (which is
trivially gameable!) plus "successful forwards".
A savvy hacker can then destroy this metric of a
competitor by picking random hashes, then sending
payments to the competitor, which the competitor
cannot claim.
This would then be a "failed forward" from my
point-of-view, since I cannot read the onion-wrapped
error message of the peer.

Thus, we can only use:

* Data from our own actions, e.g. payments from us,
  such as from the `SendpayResultMonitor`.
  Since this is our own action, others cannot
  game this data.
  * ***Unless*** they can influence our actions!
    For example, if we are a custodial service
    (which is evil, but CLBOSS needs to support
    custodial services until their operators see
    the light and stop being evil), then our
    victims can fight back by creating artificial
    invoices that have routehints pointing to our
    peers that they want to attack, but referring
    to nonexistent channels to nonexistent nodes.
    The victim then asks us to withdraw funds
    using this artificial invoice.
    The peer would have to fail those invoices,
    and that would artificially reduce reasonable
    metrics of that peer.
  * So we support a `clboss-externpay` command,
    which you have to issue *before* performing a
    `pay` from an invoice provided by one of your
    victims.
    This tells CLBOSS that a `pay` command with
    specific hash (which you can extract by
    `decodepay`) was not performed by your own
    volition (i.e. third-party gameable, i.e.
    "external") and should be ignored by the peer
    statistician.
* Fees earned from successful forwards.
  * Number of successful forwards is gameable, by
    paying tiny inconsequential amounts.
  * Actual substantial fees on successful forwards
    are effectively bribes by the "attacker" for us
    to maintain channels to our peers.
    Since our entire point for existence is to earn
    money by maintaining channels with our peers,
    such subsidy would be something we want to keep
    on coming in, at least until the attacker runs
    out of subsidy.
    i.e. it is "free money" for us, so sure, we will
    play along and keep on getting free money.

Another wrinkle here is that we count a payment we
initiated as a "success" if it reached the destination
at all.
Even if the payment failed at the destination, this
module still counts that as a success.
After all, this module is responsible for evaluating
peers for how good they are at getting payments sent
to our destinations, and any problems at the
destination are definitely not the fault of the peer.

For example, consider an MPP payment.
Some number of payments reached the destination via a
particularly "good" peer, but because some of our
*other* peers are unreliable, the entire payment
fails because the sub-payments sent via the other
unreliable peers failed.
We should still count the peer that managed to get
their sub-payment to the destination as "good".

This also allows us to work with active probing
modules without requiring specific support for those.
Active probing simply means generating routes to
random nodes on the network, getting a random hash,
and trying to reach the node via the route using the
random hash.
If the trial payment reaches the destination, that
means the route to it is fairly reliable, and also
that direct peer on that route is probably fairly
reliable.
*/

namespace {

/* Entries older than this number of seconds are deleted.  */
auto const max_entry_age = double(3 * 30 * 24 * 60 * 60);

}

namespace Boss { namespace Mod {

class PeerStatistician::Impl {
private:
	S::Bus& bus;

	Sqlite3::Db db;

	bool online;

	void start() {
		online = false;
		bus.subscribe<Msg::InternetOnline
			     >([this](Msg::InternetOnline const& m) {
			online = m.online;
			return Ev::lift();
		});
		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& m) {
			db = m.db;
			return init_db() + clean_entries();
		});
		bus.subscribe<Msg::TimerRandomDaily
			     >([this](Msg::TimerRandomDaily const& m) {
			if (!db)
				return Ev::lift();
			return clean_entries();
		});
		bus.subscribe<Msg::SendpayResult
			     >([this](Msg::SendpayResult const& r) {
			if (!db)
				return Ev::lift();
			return add_sendpayresult(r);
		});
		bus.subscribe<Msg::ListpeersAnalyzedResult
			     >([this](Msg::ListpeersAnalyzedResult const& r) {
			if (!db)
				return Ev::lift();
			/* At initial startup, we have not connected to any
			 * peers yet, and if we are at "initial", then that
			 * means that it was *us* that was offline, so we
			 * should not blame (lower the score of) the peers.
			 */
			if (r.initial)
				return Ev::lift();
			/* Similarly, if *we* are offline, do not demerit
			 * our peers!  */
			if (!online)
				return Ev::lift();
			/* Likewise when we have channeled peers and not
			 * one of them is connected: a total blackout is
			 * our own outage (lightningd --offline, a Tor or
			 * firewall failure), not every peer failing at
			 * once, and the internet probe behind `online`
			 * cannot see it (#346).  */
			if (r.connected_channeled.empty()
			 && !r.disconnected_channeled.empty())
				return Ev::lift();

			return db.transact().then([this, r](Sqlite3::Tx tx) {
				/* We also gather all channeled peers to a
				 * new set.  */
				auto all_channeled = std::set<Ln::NodeId>();
				for (auto const& n : r.connected_channeled) {
					add_connection(tx, n, true);
					all_channeled.insert(n);
				}
				for (auto const& n : r.disconnected_channeled
				    ) {
					add_connection(tx, n, false);
					all_channeled.insert(n);
				}

				/* Now destroy data for peers without
				 * channels.  */
				destroy_unchanneled(tx, all_channeled);

				tx.commit();

				return Ev::lift();
			});
		});
		bus.subscribe<Msg::ForwardFee
			     >([this](Msg::ForwardFee const& m) {
			if (!db)
				return Ev::lift();
			return add_forwardfee(m);
		});
		bus.subscribe<Msg::RequestPeerStatistics
			     >([this](Msg::RequestPeerStatistics const& r) {
			return Boss::concurrent( wait_db_available()
					       + get_stats(r)
					       );
		});

		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return bus.raise(Msg::ManifestCommand{
				"clboss-externpay", "payment_hash",
				"Inform CLBOSS that a future `pay` command "
				"with the given `payment_hash` is controlled "
				"by a third party that might have incentive "
				"to damage the metrics of our peers. "
				"Relevant only to custodial services.",
				false
			});
		});
		bus.subscribe<Msg::CommandRequest
			     >([this](Msg::CommandRequest const& c) {
			if (c.command != "clboss-externpay")
				return Ev::lift();
			auto hash_j = Jsmn::Object();
			if (c.params.is_array() && c.params.size() == 1)
				hash_j = c.params[0];
			else if ( c.params.is_object()
			       && c.params.has("payment_hash")
				)
				hash_j = c.params["payment_hash"];
			if ( !hash_j.is_string()
			  || !Sha256::Hash::valid_string(std::string(hash_j))
			   )
				return bus.raise(Msg::CommandFail{
					c.id, -32602,
					"Need valid `payment_hash` parameter",
					Json::Out::empty_object()
				});
			auto hash = Sha256::Hash(std::string(hash_j));
			auto act = wait_db_available()
				 + externpay(hash)
				 + bus.raise(Msg::CommandResponse{
					c.id, Json::Out::empty_object()
				   })
				 ;
			return Boss::concurrent(act);
		});
	}

	Ev::Io<void>
	init_db() {
		return db.transact().then([](Sqlite3::Tx tx) {
			tx.query_execute(R"QRY(
			-- Table of when we first started recording
			-- information about a peer.
			-- When measuring a particular time period,
			-- if the period starts before the recorded
			-- information here, we adjust the start time
			-- to the creation time of the peer.
			CREATE TABLE IF NOT EXISTS
			       "PeerStatistician_peers"
			     ( id TEXT PRIMARY KEY
			     -- time this entry was created.
			     , creation REAL NOT NULL
			     );

			CREATE TABLE IF NOT EXISTS
			       "PeerStatistician_sendpayresults"
			     -- peer
			     ( id TEXT NOT NULL
			       REFERENCES "PeerStatistician_peers"(id)
			       ON DELETE CASCADE
			       ON UPDATE RESTRICT
			     -- time this entry was created.
			     -- We only maintain entries for 3 months
			     -- to bound our data usage.
			     , creation REAL NOT NULL

			     -- Amount of wallclock time that our
			     -- funds were locked in the sendpay or
			     -- sendonion.
			     , lockrealtime REAL NOT NULL
			     -- Whether this attempt reached the
			     -- destination or not.
			     , destination_reached INTEGER NOT NULL

			     );
			CREATE INDEX IF NOT EXISTS
			       "PeerStatistician_sendpayresults_timeidx"
			    ON "PeerStatistician_sendpayresults"
			       (creation);
			CREATE INDEX IF NOT EXISTS
			       "PeerStatistician_sendpayresults_ididx"
			    ON "PeerStatistician_sendpayresults"
			       (id);

			CREATE TABLE IF NOT EXISTS
			       "PeerStatistician_connection"
			     ( id TEXT NOT NULL
			       REFERENCES "PeerStatistician_peers"(id)
			       ON DELETE CASCADE
			       ON UPDATE RESTRICT
			     , creation REAL NOT NULL

			     -- Whether it was connected at the time.
			     , connected INTEGER NOT NULL
			     );
			CREATE INDEX IF NOT EXISTS
			       "PeerStatistician_connection_timeidx"
			    ON "PeerStatistician_connection"
			       (creation);
			CREATE INDEX IF NOT EXISTS
			       "PeerStatistician_connection_ididx"
			    ON "PeerStatistician_connection"
			       (id);

			CREATE TABLE IF NOT EXISTS
			       "PeerStatistician_forwardfees"
			     -- incoming peer.
			     ( in_id TEXT NOT NULL
			       REFERENCES "PeerStatistician_peers"(id)
			       ON DELETE CASCADE
			       ON UPDATE RESTRICT
			     -- outgoing peer.
			     , out_id TEXT NOT NULL
			       REFERENCES "PeerStatistician_peers"(id)
			       ON DELETE CASCADE
			       ON UPDATE RESTRICT
			     -- time this *successful* forwarding
			     -- event was recorded.
			     , creation REAL NOT NULL
			     -- millisatoshis actually earned.
			     , fee INTEGER NOT NULL
			     -- Time it took for this forwarding event
			     -- to be finished.
			     , resolution_time REAL NOT NULL
			     );
			CREATE INDEX IF NOT EXISTS
			       "PeerStatistician_forwardfees_timeidx"
			    ON "PeerStatistician_forwardfees"
			       (creation);
			CREATE INDEX IF NOT EXISTS
			       "PeerStatistician_forwardfees_inididx"
			    ON "PeerStatistician_forwardfees"
			       (in_id);
			CREATE INDEX IF NOT EXISTS
			       "PeerStatistician_forwardfees_outididx"
			    ON "PeerStatistician_forwardfees"
			       (out_id);

			CREATE TABLE IF NOT EXISTS
			       "PeerStatistician_externpays"
			     ( hash TEXT PRIMARY KEY
			     , creation REAL NOT NULL
			     );
			CREATE INDEX IF NOT EXISTS
			       "PeerStatistician_externpays_timeidx"
			    ON "PeerStatistician_externpays"
			       (creation);
			)QRY");
			/* Here are the things we want to derive from
			the above data:

			FROM "PeerStatistician_sendpaytries"
			lockrealtime = SUM(lockrealtime)
			tries = COUNT(*)
			reaches = COUNT(*)
				WHERE destination_reached = 1

			FROM "PeerStatistician_connection"
			conns = COUNT(*)
				WHERE connected = 1
			connchecks = COUNT(*)

			FROM "PeerStatistician_forwardfees"
			fees = SUM(fee)

			--

			Then here are metrics

			`realtime_per_attempt`
			lockrealtime / tries - average time that a
			payment through the node takes.
			Bad if this is high.

			`success_per_attempt`
			reaches / tries - reliability of payments
			through the node, i.e. rate that we can
			expect the payment to actually reach the
			destination.
			Bad if this is low.

			`success_per_second`
			reaches / TIME - how often we found
			this node useful in reaching the destination.
			Bad if this is low.

			`connected_rate`
			conns / connchecks - how often we found the
			peer to be connected with us.
			Bad if this is even much less than 100%.

			`fee_msat_per_second`
			fees / TIME - earning rate from the peer.
			Good if this is high.
			*/

			tx.commit();
			return Ev::lift();
		});
	}

	/* Cleans up old entries.  */
	Ev::Io<void> clean_entries() {
		return db.transact().then([](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			DELETE FROM "PeerStatistician_sendpayresults"
			 WHERE creation + :max_age < :now
			     ;
			)QRY")
				.bind(":max_age", max_entry_age)
				.bind(":now", Ev::now())
				.execute();
			tx.query(R"QRY(
			DELETE FROM "PeerStatistician_connection"
			 WHERE creation + :max_age < :now
			     ;
			)QRY")
				.bind(":max_age", max_entry_age)
				.bind(":now", Ev::now())
				.execute();
			tx.query(R"QRY(
			DELETE FROM "PeerStatistician_forwardfees"
			 WHERE creation + :max_age < :now
			     ;
			)QRY")
				.bind(":max_age", max_entry_age)
				.bind(":now", Ev::now())
				.execute();
			tx.query(R"QRY(
			DELETE FROM "PeerStatistician_externpays"
			 WHERE creation + :max_age < :now
			     ;
			)QRY")
				.bind(":max_age", max_entry_age)
				.bind(":now", Ev::now())
				.execute();

			tx.commit();
			return Ev::lift();
		});
	}

	/* Creates an entry in PeerStatistician_peers.  */
	void make_entry(Sqlite3::Tx& tx, Ln::NodeId const& id) {
		tx.query(R"QRY(
		INSERT OR IGNORE
		  INTO "PeerStatistician_peers"
		VALUES( :id
		      , :now
		      );
		)QRY")
			.bind(":id", std::string(id))
			.bind(":now", Ev::now())
			.execute();
	}

	Ev::Io<void>
	add_sendpayresult(Msg::SendpayResult const& r) {
		auto id = r.first_hop;
		auto creation = Ev::now();
		auto lockrealtime = r.completion_time - r.creation_time;
		auto payment_hash = r.payment_hash;
		auto destination_reached = r.reached_destination;
		return db.transact().then([ this
					  , id
					  , creation
					  , lockrealtime
					  , payment_hash
					  , destination_reached
					  ](Sqlite3::Tx tx) {
			auto check = tx.query(R"QRY(
			SELECT 1 FROM "PeerStatistician_externpays"
			WHERE hash = :hash
			    ;
			)QRY")
				.bind(":hash", std::string(payment_hash))
				.execute()
				;
			auto in_externpays = false;
			for (auto& r : check) {
				(void) r;
				in_externpays = true;
			}
			if (in_externpays)
				return Boss::log( bus, Debug
						, "PeerStatistician: Ignore "
						  "result of external "
						  "payment hash %s."
						, std::string(payment_hash)
							.c_str()
						);

			make_entry(tx, id);
			tx.query(R"QRY(
			INSERT INTO "PeerStatistician_sendpayresults"
			VALUES( :id
			      , :creation
			      , :lockrealtime
			      , :destination_reached
			      );
			)QRY")
				.bind(":id", std::string(id))
				.bind(":creation", creation)
				.bind(":lockrealtime", lockrealtime)
				.bind( ":destination_reached"
				     , destination_reached ? 1 : 0
				     )
				.execute();

			tx.commit();
			return Ev::lift();
		});
	}

	void add_connection( Sqlite3::Tx& tx
			   , Ln::NodeId const& id
			   , bool connected
			   ) {
		make_entry(tx, id);
		tx.query(R"QRY(
		INSERT INTO "PeerStatistician_connection"
		VALUES( :id
		      , :creation
		      , :connected
		      );
		)QRY")
			.bind(":id", std::string(id))
			.bind(":creation", Ev::now())
			.bind(":connected", connected)
			.execute();
	}
	void destroy_unchanneled( Sqlite3::Tx& tx
				, std::set<Ln::NodeId> const& channeled
				) {
		auto to_del = std::vector<Ln::NodeId>();

		/* Gather all nodes in our db.  */
		auto fetch = tx.query(R"QRY(
		SELECT id FROM "PeerStatistician_peers";
		)QRY").execute();
		for (auto& r : fetch) {
			auto peer = Ln::NodeId(r.get<std::string>(0));
			auto it = channeled.find(peer);
			if (it == channeled.end())
				to_del.push_back(peer);
		}
		/* And delete.  */
		for (auto const& p : to_del) {
			/* This also deletes entries from the other
			 * tables by foreign key constraint.  */
			tx.query(R"QRY(
			DELETE FROM "PeerStatistician_peers"
			 WHERE id = :id
			     ;
			)QRY")
				.bind(":id", std::string(p))
				.execute();
		}
	}

	Ev::Io<void> add_forwardfee(Msg::ForwardFee const& m) {
		return db.transact().then([this, m](Sqlite3::Tx tx) {
			make_entry(tx, m.in_id);
			make_entry(tx, m.out_id);
			tx.query(R"QRY(
			INSERT INTO "PeerStatistician_forwardfees"
			VALUES( :in_id
			      , :out_id
			      , :creation
			      , :fee
			      , :resolution_time
			      );
			)QRY")
				.bind(":in_id", std::string(m.in_id))
				.bind(":out_id", std::string(m.out_id))
				.bind(":creation", Ev::now())
				.bind(":fee", m.fee.to_msat())
				.bind(":resolution_time", m.resolution_time)
				.execute();

			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> wait_db_available() {
		return Ev::lift().then([this]() {
			if (db)
				return Ev::lift();
			return Ev::yield() + wait_db_available();
		});
	}

	Ev::Io<void> get_stats(Msg::RequestPeerStatistics const& r) {
		return db.transact().then([this, r](Sqlite3::Tx tx) {
			auto data = std::map< Ln::NodeId
					    , Msg::PeerStatistics
					    >();
			auto start_time = r.start_time;
			auto end_time = r.end_time;

			auto sendpays = tx.query(R"QRY(
			SELECT id, lockrealtime, destination_reached
			  FROM "PeerStatistician_sendpayresults"
			 WHERE :start_time <= creation
			   AND creation <= :end_time
			     ;
			)QRY")
				.bind(":start_time", start_time)
				.bind(":end_time", end_time)
				.execute()
				;
			for (auto& r : sendpays) {
				auto node = Ln::NodeId(r.get<std::string>(0));
				auto& entry = get_entry( tx, data, node
						       , start_time, end_time
						       );
				entry.lockrealtime += r.get<double>(1);
				++entry.attempts;
				if (0 != r.get<int>(2))
					++entry.successes;
			}
			auto connects = tx.query(R"QRY(
			SELECT id, connected
			  FROM "PeerStatistician_connection"
			 WHERE :start_time <= creation
			   AND creation <= :end_time
			     ;
			)QRY")
				.bind(":start_time", start_time)
				.bind(":end_time", end_time)
				.execute()
				;
			for (auto& r : connects) {
				auto node = Ln::NodeId(r.get<std::string>(0));
				auto& entry = get_entry( tx, data, node
						       , start_time, end_time
						       );
				++entry.connect_checks;
				if (0 != r.get<int>(1))
					++entry.connects;
			}
			auto fees = tx.query(R"QRY(
			SELECT in_id, out_id, fee
			  FROM "PeerStatistician_forwardfees"
			 WHERE :start_time <= creation
			   AND creation <= :end_time
			     ;
			)QRY")
				.bind(":start_time", start_time)
				.bind(":end_time", end_time)
				.execute()
				;
			for (auto& r : fees) {
				auto in = Ln::NodeId(r.get<std::string>(0));
				auto& in_e = get_entry( tx, data, in
						      , start_time, end_time
						      );
				in_e.in_fee += Ln::Amount::msat(
					r.get<std::uint64_t>(2)
				);
				auto out = Ln::NodeId(r.get<std::string>(1));
				auto& out_e = get_entry( tx, data, out
						       , start_time, end_time
						       );
				out_e.out_fee += Ln::Amount::msat(
					r.get<std::uint64_t>(2)
				);
			}

			tx.commit();
			return bus.raise(Msg::ResponsePeerStatistics{
				r.requester, std::move(data)
			});
		});
	}
	/* Used by get_stats above, to ensure that the given node
	 * has an entry in the given peer statistics data.
	 * If not yet, we initialize it and the appropriate time
	 * as well.
	 */
	Msg::PeerStatistics&
	get_entry( Sqlite3::Tx& tx
		 , std::map<Ln::NodeId, Msg::PeerStatistics>& data
		 , Ln::NodeId const& node
		 , double start_time
		 , double end_time
		 ) {
		auto it = data.find(node);
		if (it != data.end())
			return it->second;

		auto& entry = data[node];
		auto fetch = tx.query(R"QRY(
		SELECT creation
		  FROM "PeerStatistician_peers"
		 WHERE id = :id
		     ;
		)QRY")
			.bind(":id", std::string(node))
			.execute()
			;
		auto node_start = double();
		for (auto& r : fetch)
			node_start = r.get<double>(0);
		if (start_time < node_start)
			start_time = node_start;
		entry.start_time = start_time;
		entry.end_time = end_time;
		entry.age = Ev::now() - node_start;
		/* Initialize the statistics.  */
		entry.lockrealtime = 0;
		entry.attempts = 0;
		entry.successes = 0;
		entry.connects = 0;
		entry.connect_checks = 0;
		entry.in_fee = Ln::Amount::sat(0);
		entry.out_fee = Ln::Amount::sat(0);

		return entry;
	}

	/* Called by `clboss-externpay` command.  */
	Ev::Io<void> externpay(Sha256::Hash const& hash) {
		return db.transact().then([hash](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			INSERT OR REPLACE
			  INTO "PeerStatistician_externpays"
			VALUES( :hash
			      , :creation
			      );
			)QRY")
				.bind(":hash", std::string(hash))
				.bind(":creation", Ev::now())
				.execute()
				;
			tx.commit();
			return Ev::lift();
		});
	}

public:
	Impl() =delete;
	explicit
	Impl(S::Bus& bus_) : bus(bus_) { start(); }
};

PeerStatistician::PeerStatistician(PeerStatistician&&) =default;
PeerStatistician::~PeerStatistician() =default;

PeerStatistician::PeerStatistician(S::Bus& bus)
	: pimpl(Util::make_unique<Impl>(bus)) { }

}}
