# CLBOSS Channel Balancing

Rebalance planning lives in `XRebalancer`; execution lives in the
external [`xrebalance`](https://github.com/ksedgwic/xrebalance)
plugin, which reports per-part results back via `xrebalance_part`
notifications.

```mermaid
   %%{init: {"flowchart": {"defaultRenderer": "elk"}} }%%

   flowchart LR

   XrebalancePlugin["xrebalance plugin (external)"]

   style RebalanceModeManager fill:#9fb,stroke:#333,stroke-width:4px
   Manifester-->|Manifestation|RebalanceModeManager
   XRebalancer-->|RequestRebalanceMode|RebalanceModeManager
   RebalanceModeManager-->|ResponseRebalanceMode|XRebalancer

   style DemandTracker fill:#9fb,stroke:#333,stroke-width:4px
   HtlcAcceptor-->|SolicitHtlcAcceptedDeferrer|DemandTracker
   DemandTracker-->|ProvideHtlcAcceptedDeferrer|HtlcAcceptor
   DemandTracker-->|DemandObserved|XRebalancer

   style XRebalancer fill:#9bf,stroke:#333,stroke-width:4px
   Initiator-->|Init|XRebalancer
   Initiator-->|DbResource|XRebalancer
   Manifester-->|Manifestation|XRebalancer
   XRebalancer-->|RequestRebalanceUnmanaged|RebalanceUnmanager
   RebalanceUnmanager-->|ResponseRebalanceUnmanaged|XRebalancer
   XRebalancer-->|"xrebalance (RPC)"|XrebalancePlugin

   style XRebalancePartMonitor fill:#9bf,stroke:#333,stroke-width:4px
   Manifester-->|Manifestation|XRebalancePartMonitor
   XRebalancePartMonitor-->|ManifestNotification|Manifester
   XrebalancePlugin-.->|"xrebalance_part (notification)"|XRebalancePartMonitor
   XRebalancePartMonitor-->|RequestPeerFromScid|PeerFromScidMapper
   PeerFromScidMapper-->|ResponsePeerFromScid|XRebalancePartMonitor
   XRebalancePartMonitor-->|XRebalanceAttribution|EarningsTracker
```
