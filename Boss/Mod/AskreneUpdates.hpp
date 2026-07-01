#ifndef BOSS_MOD_ASKRENEUPDATES_HPP
#define BOSS_MOD_ASKRENEUPDATES_HPP

#include"Ev/Io.hpp"
#include"Ev/now.hpp"
#include<functional>
#include<memory>
#include<string>

namespace Boss { namespace Mod { class Rpc; } }
namespace Boss { namespace Msg { struct ResponseAskreneUpdates; } }
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::AskreneUpdates
 *
 * @brief owns CLBOSS's learned askrene "updates" -- the node disables
 * and channel_update overrides the rebalancers pick up from routing
 * failures -- as an append-only sqlite log, and projects the still-fresh
 * ones into a private per-request layer on demand.
 *
 * @desc askrene never ages disabled_nodes or local channel_update
 * overrides (unlike the timestamped inform-channel constraints, which
 * askrene-age trims), so a node or channel a rebalancer learned to avoid
 * could never heal in the layer itself.  Rather than wipe a shared layer
 * on a timer -- which races getroutes and can abort the (important)
 * askrene plugin when it names a momentarily-absent layer -- CLBOSS keeps
 * the updates here and rebuilds a fresh, uuid-named, non-persistent layer
 * for each getroutes:
 *
 *   - Records arrive as Boss::Msg::AskreneNodeDisableUpdate /
 *     Boss::Msg::AskreneChannelUpdate and are appended (with a timestamp)
 *     to two tables.  The log is append-only so it can also be mined
 *     later (which nodes churn, which channels re-price).
 *   - Boss::Msg::RequestAskreneUpdates returns, via
 *     Boss::Msg::ResponseAskreneUpdates, the distinct node disables and
 *     the latest override per channel direction that are still within
 *     their projection window (clboss-node-disable-age-secs /
 *     clboss-channel-update-age-secs).  A recovered node/channel simply
 *     stops being projected once its last failure ages past the window.
 *   - Rows are pruned from the log only at the retention horizon
 *     (clboss-update-retain-secs, default ~30d).
 *
 * The static open_layer / close_layer helpers build and tear down the
 * per-request layer from a response; both rebalance engines use them
 * identically, so the projection has no per-engine variation.  A layer is
 * named by exactly one getroutes and removed only after it completes, so
 * -- unlike the old shared wiped layer -- it can never be absent while
 * another getroutes references it.
 */
class AskreneUpdates {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	AskreneUpdates() =delete;
	AskreneUpdates(AskreneUpdates&&);
	~AskreneUpdates();

	explicit
	AskreneUpdates( S::Bus& bus
		      , std::function<double()> get_now = &Ev::now
		      );

	/* Create a fresh, uuid-named, non-persistent askrene layer and
	 * load it with the given updates (disable_node for each node,
	 * update_channel for each channel override).  Returns the layer
	 * name for the caller to include in its getroutes `layers` array,
	 * or an EMPTY string if the layer could not be created (askrene
	 * absent): the caller must then omit it from the array rather than
	 * name a nonexistent layer.  Stateless -- safe to call from either
	 * engine.  */
	static Ev::Io<std::string>
	open_layer( Boss::Mod::Rpc& rpc
		  , Boss::Msg::ResponseAskreneUpdates const& updates
		  );

	/* Remove a layer created by open_layer.  Best-effort: a
	 * remove-layer failure is logged and swallowed (the layer is
	 * non-persistent and private to the finished request).  */
	static Ev::Io<void>
	close_layer( Boss::Mod::Rpc& rpc
		   , std::string const& layer
		   );
};

}}

#endif /* !defined(BOSS_MOD_ASKRENEUPDATES_HPP) */
