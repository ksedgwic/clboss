#ifndef BOSS_MOD_PEERTRACKRECORD_HPP
#define BOSS_MOD_PEERTRACKRECORD_HPP

#include"Boss/Msg/TrackRecord.hpp"
#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include"Ln/CommandId.hpp"
#include"Sqlite3.hpp"
#include<cstdint>
#include<functional>
#include<string>
#include<vector>

namespace Boss { namespace Msg { struct Option; }}
namespace Ln { class NodeId; }
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::PeerTrackRecord
 *
 * @brief Judges the earnings track record of nodes we had
 * channels with before, so channel-open candidate selection
 * can prefer proven earners ("keepers") and defer known
 * underperformers.
 *
 * @desc Responds to `Boss::Msg::RequestPeerTrackRecord` with
 * `Boss::Msg::ResponsePeerTrackRecord`.
 *
 * The judgment is TRAL (annualized net return on liquidity,
 * in basis points), the same metric as the
 * `contrib/clboss-forwarding-stats` TRAL column: net earnings
 * from the `EarningsTracker` daily buckets, divided by the
 * average channel balance and the observed operational days
 * from the `FeeMonitor` records, annualized.  Both data
 * sources persist after a channel closes, which is exactly
 * the case this module serves: candidates whose previous
 * channel with us is gone.
 *
 * Operational days count only in-window records (~one per
 * hour while a channel exists), so a channel that overlapped
 * the window partially, or closed mid-window, is annualized
 * over its actual operating time and not diluted by the gap.
 */
class PeerTrackRecord {
private:
	S::Bus& bus;
	std::function<double()> get_now;
	Sqlite3::Db db;

	/* Options; see the manifest descriptions in the .cpp.  */
	std::int64_t window_days;
	std::int64_t keeper_tral_bps;
	std::int64_t min_record_days;

	void start();
	Ev::Io<void> on_option(Msg::Option const& o);
	Ev::Io<void> handle_int_option( Msg::Option const& o
				      , std::int64_t& target
				      , char const* name
				      , std::int64_t minimum
				      );
	Ev::Io<void> on_request( void* requester
			       , std::vector<Ln::NodeId> nodes
			       );
	Ev::Io<void> on_command(Ln::CommandId id, std::string nodeid);
	Ev::Io<Sqlite3::Tx> db_transact();
	bool have_tables(Sqlite3::Tx& tx);
	Msg::TrackRecord compute( Sqlite3::Tx& tx
				, std::string const& node
				, double now
				);

public:
	PeerTrackRecord() =delete;
	PeerTrackRecord(PeerTrackRecord&&) =delete;
	PeerTrackRecord(PeerTrackRecord const&) =delete;

	explicit
	PeerTrackRecord( S::Bus& bus_
		       , std::function<double()> get_now_ = &Ev::now
		       );
};

}}

#endif /* !defined(BOSS_MOD_PEERTRACKRECORD_HPP) */
