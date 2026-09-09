# What's new in CLBOSS v0.17.0

2026-09-08.  Applies to CLBOSS v0.17.0 with the `xrebalance` plugin
v0.4.5 or later, on Core Lightning v26.04 or later.

CLBOSS v0.17.0 replaces the built-in rebalancers, ranks channel-open
candidates by their earnings record, and makes most options settable
at runtime.  It needs a newer Core Lightning than 0.16.x did.

## Rebalancing replaced

The `JitRebalancer`, `EarningsRebalancer`, and `InitialRebalancer`
planned every transfer with `getroute`, which Core Lightning has
deprecated.  All three are removed, along with the `FundsMover`
executor they shared, the `clboss-movefunds` command, and the
`clboss-max-rebalance-fee-ppm` option.

## The new rebalancer

The new node rebalancing system consists of two parts: the
high-level "how much to move, and where" decisions are made by
CLBOSS, while the low-level work of finding routes and moving the
sats is handled by the new
[`xrebalance`](https://github.com/ksedgwic/xrebalance) plugin on
Core Lightning's `askrene` min-cost-flow router.  CLBOSS prices
each transfer from what the channels it serves have earned, net of
earlier rebalance costs, so rebalancing does not spend more than a
channel's record justifies.

There are two kinds of high-level rebalancing cycle.  "Matched set"
cycles run periodically and combine the peers most in need of
filling with those most in need of draining into one batched
transfer.  "Demand" cycles are triggered every time a forwarded
payment goes out through a channel that is already in need of
filling, and refill that peer alone.

## Channel candidates ranked by track record

Few decisions carry as far as the choice of which nodes to open
channels to: a channel stays open for months, earning whatever
traffic its peer brings.  CLBOSS now weighs what earlier channels to
the same node earned, and whether the node supports splicing, when
it chooses.  Nodes whose earlier channels earned well are funded
first, nodes with no record next, and nodes that underperformed
last; among the unknowns, peers that support splicing are preferred,
since their channels can be resized in place.  `clboss-track-record
<nodeid>` shows how CLBOSS rates any node.

## Options settable at runtime

Many options are now dynamic: `lightning-cli setconfig` changes
them without a restart, and an invalid value is rejected with an
error instead of being silently ignored.  Each option's README
entry says whether it is dynamic.  `clboss-rebalance-mode` and all
the `clboss-xrebalance-*` options are.

## Requirements and upgrading from 0.16.x

- Core Lightning v26.04 or later.  v25.09 is the hard floor: below
  it CLBOSS refuses to start, and from v25.09 up to v26.04 it starts
  with a warning.  Operators on an older Core Lightning should stay
  on CLBOSS 0.16.x.
- Build the `xrebalance` plugin, v0.4.5 or later, and load it
  alongside CLBOSS; "The xrebalance plugin" under Installing in the
  README has the steps.  Without it CLBOSS runs everything except
  rebalancing and warns once an hour.
- Remove `clboss-max-rebalance-fee-ppm` from the configuration;
  `lightningd` refuses to start on an unknown option.

Nothing else needs setting.  The [CHANGELOG](../../CHANGELOG.md)
has the full list of changes and fixes.
