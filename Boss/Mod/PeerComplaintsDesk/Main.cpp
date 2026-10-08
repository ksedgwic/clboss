#include"Boss/Mod/PeerComplaintsDesk/Exempter.hpp"
#include"Boss/Mod/PeerComplaintsDesk/Main.hpp"
#include"Boss/Mod/PeerComplaintsDesk/Recorder.hpp"
#include"Boss/Mod/PeerComplaintsDesk/Unmanager.hpp"
#include"Boss/Mod/Rpc.hpp"
#include"Boss/ModG/ReqResp.hpp"
#include"Boss/Msg/ChannelDestruction.hpp"
#include"Boss/Msg/DbResource.hpp"
#include"Boss/Msg/Init.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/OnchainFee.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/RaisePeerComplaint.hpp"
#include"Boss/Msg/RequestPeerMetrics.hpp"
#include"Boss/Msg/ResponsePeerMetrics.hpp"
#include"Boss/Msg/SolicitPeerComplaints.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/Msg/Timer10Minutes.hpp"
#include"Boss/Msg/TimerTwiceDaily.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/foreach.hpp"
#include"Ev/now.hpp"
#include"Jsmn/Object.hpp"
#include"Json/Out.hpp"
#include"S/Bus.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include"Util/stringify.hpp"
#include<algorithm>
#include<assert.h>
#include<memory>
#include<string>
#include<vector>

namespace {

/* Whether this feature is enabled by default.  */
auto constexpr default_enabled = bool(false);

/* Maximum age, in seconds, to retain complaints.  */
auto constexpr max_complaint_age = double(7 * 24 * 60 * 60);
/* Maximum age, in seconds, to retain complaints for closed channels.  */
auto constexpr max_closed_complaint_age = double(90 * 24 * 60 * 60);

/* Number of non-ignored complaints before we decide to close.  */
auto constexpr max_acceptable_complaints = std::size_t(5);

/* Number of seconds we will request for unilateral closing timeout.  */
auto constexpr channel_close_timeout = std::size_t(3 * 60);
/* How strongly do we insist on our own feerate.  */
auto const fee_negotiation_step = std::string("1");

/* How long we wait for an offline peer to come back for a
 * mutual close before giving up and closing anyway, letting
 * the unilateral timeout escalate.
 */
auto constexpr close_patience = double(3 * 24 * 60 * 60);

}

namespace Boss { namespace Mod { namespace PeerComplaintsDesk {

class Main::Impl {
private:
	Unmanager unmanager;
	Exempter exempter;

	S::Bus& bus;

	bool enabled;

	Sqlite3::Db db;

	Boss::Mod::Rpc* rpc;

	bool soliciting;
	bool fees_low;

	std::map<Ln::NodeId, std::string> exempted_nodes;
	std::map<Ln::NodeId, std::string> pending_complaints;

	std::unique_ptr<Msg::SolicitPeerComplaints> solicit_message;

	ModG::ReqResp< Msg::RequestPeerMetrics
		     , Msg::ResponsePeerMetrics
		     > metrics_rr;

	void start() {
		enabled = default_enabled;
		rpc = nullptr;
		soliciting = false;
		fees_low = false;
		exempted_nodes.clear();
		pending_complaints.clear();

		/* Check if we are enabled.  */
		bus.subscribe<Msg::Manifestation
			     >([this](Msg::Manifestation const& _) {
			return bus.raise(Msg::ManifestOption{
				"clboss-auto-close", Msg::OptionType_Bool,
				Json::Out::direct(default_enabled),
				"Whether CLBOSS should automatically close "
				"bad channels (EXPERIMENTAL).  "
				"Set 'true' to enable, 'false' to disable."
			});
		});
		bus.subscribe<Msg::Option
			     >([this](Msg::Option const& o) {
			if (o.name != "clboss-auto-close")
				return Ev::lift();

			enabled = bool(o.value);
			if (enabled != default_enabled)
				return Boss::log( bus, Info
						, "PeerComplaintsDesk: "
						  "Auto-close: %s."
						, enabled ? "enabled" :
							    "disabled"
						);
			return Ev::lift();
		});

		bus.subscribe<Msg::DbResource
			     >([this](Msg::DbResource const& m) {
			db = m.db;
			return db.transact().then([](Sqlite3::Tx tx) {
				Recorder::initialize(tx);
				tx.commit();

				return Ev::lift();
			});
		});
		bus.subscribe<Msg::Init
			     >([this](Msg::Init const& m) {
			rpc = &m.rpc;
			return Ev::lift();
		});

		bus.subscribe<Msg::OnchainFee
			     >([this](Msg::OnchainFee const& m) {
			fees_low = m.fees_low;
			return Ev::lift();
		});

		bus.subscribe<Msg::TimerTwiceDaily
			     >([this](Msg::TimerTwiceDaily const& _) {
			return Boss::concurrent(solicit())
			     + Boss::concurrent(cleanup())
			     ;
		});
		/* Poll close candidates every 10 minutes: a flaky
		 * peer's brief online windows should actually get
		 * caught for a mutual close.
		 */
		bus.subscribe<Msg::Timer10Minutes
			     >([this](Msg::Timer10Minutes const& _) {
			if (!rpc)
				return Ev::lift();
			return Boss::concurrent(check_close());
		});

		bus.subscribe<Msg::RaisePeerComplaint
			     >([this](Msg::RaisePeerComplaint const& m) {
			if (!soliciting)
				return Ev::lift();

			auto it = pending_complaints.find(m.peer);
			if (it == pending_complaints.end())
				pending_complaints[m.peer] = m.complaint;
			else
				/* Multiple complaints?
				 * Merge into single complaint.
				 */
				it->second += std::string("; ") + m.complaint;
			return Ev::lift();
		});

		bus.subscribe<Msg::ChannelDestruction
			     >([this](Msg::ChannelDestruction const& m) {
			return Boss::concurrent(on_channel_destroy(m.peer));
		});

		bus.subscribe<Msg::SolicitStatus
			     >([this](Msg::SolicitStatus const& _) {
			return provide_status();
		});
	}

	Ev::Io<void> solicit() {
		if (soliciting)
			return Ev::lift();

		soliciting = true;
		pending_complaints.clear();

		return Ev::lift().then([this]() {

			return exempter.get_exemptions();
		}).then([this](std::map<Ln::NodeId, std::string> new_exempted_nodes) {
			exempted_nodes = std::move(new_exempted_nodes);

			return metrics_rr.execute(Msg::RequestPeerMetrics{});
		}).then([this](Msg::ResponsePeerMetrics metrics) {
			solicit_message = Util::make_unique<Msg::SolicitPeerComplaints>();
			solicit_message->day3 = metrics.day3;
			solicit_message->weeks2 = metrics.weeks2;
			solicit_message->month1 = metrics.month1;

			return Boss::log( bus, Debug
					, "PeerComplaintsDesk: Soliciting complaints."
					);
		}).then([this]() {
			auto tmp_solicit_message = std::move(solicit_message);
			solicit_message = nullptr;
			return bus.raise(std::move(*tmp_solicit_message));
		}).then([this]() {
			return db.transact();
		}).then([this](Sqlite3::Tx tx) {
			auto act = Ev::lift();

			for (auto const& c : pending_complaints) {
				auto const& peer = c.first;
				auto const& complaint = c.second;

				auto ex_it = exempted_nodes.find(peer);
				if (ex_it == exempted_nodes.end()) {
					/* Peer is not exempted, record complaint.  */
					Recorder::add_complaint( tx
							       , peer, complaint
							       );
					act += Boss::log( bus, Debug
							, "PeerComplaintsDesk: "
							  "%s: %s"
							, Util::stringify(peer).c_str()
							, complaint.c_str()
							);
				} else {
					/* Peer is exempted, record complaint as ignored.  */
					Recorder::add_ignored_complaint( tx
								       , peer, complaint
								       , ex_it->second
								       );
					act += Boss::log( bus, Debug
							, "PeerComplaintsDesk: "
							  "%s: %s "
							  "[IGNORED: %s]"
							, Util::stringify(peer).c_str()
							, complaint.c_str()
							, ex_it->second.c_str()
							);
				}
			}
			tx.commit();

			/* Finished soliciting.  */
			soliciting = false;
			/* Do not waste memory.  */
			exempted_nodes.clear();
			pending_complaints.clear();

			/* Return logs.  */
			return act;
		});
	}
	Ev::Io<void> cleanup() {
		return db.transact().then([](Sqlite3::Tx tx) {
			Recorder::cleanup(tx, max_complaint_age, max_closed_complaint_age);
			tx.commit();

			return Ev::lift();
		});
	}
	Ev::Io<void> check_close() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto complaints = Recorder::check_complaints(tx);

			auto const& unmanaged = unmanager.get_unmanaged();

			auto act = Ev::lift();
			act += Boss::log( bus, Debug
					, "PeerComplaintsDesk: "
					  "Checking for channels to close.%s"
					, enabled ? "" :
						    "  (Auto-close is "
						    "disabled, this is "
						    "just practice)."
					);

			auto msg = std::string();
			auto first = true;
			auto to_close = std::vector<Ln::NodeId>();
			for (auto& c : complaints) {
				if (first)
					first = false;
				else
					msg += ", ";
				msg += Util::stringify(c.first)
				     + " ("
				     + Util::stringify(c.second)
				     ;
				if (unmanaged.count(c.first) != 0) {
					msg += "; !UNMANAGED!)";
					continue;
				}
				msg +=
				     + ")"
				     ;
				if (c.second >= max_acceptable_complaints)
					to_close.push_back(c.first);
			}
			/* Forget deferred closes for peers no longer
			 * over the threshold (recovered, unmanaged,
			 * or channel gone).
			 */
			for (auto& kv : Recorder::get_close_pendings(tx)) {
				if (std::find( to_close.begin()
					     , to_close.end()
					     , kv.first
					     ) == to_close.end())
					Recorder::clear_close_pending( tx
								     , kv.first
								     );
			}
			tx.commit();

			if (!first)
				act += Boss::log( bus, Debug
						, "PeerComplaintsDesk: Complaints: %s"
						, msg.c_str()
						);

			msg = std::string();
			first = true;
			for (auto& p : to_close) {
				if (first)
					first = false;
				else
					msg += ", ";
				msg += Util::stringify(p);

				if (!enabled)
					continue;

				act += Boss::concurrent(close_channel(p));
			}
			if (!first)
				act += Boss::log( bus
						, enabled ? Info : Debug
						, "PeerComplaintsDesk: %s: %s"
						, enabled ? "Closing" :
							    "Would have "
							    "closed"
						, msg.c_str()
						);
			return act;
		});
	}
	Ev::Io<void> close_channel(Ln::NodeId const& p) {
		if (!enabled)
			return Ev::lift();

		return Ev::lift().then([this, p]() {
			auto parms = Json::Out()
				.start_object()
					.field("id", Util::stringify(p))
				.end_object()
				;
			return rpc->command("listpeerchannels", std::move(parms));
		}).then([this, p](Jsmn::Object res) {
			auto connected = false;
			auto ids = std::vector<std::string>();
			if (res.has("channels")) {
				auto cs = res["channels"];
				for (auto c : cs) {
					if (c.has("channel_id")
					 && c["channel_id"].is_string()
					 && c.has("state")
					 && c["state"].is_string()
					 && closable_state(std::string(c["state"])))
						ids.push_back(std::string(
							c["channel_id"]
						));
					if (!c.has("peer_connected"))
						continue;
					auto conn = c["peer_connected"];
					if (!conn.is_boolean())
						continue;
					if (bool(conn))
						connected = true;
				}
			}
			if (connected) {
				/* Take the mutual close as soon as the
				 * peer is reachable, regardless of the
				 * feerate: a mutual close even at high
				 * fees is cheaper than a unilateral at
				 * low fees, and online windows for these
				 * peers are rare.
				 */
				return db.transact(
				).then([p](Sqlite3::Tx tx) {
					Recorder::clear_close_pending(tx, p);
					tx.commit();
					return Ev::lift();
				}).then([this, p, ids]() {
					return do_close(p, ids);
				});
			}
			/* Offline: wait for a mutual-close window
			 * until patience runs out.  The deferral is
			 * persisted, so a restart does not reset
			 * the patience window.
			 */
			return db.transact(
			).then([p](Sqlite3::Tx tx) {
				Recorder::note_close_pending(tx, p, Ev::now());
				auto since = Recorder::get_close_pending(tx, p);
				tx.commit();
				assert(since);
				return Ev::lift(*since);
			}).then([this, p, ids](double since) {
				if (Ev::now() - since < close_patience)
					return Boss::log( bus, Debug
							, "PeerComplaintsDesk: %s is "
							  "not connected, deferring "
							  "close while waiting for a "
							  "mutual-close window."
							, Util::stringify(p).c_str()
							);
				/* Patience expired: get it over
				 * with, but only when fees are low —
				 * the unilateral path is the
				 * expensive one.
				 */
				if (!fees_low)
					return Boss::log( bus, Debug
							, "PeerComplaintsDesk: %s close "
							  "patience expired, holding "
							  "unilateral close until fees "
							  "are low."
							, Util::stringify(p).c_str()
							);
				return Boss::log( bus, Info
						, "PeerComplaintsDesk: %s never "
						  "came back online, closing "
						  "anyway."
						, Util::stringify(p).c_str()
						).then([this, p, ids]() {
					return do_close(p, ids);
				});
			});
		}).catching<RpcError>([this, p](RpcError e) {
			return Boss::log( bus, Error
					, "PeerComplaintsDesk: close %s error: %s"
					, Util::stringify(p).c_str()
					, Util::stringify(e.error).c_str()
					);
		});
	}
	/* States a `close` can be issued in.  A channel already
	 * shutting down or closing is left alone.  */
	static bool closable_state(std::string const& state) {
		return state == "CHANNELD_NORMAL"
		    || state == "CHANNELD_AWAITING_LOCKIN"
		    || state == "CHANNELD_AWAITING_SPLICE"
		    || state == "DUALOPEND_AWAITING_LOCKIN"
		     ;
	}
	Ev::Io<void> close_one(std::string const& id) {
		auto parms = Json::Out()
			.start_object()
				.field("id", id)
				.field("unilateraltimeout", channel_close_timeout)
				.field("fee_negotiation_step", fee_negotiation_step)
			.end_object()
			;
		return rpc->command("close", std::move(parms))
			.then([](Jsmn::Object _) {
			return Ev::lift();
		});
	}
	/* Complaints are per peer, so the whole peer is closed.
	 * `close id=<node>` is refused by lightningd when the peer
	 * has more than one channel ("Peer has multiple channels"),
	 * and the close would then be retried every cycle forever
	 * (#352), so with several channels close each by its
	 * channel id.  A failure on one channel is logged and does
	 * not stop the others.  */
	Ev::Io<void> do_close( Ln::NodeId const& p
			     , std::vector<std::string> const& ids
			     ) {
		if (ids.size() < 2)
			return close_one(Util::stringify(p));
		auto f = [this, p](std::string id) {
			return close_one(id).catching<RpcError>([ this
								, p
								, id
								](RpcError e) {
				return Boss::log( bus, Error
						, "PeerComplaintsDesk: close "
						  "%s channel %s error: %s"
						, Util::stringify(p).c_str()
						, id.c_str()
						, Util::stringify(e.error).c_str()
						);
			});
		};
		return Ev::foreach(std::move(f), ids);
	}
	Ev::Io<void> on_channel_destroy(Ln::NodeId const& p) {
		return db.transact().then([p](Sqlite3::Tx tx) {
			Recorder::clear_close_pending(tx, p);
			Recorder::channel_closed(tx, p);
			tx.commit();
			return Ev::lift();
		});
	}

	Ev::Io<void> provide_status() {
		return db.transact().then([this](Sqlite3::Tx tx) {
			auto complaints = Recorder::get_all_complaints(tx);
			auto closed_complaints = Recorder::get_closed_complaints(tx);
			tx.commit();

			auto peer_complaints_j = jsonify(complaints);
			auto closed_peer_complaints_j = jsonify(closed_complaints);

			return bus.raise(Msg::ProvideStatus{
					"peer_complaints",
					std::move(peer_complaints_j)
			       })
			     + bus.raise(Msg::ProvideStatus{
					"closed_peer_complaints",
					std::move(closed_peer_complaints_j)
			       })
			     ;
		});
	}
	Json::Out jsonify(std::map< Ln::NodeId
				  , std::vector<std::string>
				  > const& map) {
		auto rv = Json::Out();
		auto obj = rv.start_object();
		for (auto const& ps : map) {
			auto arr = obj.start_array(Util::stringify(ps.first));
			for (auto const& s : ps.second)
				arr.entry(s);
			arr.end_array();
		}
		obj.end_object();
		return rv;
	}

public:
	Impl() =delete;
	Impl(Impl const&) =delete;
	Impl(Impl&&) =delete;

	explicit
	Impl(S::Bus& bus_) : unmanager(bus_)
			   , exempter(bus_, unmanager)
			   , bus(bus_)
			   , metrics_rr(bus_)
			   { start(); }
};

Main::Main(Main&&) =default;
Main::~Main() =default;

Main::Main(S::Bus& bus) : pimpl(Util::make_unique<Impl>(bus)) { }

}}}
