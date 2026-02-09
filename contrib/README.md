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

./recently-closed

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
- **`clboss-routing-stats`** ranks peers using recent earnings data and also
  accepts the `--days` option.
- **`recently-closed`** lists channels that closed within the last N days, also
  controlled via `--days`.
- **`fee-log-parser`** is a parser that streams DEBUG-level logging and writes
  a sqlite database containing fee algorithm information. CLBOSS now records
  the same schema in its internal database (`data.clboss`, tables
  `feemon_peers` and `feemon_change_events`) during normal operation.
- **`clboss-feemon-history`** is a CLBOSS command that returns per-peer fee modifier
  history between optional `since`/`before` timestamps.
- **`feemon-validate`** compares `fee-log-parser` sqlite history against
  `clboss-feemon-history` per peer over a requested time window. It reports
  per-peer progress, prints compact timestamp diagnostics for missing/extra
  records, prints full-record diagnostics for field mismatches, and exits
  non-zero when discrepancies are found. Default external DB path is
  `./clboss-fee-info.sqlite3` and default timestamp tolerance is 20 seconds.
  Default float tolerance is `1e-5` and is scaled by value magnitude
  (`tol * max(1, |a|, |b|)`) to avoid false mismatches from JSON float
  rendering precision (notably `mult_product`).
  Derived integer fields `est_base` and `est_ppm` use a relative tolerance
  with default `1e-3` (`--int-rel-tolerance`) so small rounding effects at
  large magnitudes do not trigger mismatches.
  `--since`/`--before` accept Unix epoch seconds in addition to the existing
  human-readable time formats. Naive timestamps are interpreted in local time;
  Unix epoch input is UTC; explicit timezone offsets are honored.
- **`plot-fees`** plots fee-related time series from the `fee-log-parser` sqlite
  output. `--peer` accepts a nodeid, alias (via lightning-cli/listnodes), or
  SCID (via lightning-cli/listpeerchannels). The combo view includes a daily
  earnings panel (incoming/outgoing msat per day) when lightning-cli is
  available, and the `incoming-earnings`/`outgoing-earnings` views render
  those panels on their own. Use `--title` to override the plot title
  (defaults to the peer label; pass empty to omit).
- **`plot-aggregate`** plots aggregate percentile summaries from the
  `fee-log-parser` sqlite output. Views include
  `baseline-base`, `baseline-ppm`, `size`, `balance`, `theory`,
  `advertised-base`, `advertised-ppm`, `earnings`, and a `combo` view. Each
  view shows daily
  p00/p10/p25/p50/p75/p90/p100 percentiles across nodes. The `earnings`
  view uses `clboss-earnings-history all` to compute net earnings
  percentiles (sat/day).
