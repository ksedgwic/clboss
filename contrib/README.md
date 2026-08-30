# Contributed CLBOSS Utilities

## Installing

There are two ways to install the requirements:
- poetry
- nix

### Poetry
There are two ways to install poetry:
- pipx
- official installer

#### Pipx

```
# Install pipx
sudo apt update
sudo apt install pipx
pipx install poetry
```

#### [Or, click here for the official installer](https://python-poetry.org/docs/#installing-with-the-official-installer)

Once poetry is installed, install the Python dependencies:

```
# The following commands need to be run as the user who will be running
# the clboss utility commands (connecting to the CLN RPC port)

# Install clboss contrib utilities
poetry shell
poetry install
```

### Nix
If you have nix, you can just do, from the project root:
```
nix-shell contrib-shell.nix
```

Then before running the commands below, be sure to do:

```
cd contrib/
```

## Running

```
./clboss-earnings-history

./clboss-recent-earnings

./clboss-routing-stats

./clboss-forwarding-stats

./clboss-channel-sizing

./recently-closed

./clboss-xrebalance-view

./cln-plugin-bounce <plugin-name>...

The `clboss-routing-stats` and `clboss-forwarding-stats` scripts now accept `--days` to limit
how many days of earnings history are considered when ranking channels.

```

### Script Details

- **`clboss-earnings-history`** now supports additional options:
  - `--csv-file <file>` writes the raw earnings data as CSV.
  - `--graph-file <file>` generates a PNG plot of net earnings.
  - `--bucket` lets you aggregate by `day`, `week`, `fortnight`, `month`, or `quarter`.
- **`clboss-forwarding-stats`** summarizes channel forwarding data and can be
  restricted with `--days`.
- **`clboss-channel-sizing`** shows which channels want more capacity on
  our side and which carry capital that never moves.  Each hour of
  CLBOSS's balance samples is labelled low (local side under `--edge`
  percent of capacity, default 10), high (over 100 - edge) or interior;
  settled forwards are credited to the state their channel was in, giving
  an interior and an edge earn rate per direction (`%Low`/`%Int`/`%High`
  are the shares of hours in each state; a rate over a state the channel
  was never in prints as `-`).  `GainLoc` is what the
  low hours would have earned at the interior rate (our side ran dry) and
  `GainInb` the same for the high hours (the peer's side ran dry): upper
  bounds on what more capacity on that side recovers (run with `--edge 5`
  for a lower bound).  They are only meaningful for `Class` `bidir`
  channels (flow within 25% of balanced, rebalancing under 25% of
  forwarded volume, not pinned at an edge); a sink or source drains to
  the same floor at any size, and a channel at one edge 90% of the time
  is classed by that edge because the flow it refuses there never shows
  up in its net flow.  `?` marks a rate resting on under three days of
  interior time.  `MinLoc` is the lowest the local side got during the
  window, in M sat: capital that never moved, which a splice-out could
  remove without changing any forward that happened.  A longer window can
  only lower it, so read it at 60-90 days before removing capital; the
  samples are hourly, so a dip that came and went within the hour is
  missed.

  The table is grouped.  *Grow*: `bidir` channels whose `GainLoc` clears
  `--min-gain` (default 1000 sat, about one on-chain transaction), by
  `GainLoc`.  *Shrink*: channels whose `MinLoc` is at least `--idle-frac`
  of capacity (default 0.4) and 1M sat, by `MinLoc`.  *Right-sized*: the
  other `bidir` channels, by `NetEarn` (forwarding fees less the
  rebalance expenditures CLBOSS attributes to the peer over the window).  *Liquidity-limited*: sinks,
  sources and rebalance-carried channels, where refills rather than size
  are the lever -- sinks together by `RefOut`, then sources by `GainInb`
  (inbound refusals happen at the peer, so that estimate is all there
  is), then rebalance-carried by `NetEarn`.  *Too young*: under `--min-days` (30, the
  default window) of samples, since a new channel's balance is its
  opening state rather than its behavior, by `Days`.  *Little or no traffic*: under `--min-turns`
  (0.1) of capacity forwarded in the window -- a probe still waiting for
  flow, or a dead channel, told apart by `Days` -- by `Cap`.  `Change` is the proposed splice in M sat: for
  Shrink, remove enough that the window's lowest local balance lands at
  the edge threshold of the smaller channel (in parentheses when under 1M sat
  would be left: consider closing instead, which is
  `clboss-forwarding-stats --days 90 --sort tral`'s call); for Grow, add the current capacity -- a step rather
  than a measurement, since refused volume is inflated by retries, so
  double and read the next window.  In Grow and Shrink the `Hold`
  column names the penalty to weigh before acting on a row -- advice, not a veto: `thin` evidence; peer `offline` or `flaky`
  (3-day connect rate under 90%); or, when the peer cannot splice so a
  resize means a close, `inbound` for the inbound liquidity the close
  would forfeit (`Inb` over a quarter of `Cap`) and `busy` for the
  productive channel it would interrupt (`NetEarn` over `--min-gain`).
  On a terminal the triggering stat is tinted and actionable rows are
  bold (`--color` forces this, `--no-color` disables it).

  `Turns` is forwarded volume over capacity for the window.  `Inb` is the
  peer's side of the channel now, in M sat: the inbound liquidity a
  close-and-reopen gives up and a splice keeps.  `Up`/`Up3d` show whether
  the peer is connected now and its 3-day connect rate, `Splice` whether
  it negotiates or announces splicing (feature bit 62/63), so a candidate
  can be resized in place instead of closed and reopened.  `--days` sets
  the window (default 30; a candidate should also show at 60 before
  acting; balance samples exist since CLBOSS started recording them),
  `--since`/`--before` fix it in unix seconds, `--wide` adds the edge
  earn rates, the share of refused forwards that happened at the edge and
  the balance range used, `--sort` picks a flat order instead of groups,
  `--json` dumps the rows with their group and holds.
- **`clboss-routing-stats`** ranks peers using recent earnings data and also
  accepts the `--days` option.
- **`recently-closed`** lists channels that closed within the last N days, also
  controlled via `--days`.
- **`clboss-xrebalance-view`** shows what the xrebalance planner would do
  with the node's current state: every channel with its balance band and
  windowed net earnings rates, the fill and drain candidates tinted, the
  floor ladder with each rung's volume and budget, the channels of the
  widest cycle in bold, and the `xrebalance` request the driver would send
  for it (`dryrun=true` by default).  Tuning defaults come from the live
  `clboss-xrebalance-*` options; `--fill-loc`, `--drain-loc`,
  `--route-cost-floor`, `--grant`, and `--gain` preview other settings.
- **`cln-plugin-bounce`** stops and restarts running plugins without
  restarting `lightningd`.  `plugin stop` needs a plugin's exact
  registered name, which for versioned installs includes the version
  string; the script looks each one up from `plugin list`, stops the
  named plugins in the order given, and starts them again in reverse
  order, so the list order encodes any shutdown dependency between
  them.  Restarts use the unversioned sibling path when one exists
  (usually a symlink maintained by the install script), so a repointed
  symlink brings up the new version.  Config-file edits made since
  `lightningd` started are applied in a second phase, which is skipped
  with a warning naming the option and file when any config-file
  option is no longer registered -- including one a newly installed
  build dropped, which leaves `lightningd` holding a stale configvar
  until it restarts.  Plugin names are the arguments
  not starting with `-`; every other argument is passed to
  `lightning-cli` (e.g. `--signet --lightning-dir=...`), so names and
  options may appear in any order.  Plain POSIX sh plus `jq`, so unlike
  a shell alias it also works under `sudo`.
- **`fee-log-parser`** is a parser that streams DEBUG-level logging and writes
  a sqlite database containing fee algorithm information. CLBOSS now records
  the same schema in its internal database (`data.clboss`, tables
  `feemon_peers` and `feemon_change_events`) during normal operation.
- **`clboss-feemon-history`** is a CLBOSS command that returns per-peer fee modifier
  history between optional `since`/`before` timestamps.
- **`clboss-feemon-peers`** is a CLBOSS command that returns peer nodeids with fee
  monitor history between optional `since`/`before` timestamps.
- **`feemon-validate`** compares `fee-log-parser` sqlite history against
  `clboss-feemon-history` per peer over a requested time window. It reports
  per-peer progress, prints compact timestamp diagnostics for missing/extra
  records, prints full-record diagnostics for field mismatches, and exits
  non-zero when discrepancies are found. Default external DB path is
  `./clboss-fee-info.sqlite3` and default timestamp tolerance is 60 seconds.
  Default float tolerance is `1e-5` and is scaled by value magnitude
  (`tol * max(1, |a|, |b|)`) to avoid false mismatches from JSON float
  rendering precision (notably `mult_product`).
  Derived integer fields `est_base` and `est_ppm` use a relative tolerance
  with default `1e-3` (`--int-rel-tolerance`) so small rounding effects at
  large magnitudes do not trigger mismatches.
  `--since`/`--before` accept Unix epoch seconds in addition to the existing
  human-readable time formats. Naive timestamps are interpreted in local time;
  Unix epoch input is UTC; explicit timezone offsets are honored.
- **`plot-fees`** plots fee-related time series for a peer from merged fee monitor
  data: API history (`clboss-feemon-history`) plus legacy sqlite history
  (`fee-log-parser`). When both sources cover a period, API records are
  preferred and sqlite is used only for earlier history. By default it uses API
  data only; pass `--db` to include legacy sqlite history. `--peer` accepts a
  nodeid, alias (via lightning-cli/listnodes), or SCID (via
  lightning-cli/listpeerchannels). The combo view includes a daily earnings
  panel (incoming/outgoing msat per day) when lightning-cli is available, and
  the `incoming-earnings`/`outgoing-earnings` views render those panels on
  their own. In the `theory` panel, a `theory_center` line is drawn only where
  API records include `price_center`; legacy-only spans omit that line. Use
  `--title` to override the plot title (defaults to the peer label; pass empty
  to omit).
- **`plot-aggregate`** plots aggregate percentile summaries from merged fee monitor
  data (API preferred over overlapping legacy sqlite history). By default it
  uses API data only; pass `--db` to include legacy sqlite history. Views include
  `baseline-base`, `baseline-ppm`, `size`, `balance`, `theory`,
  `advertised-base`, `advertised-ppm`, `earnings`, and a `combo` view. Each
  view shows daily
  p00/p10/p25/p50/p75/p90/p100 percentiles across nodes. The `earnings`
  view uses `clboss-earnings-history all` to compute net earnings
  percentiles (sat/day). In API mode, peer discovery uses
  `clboss-feemon-peers [since] [before]` so windowed aggregate plots include
  peers that were active during the selected period (even if currently closed).
