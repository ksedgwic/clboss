#ifndef BOSS_MOD_ASKRENEVOLATILELAYER_HPP
#define BOSS_MOD_ASKRENEVOLATILELAYER_HPP

#include<memory>

namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::AskreneVolatileLayer
 *
 * @brief owns the lifecycle of the shared, non-persistent
 * `clboss-volatile` askrene layer: creates it at startup and wipes it
 * (remove + recreate) on a periodic timer.
 *
 * @desc the volatile layer holds the "blocks" both rebalancers learn --
 * node disables and channel_update overrides.  askrene-age never removes
 * those (they carry no aging timestamp), so without this they would
 * accumulate forever and a node/channel could never heal.  Wiping the
 * whole layer on a cadence lets the blocks re-accumulate from fresh
 * failures, so a recovered node becomes routable again within one wipe
 * interval.  Cadence is clboss-volatile-layer-wipe-secs (dynamic; default
 * 3h).  The layer is non-persistent, so a CLN restart is itself a free
 * wipe.
 */
class AskreneVolatileLayer {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	AskreneVolatileLayer() =delete;

	AskreneVolatileLayer(AskreneVolatileLayer&&);
	~AskreneVolatileLayer();

	explicit
	AskreneVolatileLayer(S::Bus&);
};

}}

#endif /* !defined(BOSS_MOD_ASKRENEVOLATILELAYER_HPP) */
