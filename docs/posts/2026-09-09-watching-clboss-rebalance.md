# Watching CLBOSS rebalance

2026-09-09.  Applies to CLBOSS v0.17.0 with the `xrebalance` plugin
v0.4.5 or later.

Here are some log greps you can run to watch the rebalancer in
action, with notes on what the lines say, and a few commands that
show the same picture without the log.

`LOG` below is your `lightningd` log file, the one named by
`log-file` in its configuration.  Without a `log-file` setting,
`lightningd` logs to stderr, and `journalctl -fu <service>` takes the
place of `tail -F $LOG`.

## Show the high-level decisions and cycles

    tail -F $LOG | grep --line-buffered -F -e "XRebalancer: cycle [" -e "XRebalancer: transfer"

Each cycle logs what it decided and, a few seconds to a few minutes
later, how it went.

### Matched cycles

    XRebalancer: floor levels [4 rungs]: 692/502/276/173 ppm
    XRebalancer: cycle [matched] floor=502.4 (auto picked 502) window=90d -> request=3_874_362_000 msat (matched volume), joint=502.4 ppm (fill>=382.4 + drain>=120.0), maxfee 502 ppm, grant 100, gain 1.2; sources=7 dests=2; executing.

The floor ladder comes from the node's own earnings rates; `auto`
picks one rung per cycle.  `joint` is the fee ceiling: the lowest
outbound rate among the fill peers plus the lowest inbound rate
among the drain peers that made the set.  `request` is the volume
the set can absorb.  `grant` and `gain` are the current settings.

### Demand cycles

A demand cycle is triggered by a forward through a channel that is
already below its fill band:

    XRebalancer: cycle [demand] trigger=963230x2418x0 target=963230x2418x0:1199960 window=90d -> request=1_199_960_000 msat (deficit to fill edge), rung=top 20% -> maxfee=222 ppm (target 101.9 + min offered 120.0), grant 100, gain 1.2; sources=7 dests=1; executing.

`target` names the channel and its deficit in sat.  `rung` is the
share of the drain pool used as sources; the ceiling is the
target's own rate plus the lowest rate among those sources.

### Transfer results

Both kinds of cycle end with a transfer line.  When something moved:

    XRebalancer: transfer done [req 0e4873ae]: 3/24 parts in 7 round(s) over 201s (0.12 parts/s), delivered 5_855_015 msat, fee 2_195 msat (375 ppm) (9 pending, 421_432_590 msat, settling in background); stop: no further routes: getroutes: Error code 206: Could not find route without excessive cost.

Parts completed over parts sent, rounds, elapsed time, what was
delivered and at what fee, parts still in flight when the request
returned (they settle on their own and are booked when they do),
and why the plugin stopped.  When nothing moved:

    XRebalancer: transfer failed [req f9c66c20]: 0 part(s) in 1 round(s) over 1s; stop: no further routes: no usable route from the sources to the destinations at this amount and budget, nor at smaller amounts (getroutes 205).

Most cycles end this way, and that is normal: the fee ceiling is
strict, so most of the time no route fits under it.  A failed
cycle costs nothing, and the next tick picks another rung.  The two
stop reasons that fill a log are `getroutes 205`, no route at all
within the budget, and `Error code 206`, routes exist but cost more
than the budget.

Between cycles you may also see `no viable cycle -- no matched
volume clears floor ...` (the rung picked was too high for the
current set), `no cycle -- NO_CANDIDATES (fill=0 drain=...)` (no
channel is outside its bands), and `xrebalance plugin not loaded`
at UNUSUAL level, which means the plugin is missing and rebalancing
is paused until it is back.

## Show the blow-by-blow progress of each cycle

The plugin logs at DEBUG.  To see its lines without turning on
DEBUG for everything, add to the `lightningd` configuration and
restart:

    log-level=debug:plugin-xrebalance

Then:

    tail -F $LOG | grep --line-buffered plugin-xrebalance

Each request logs the plan, one line per completed part, one line
per round, and the finish; these are from the request whose
`transfer done` line appears above:

    req 0e4873ae: move up to 4_154_574_000msat, 6 sources -> 2 destinations, budget 2_085_596msat (501ppm)
    req 0e4873ae: part  3/ 4 complete: delivered     1_917_329 msat fee       719 msat (   375 ppm)
    req 0e4873ae: round 5/50: delivered 1_917_329 msat this round (5_855_015 total), 421_432_590 msat pending, 3_727_286_395 msat remaining
    req 0e4873ae: finished after 7 round(s): no further routes: getroutes: Error code 206: Could not find route without excessive cost

## Show the successful transfer parts

    tail -F $LOG | grep --line-buffered "complete: delivered"

One line per part that landed, with its amount, fee, and rate:

    req 0e4873ae: part  3/ 4 complete: delivered     1_917_329 msat fee       719 msat (   375 ppm)

This needs the same `log-level` setting as the previous section.
The `transfer done` line sums these per request; this grep shows
them as they happen, which is the quickest way to see that
rebalancing is working.

## Attribution

`log-level=debug:plugin-clboss` adds CLBOSS's own detail: under each
cycle line, the `sources=[...] dests=[...]` line with the channels
and their caps, and for every completed part the booking:

    XRebalancePartMonitor: 026f4620... -> 02e4971e..., moved 1917329msat, fee 719msat.

That line is the rebalance cost entering the earnings record of the
two peers, where it lowers the ceiling of the next cycle that
involves them.  A part booked to `unknown` on either side is worth
reporting.

## Without the log

- `lightning-cli clboss-status | jq .xrebalancer`: whether the plugin
  answers (`present`, `absent`, or `unknown` before the first
  cycle), the time of its last answer, consecutive failures, and
  the last error.
- `contrib/clboss-xrebalance-view`: the next cycle before it runs.
  Each channel's band and its peer's rates, raw and with grant and
  gain applied, the fill and drain pools, the floor ladder, and the
  request the widest cycle would send.  `--grant`, `--gain`, and
  `--route-cost-floor` preview a setting before `setconfig`.
- `lightning-cli clboss-recent-earnings 7`: per peer, fees earned
  against fees spent on rebalancing (`in_expenditures`,
  `out_expenditures`) over the last seven days.
- `lightning-cli xrebalance-stats`: the plugin's version, settings,
  and what its `askrene` layer has learned.

## When reporting a problem

    grep -E 'UNUSUAL|BROKEN' $LOG | grep -E 'plugin-clboss|plugin-xrebalance'

Include those lines, the cycle and transfer lines around the event,
your settings (`lightning-cli listconfigs | grep xrebalance`), and
the CLBOSS, plugin, and Core Lightning versions.
