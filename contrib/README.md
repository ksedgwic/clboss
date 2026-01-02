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

./fee_log_parser /path/to/lightning.log

./fee_log_parser /path/to/lightning.log --db /path/to/fee-changes.sqlite3

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
- **`fee_log_parser`** parses fee-setting logs from stdin or file paths, emits CSV
  to stdout by default, and can store results in sqlite with `--db`.
- **`plot-fees`** plots fee-related time series from the `fee_log_parser` sqlite
  output. Use `price-level` or `base-ppm` views for `price_level` or
  `est_base`/`est_ppm` respectively.
