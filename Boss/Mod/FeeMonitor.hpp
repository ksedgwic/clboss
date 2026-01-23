#ifndef BOSS_MOD_FEEMONITOR_HPP
#define BOSS_MOD_FEEMONITOR_HPP

#include"Ln/NodeId.hpp"
#include"Sqlite3/Db.hpp"
#include<cstdint>
#include<map>
#include<optional>

namespace Boss { namespace Msg { struct DbResource; }}
namespace Boss { namespace Msg { struct MonitorFeeByBalance; }}
namespace Boss { namespace Msg { struct MonitorFeeByTheory; }}
namespace Boss { namespace Msg { struct MonitorFeeSetChannel; }}
namespace Boss { namespace Msg { struct MonitorFeeBySize; }}
namespace Boss { namespace Msg { struct PeerMedianChannelFee; }}
namespace Ev { template<typename a> class Io; }
namespace S { class Bus; }
namespace Sqlite3 { class Tx; }

namespace Boss { namespace Mod {

/** class Boss::Mod::FeeMonitor
 *
 * @brief Collects fee-setting context and records fee changes
 * into the internal sqlite database.
 */
class FeeMonitor {
private:
	struct PeerInfo {
		std::optional<std::uint32_t> baseline_base;
		std::optional<std::uint32_t> baseline_ppm;
		std::optional<double> size_mult;
		std::optional<std::uint64_t> size_total_peers;
		std::optional<std::uint64_t> size_less_peers;
		std::optional<double> balance_mult;
		std::optional<std::uint64_t> balance_our_msat;
		std::optional<std::uint64_t> balance_total_msat;
		std::optional<std::int64_t> price_level;
		std::optional<double> price_mult;
		std::optional<std::uint32_t> price_cards_left;
	};

	S::Bus& bus;
	Sqlite3::Db db;
	std::map<Ln::NodeId, PeerInfo> peers;

	void start();

	Ev::Io<void> initialize_db();
	Ev::Io<Sqlite3::Tx> db_transact();
	std::uint64_t get_peer_id(Sqlite3::Tx& tx, Ln::NodeId const& node);

	Ev::Io<void> on_db(Boss::Msg::DbResource const& m);
	Ev::Io<void> on_baseline(Boss::Msg::PeerMedianChannelFee const& m);
	Ev::Io<void> on_size(Boss::Msg::MonitorFeeBySize const& m);
	Ev::Io<void> on_balance(Boss::Msg::MonitorFeeByBalance const& m);
	Ev::Io<void> on_price(Boss::Msg::MonitorFeeByTheory const& m);
	Ev::Io<void> on_set(Boss::Msg::MonitorFeeSetChannel const& m);

public:
	FeeMonitor(FeeMonitor const&) =delete;
	FeeMonitor(FeeMonitor&&) =delete;

	explicit
	FeeMonitor(S::Bus& bus_);
	~FeeMonitor();
};

}}

#endif /* !defined(BOSS_MOD_FEEMONITOR_HPP) */
