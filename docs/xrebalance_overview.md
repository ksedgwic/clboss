# CLBOSS xrebalancer Overview

Core Lightning v26.06 deprecates `getroute` and `pay`, with removal
scheduled by v27.03.  The CLBOSS 0.16 rebalancers were built on
`getroute`: `FundsMover` planned every transfer with it, one route
at a time, and the channel probes used it too.  Their successor in
CLN is `askrene`, a min-cost-flow router that plans all sources and
destinations of a transfer together and learns liquidity bounds
from failures.  Rather than port three rebalancers to it, CLBOSS
0.17.0 replaces them with one, `XRebalancer`, that plans circular
rebalances from each peer's earnings record and executes them
through the external
[`xrebalance`](https://github.com/ksedgwic/xrebalance) plugin on
`askrene`.  This document says what changed, how a cycle is
decided, and what the tuning options do.  The options themselves
are listed in the README under `--clboss-xrebalance-*`.

## What was replaced

Removed in 0.17.0: the `JitRebalancer`, `EarningsRebalancer`, and
`InitialRebalancer` modules, the `FundsMover` executor they shared,
the `clboss-movefunds` command, the `clboss-earnings-rebalancer`
debug trigger, and the `clboss-max-rebalance-fee-ppm` option.

## The main ideas

- **A channel's earnings justify its rebalance fee.**  For each
  peer and each side -- inbound, traffic that entered through the
  peer; outbound, traffic that left through it -- `EarningsTracker`
  keeps the volume forwarded, the fees earned, and the fees spent
  on rebalancing over a trailing window
  (`clboss-xrebalance-earnings-window-days`, default 90).  A side's
  net rate is (earnings - expenditures) / forwarded, in ppm.  CLBOSS
  does not pay more to rebalance a channel than that channel earns,
  net of what earlier rebalances cost: a transfer's fee ceiling is
  the sum of the net rates of the two sides it serves.
- **Two kinds of cycle.**  Matched cycles run on a clock and pair
  the peers that most need filling with those that most need
  draining, sized and priced from the record; they take the place
  of the earnings rebalancer.  Demand cycles run when a forward
  drains a channel that is already low and refill that peer alone;
  they take the place of JIT rebalancing, without holding the
  forward's HTLC.
- **Batched circular execution.**  A cycle is one request to the
  `xrebalance` plugin: source channels, destination channels, an
  amount, and the fee ceiling.  The plugin solves all sources and
  destinations together in one `askrene` min-cost-flow plan, sends
  the parts as independent circular self-payments, and replans
  what did not move using what the failures showed, for up to
  `xrebalance-max-rounds` rounds (default 50).  The lowest-level
  loop -- plan, send, learn, replan -- lives in the plugin; CLBOSS
  decides what to move and at what price, and waits for the
  request to finish.

## How a matched cycle is decided

1. **Bands.**  A channel below 25% local liquidity
   (`clboss-xrebalance-fill-loc`) is a fill candidate; above 75%
   (`clboss-xrebalance-drain-loc`), a drain candidate.  A transfer
   moves a channel only to its band edge, not further, so channels
   between 25% and 75% take no part in rebalancing, as source or as
   destination.  Liquidity is measured per peer, summing its
   channels.  Offline peers and peers marked `balance` by
   `clboss-unmanage` are excluded.
2. **Admission.**  A fill candidate needs a positive outbound net
   rate; a drain candidate needs a positive inbound net rate.  Each
   pool is sorted by that rate, best first.
3. **Matching.**  The top fills are combined with the top drains,
   and the fee ceiling is set by the weakest members of the set:
   the lowest outbound rate among the fills plus the lowest inbound
   rate among the drains.  A focused set -- the best fill with the
   best drain -- justifies a high ceiling but moves little; a wide
   set moves more, but its ceiling falls to what its weakest
   members earn.  How wide the set grows is set by
   `clboss-xrebalance-route-cost-floor`; the default, `auto`,
   varies it from cycle to cycle.
4. **The request.**  The picked peers' channels go into the
   `xrebalance` request as sources (drain side) and destinations
   (fill side), each with a cap equal to its share of the peer's
   deficit, with the volume and the fee ceiling.

Each cycle logs its choice (`XRebalancer: cycle [matched] ...`)
and its outcome (`XRebalancer: transfer done|failed ...`).
`contrib/clboss-xrebalance-view` prints the same view from the
live node -- bands, pools, both rate columns, the floor levels, and
the request the widest cycle would send -- without running a
cycle.

## When cycles run

- **Matched cycles** run on a Poisson clock at
  `clboss-xrebalance-per-hour` cycles per hour on average (default
  24).  `0` stops the clock; demand cycles still run.
- **Demand cycles** are a side effect of routing forwards.  When a
  forward leaves through a channel whose peer is a fill candidate,
  a cycle runs to fill that peer alone, from several drain sources
  at once, priced by the same rule as a matched cycle.  Demand
  decides when to rebalance; the bands and the record still decide
  who qualifies, how much, and the price.

One cycle runs at a time.  A clock tick or a demand trigger that
arrives while a cycle is in flight is skipped; the clock ticks
again, and recurring traffic re-triggers demand.

## Execution

The `xrebalance` plugin carries out a request: it solves all
sources and destinations in one min-cost-flow plan, splits the
volume into up to `clboss-xrebalance-maxparts` parts (default 80),
and sends each part as an independent self-payment with its own
preimage, so parts settle independently.  Up to
`xrebalance-max-rounds` rounds replan whatever has not moved,
using what the failed parts showed: liquidity bounds go into a
persistent `askrene` layer (aged out after
`xrebalance-constraint-age`), and stale fees, gone channels,
inbound-fee peers, and failing nodes are excluded for
`xrebalance-override-age`.  The fee ceiling is enforced at the
askrene quote and again on each route.  Partial delivery is
normal: every settled part is liquidity moved, and a request that
delivers nothing is a result, not an error.

The RPC returns when the rounds end.  The response carries the
totals and a summary -- parts sent and completed, the amount still
settling, and the failed part that came closest -- which is what
the `transfer` log line reports.  Parts that are still in flight
when the request returns settle in the background.

## Bookkeeping

The plugin broadcasts an `xrebalance_part` notification for every
part that resolves; `XRebalancePartMonitor` maps the part's source
and destination channels to peers and books the moved amount and
fee into `EarningsTracker`: a part that drains channel A into
channel B is an expenditure on A's inbound side (the drain made
inbound room on A) and on B's outbound side (the fill made outbound
room on B).  The record that prices the next cycle therefore
includes what this cycle cost.  The notifications carry no sender,
so a rebalance issued by hand through the `xrebalance` RPC is
booked the same way, as a rebalance expense in the CLBOSS database.
`clboss-recent-earnings` and `clboss-earnings-history` show the
numbers.

## Operating and tuning

All `clboss-xrebalance-*` options are dynamic (`lightning-cli
setconfig`); `clboss-rebalance-mode` selects `xrebalance` (default)
or `off`.  What each knob moves:

| option | default | what it changes |
| --- | --- | --- |
| `per-hour` | 24 | how often matched cycles run; demand cycles are separate |
| `fill-loc` / `drain-loc` | 25 / 75 | which peers qualify, and how far a transfer moves them |
| `earnings-window-days` | 90 | how much history the rates are measured over |
| `route-cost-floor` | auto | how wide a matched set may grow: a fixed ppm floor on the fee ceiling, or `auto`, a ladder of floors derived from the node's own rates with a random rung picked each cycle, so focused cycles at high ceilings and wide cycles at low ones alternate |
| `maxparts` | 80 | how finely a transfer may split; larger means more paths and more refusals per solve |
| `grant` | 0 | an assumed prior rate credited to every peer, see below |
| `gain` | 1 | a multiplier on the measured rates, see below |

**grant and gain** bend the strict rule that a side must have
earned what a transfer costs.

- `grant` credits every peer, on both sides, an assumed rate of
  `grant` ppm as if it had already been earned on one rebalance's
  worth of volume: on the out side `fill-loc` percent of the peer's
  capacity, one fill; on the in side `100 - drain-loc` percent, one
  drain:

      w = capacity * fill-loc / 100            (out side)
      w = capacity * (100 - drain-loc) / 100   (in side)
      adjusted rate = (net + w * grant / 1e6) / (forwarded + w)

  A peer with no record reads exactly `grant`, and the credit is
  what one move to the band edge costs at `grant` ppm.  The credit
  weighs one rebalance's worth of volume, so it matters only while
  a peer's forwarded volume is small next to that; as the record
  grows, the adjusted rate converges on the measured net rate, and
  expenditures spend the credit down.  It admits new peers and
  peers with thin records, and lifts a slightly negative record to
  a small positive one.  This is what
  takes the place of the `InitialRebalancer`: an optimistic credit
  that lets a new channel be filled before it has earned anything,
  and that its own record then confirms or spends down.
- `gain` multiplies the result.  Above 1 it admits routes costing
  up to `gain` times what the record shows; below 1 it tightens
  the rule, admitting only routes cheaper than the record shows.
  Any value above 0 is accepted.

At `grant` 0 and `gain` 1 the adjusted rate equals the raw net
rate.  `clboss-xrebalance-view` shows both, `InNetPpm` / `OutNetPpm`
raw and `InAdjPpm` / `OutAdjPpm` adjusted, so the effect of the two
settings is visible per peer.  A node with no record cannot
rebalance under the strict rule at all, since no side has earned
anything; `grant` is what admits it, and the credit dilutes on its
own as forwarded volume grows past one rebalance's worth of the
channel.  One production node runs `grant` 100 and `gain` 1.2 on a
mature record.

What to watch:

- `clboss-recent-earnings` -- fees spent on rebalancing
  (`in_expenditures` / `out_expenditures`) against fees earned,
  per peer and in total.
- `XRebalancer: transfer ...` lines -- parts, rounds, delivered,
  fee and ppm, the pending amount, and the closest failure.
- `clboss-status`, key `xrebalancer` -- whether the plugin answers.
- `contrib/clboss-xrebalance-view` -- the next cycle before it runs;
  `--grant` / `--gain` / `--route-cost-floor` preview a setting
  before `setconfig`.

`clboss-xrebalance-maxparts` sizes each `getroutes` call; raise
`lightningd`'s `askrene-timeout` with it (30 seconds has been
enough at the default).

## Requirements and upgrading

- Core Lightning v25.09 or later is refused below; v26.04 or later
  is the tested floor.
- The `xrebalance` plugin, v0.4.5 or later, loaded alongside
  CLBOSS.  Without it CLBOSS runs everything else and logs a
  warning once an hour; `clboss-status` shows the plugin state.
- Remove `clboss-max-rebalance-fee-ppm` from the configuration;
  `lightningd` refuses to start on an unknown option.
- Nothing else needs setting.

The CHANGELOG's "Upgrading from 0.16.x" note covers the
development-build layer cleanup.
