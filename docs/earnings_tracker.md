# CLBOSS Earnings Tracker

```mermaid
   %%{init: {"flowchart": {"defaultRenderer": "elk"}} }%%

   flowchart TB

   style EarningsTracker fill:#9fb,stroke:#333,stroke-width:4px
   Initiator-->|DbResource|EarningsTracker
   ForwardFeeMonitor-->|ForwardFee|EarningsTracker
   XRebalancePartMonitor-->|XRebalanceAttribution|EarningsTracker
   StatusCommand-->|SolicitStatus|EarningsTracker
   EarningsTracker-->|ProvideStatus|StatusCommand
```
