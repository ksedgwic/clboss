> ⚠️ **Note about repository ownership**
>
> This repository was originally created and maintained by [ZmnSCPxj](https://github.com/ZmnSCPxj).
> In 2025, the top-level CLBOSS repository was transferred to
> [Ken Sedgwick](https://github.com/ksedgwic), who is the current maintainer.

CLBOSS The Core Lightning Node Manager
===================================

CLBOSS is an automated manager for Core Lightning forwarding nodes.

CLBOSS is effectively a bunch of heuristics modules wired together
to a regular clock to continuously monitor your node.

Its design goal is to make it so that running a Lightning Network
node is as simple as installing Core Lightning and CLBOSS, putting
some amount of funds of 0.1BTC or more, and making sure you have
continuous Internet and power to the hardware running it.

Current versions of CLBOSS might not achieve this goal yet
perfectly, but hopefully with enough effort and iteration and raw
coding and etc etc it will someday be as unusual to manually
manage a Lightning node as writing (as opposed to reading) machine
language is unusual today.

I hope CLBOSS can make the transition from pre-Lightning to
post-Lightning much smoother in practice.

So far CLBOSS can do the following automatically:

* Open channels to other, useful nodes when fees are low and there are onchain funds
* Acquire incoming capacity via `boltz.exchange` swaps.
* Rebalance open channels, via the external `xrebalance` plugin.
* Set forwarding fees so that they're competitive to other nodes

You can read more information about CLBOSS here:
https://zmnscpxj.github.io/clboss/index.html
As of this release, this page is a work in progress, stay tuned
for updates!

## Project ownership and maintenance

This repository was originally created and maintained by [ZmnSCPxj](https://github.com/ZmnSCPxj).
In 2025, ownership of the top-level CLBOSS repository was transferred to
[Ken Sedgwick](https://github.com/ksedgwic), who is the current maintainer.

All history and prior contributions remain credited to their original authors.

Dependencies
------------

If you are installing from some official [source release tarball](https://github.com/ZmnSCPxj/clboss/releases),
you only need the below packages installed.

Debian-derived systems:
* `build-essential`
* `pkg-config`
* `libev-dev`
* `libcurl4-gnutls-dev`
* `libsqlite3-dev`
* `libunwind-dev`

RPM-dervied :
* `groupinstall "Development Tools"`
* `pkg-config`
* `libev-devel`
* `libcurl-devel`
* `libsqlite3x-devel`
* `libunwind-devel`

Alpine:
* `build-base`
* `pkgconf`
* `libev-dev`
* `curl-dev`
* `sqlite-dev`
* `libunwind-dev`

Equivalent packages have a good probability of existing in
non-Debian-derived distributions as well.

The following dependency is technically optional, but is strongly
recommended (CLBOSS will check it at runtime so you do not need
it while building):

* `dnsutils`

For alpine linux the package is: `bind-tools`.

If you have to build directly from github.com, you need the below
Debian/RPM/Alpine packages in addition:

* `git`
* `automake`
* `autoconf-archive`
* `libtool`

A design goal of CLBOSS is to reduce the above dependencies even
further.

Installing
----------

### Requirements

CLBOSS supports **Core Lightning v26.04 or later**; that is the
oldest release it is tested against.  Its probing subsystems build
routes from the `getroutes` per-hop out-side fields (`node_id_out` /
`amount_out_msat` / `cltv_out`): on v26.06 and later these are read
directly, and on older versions they are derived from the deprecated
per-hop fields, which carry the same values one hop over.

At startup CLBOSS checks the CLN version.  From v25.09 up to v26.04
it starts with a warning that the version is untested.  Older than
v25.09 it refuses to run — before creating or modifying any on-disk
state — because `getroutes` gained the `maxparts` parameter in
v25.09 and CLBOSS passes it on every call, so probing, dowsing, and
candidate matchmaking would all fail.  If (and only if) your older
CLN carries backports of what CLBOSS needs (askrene `getroutes` with
`maxparts`, `xpay`), you can bypass the refusal with
`--clboss-skip-cln-version-check`.  Users on older CLN releases
should stay on CLBOSS 0.16.x.

From an [official source release](https://github.com/ZmnSCPxj/clboss/releases), just:

    ./configure && make
    sudo make install # or su first, then make install

This will install `clboss` as a standard executable, usually in
`/usr/local/bin/` by default.
You will then need to modify your `lightning.conf` to add the
path to `which clboss` as a `plugin` or `important-plugin` of
`lightningd`.

For production rollouts/quick rollback, you can install a versioned
binary and keep `clboss` as a symlink you can switch:

    sudo make install-versioned

This installs `clboss-<git-describe>` (e.g.
`/usr/local/bin/clboss-v0.15.1-47-g9b61c28`) and updates the symlink
at `/usr/local/bin/clboss` (or whatever `--prefix` you configured).

To roll back (or switch forward) after multiple installs replace the
`/usr/local/bin/clboss` symbolic link with one pointing to the
desired version.

Usually, autotools-based projects like CLBOSS will default
to using `-g -O2` for compilation flags, where `-g` causes
the compiler to include debug info.
CLBOSS changes this default to `-O2` so that users by default
get a binary without debug symbols (a binary with debug symbols
would be 20x larger!), but if it matters to you, you can
override the CLBOSS default via `CXXFLAGS`, such as:

    ./configure CXXFLAGS="-g -O2"  # or whatever flags you like
    ./configure CXXFLAGS="-g -Og"  # recommended for debugging
    ./configure CXXFLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address" # useful for debugging heap issues

And if your build machine has more than 1 core, you probably
want to pass in the `-j` option to `make`, too:

    make -j4  # or how many cores you want to build on

From a git clone, you first need to execute:

    autoreconf -i

Then run the `./configure && make && sudo make install`.

You can then add a `plugin=/path/to/clboss` or
`important-plugin=/path/to/clboss` setting to your Core Lightning
configuration file.

### FreeBSD

The following packages as of 12.2-RELEASE are necessary when
building, whether from git clone or from official source
release:

    pkg install curl
    pkg install gmake
    pkg install libev
    pkg install pkgconf
    pkg install sqlite3
    pkg install libunwind

In addition, you have to use `gmake` for building, not the
system `make`, as the included `libsecp256k1` requires
`gmake`.

    ./configure && gmake
    sudo gmake install # or su first, then gmake install

You need to install the below first before you can run
`autoreconf -i` sucessfully on a git clone.

    pkg install autoconf-archive
    pkg install autotools
    pkg install git

While releases and pre-releases will be tested for
compileability in a FreeBSD VM, git `master` may
transiently be in a state where the default CLANG may
raise warnings that are not raised by GCC, or may refer to
Linux-specific header files and functions.

### Nix

If you are a Nix user for developments you are use nix to build clboss, and to get started
you need nix flake activated on your machine and then run the following command:

```
nix develop
autoreconf -i
./configure && make
```

### Developer helpers

To generate an IDE-friendly `compile_commands.json`, run `make
compile_commands.json` (requires `bear`). The file is git-ignored;
rerun the target whenever your build flags or sources change.

### Coverage

To run the unit tests with code coverage instrumentation:

    make coverage

If `lcov`/`genhtml` are available, this writes `coverage.info` and an HTML report
at `coverage-html/index.html`. To generate the report from an existing
coverage-instrumented build, run:

    make coverage-report

If you want debug symbols as well, override the coverage flags, e.g.:

    make COVERAGE_CXXFLAGS="-Og -g --coverage" COVERAGE_CFLAGS="-Og -g --coverage" coverage

To clean coverage outputs (`coverage.info`, `coverage-html/`, and `*.gcda/*.gcno`
files):

    make coverage-clean

Installing `lcov`:
* Fedora: `sudo dnf install lcov`
* Debian/Ubuntu: `sudo apt-get install lcov`
* Or via Nix (without installing system-wide): `nix shell nixpkgs#lcov --command make coverage-report`

### Testing on signet w/ swaps

CLBOSS can run its `boltz.exchange` swaps against a signet Boltz backend,
which is useful for exercising the swap machinery without spending real
funds. This needs a CLBOSS built with signet support, which ships a
`Network_Signet` Boltz instance.

The signet Boltz service has no public clearnet endpoint, so CLBOSS
reaches it at a local address (`http://127.0.0.1:8080`) that you front
with a forward to the service. The service is reachable over I2P
(preferred) or Tor — its `.onion` is also configured, but has been
unreliable in practice.

To forward the local port over I2P:

1. Install and run [i2pd](https://i2pd.readthedocs.io/). Its SOCKS5 proxy
   listens on `127.0.0.1:4447` by default; no extra configuration is
   required.

2. Forward `127.0.0.1:8080` to the signet Boltz I2P address through that
   SOCKS5 proxy with `socat`. A systemd unit keeps it running:

```
[Unit]
Description=socat proxy: 127.0.0.1:8080 -> Boltz signet I2P (via i2pd SOCKS5)
After=i2pd.service network-online.target
Wants=i2pd.service

[Service]
Type=simple
ExecStart=/usr/bin/socat --experimental -d TCP4-LISTEN:8080,bind=127.0.0.1,fork,reuseaddr SOCKS5-CONNECT:127.0.0.1:4447:boltz7c3f7bb5pe7k25uv2wdd57oayoazacorwdvzksz6q7hicxq.b32.i2p:80
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

For a one-off test, run just the `socat …` command from `ExecStart`
directly. With i2pd and this forward running, a signet CLBOSS uses the
Boltz signet backend automatically.

To smoke-test the backend without CLBOSS, the `dev-boltz-api` helper
honors two environment variables: set `BOLTZ_PROXY=socks5h://127.0.0.1:4447`
and `BOLTZ_API_BASE=http://boltz7c3f7bb5pe7k25uv2wdd57oayoazacorwdvzksz6q7hicxq.b32.i2p`
to reach the service directly over I2P.

### Contributed Utilities

There are a number of contributed utilities in the `contrib` directory.  See
`contrib/README.md` for details on scripts such as `clboss-earnings-history`,
`clboss-forwarding-stats`, `clboss-routing-stats`, and `recently-closed`.


Operating
---------

A goal of CLBOSS is that you never have to monitor or check your
node, or CLBOSS, at all.

Nevertheless, CLBOSS exposes a few commands and options as well.
Many of them are undocumented commands for internal testing, but
some may be of interest to curious node operators, or those who
have special use-cases.

### `clboss-status`

This simply displays a bunch of status about a few of the modules
CLBOSS has.
Possibly the most interesting are these:

* `channel_candidates` - An array of nodes that we plan to
  eventually build channels to in the future, if we ever get
  onchain funds.
  `onlineness` only reaches up to 24 and saturates.
  Candidates are generally scored by the uptime they appear to
  have (CLBOSS tries to `connect` to them).
* `internet` - Whether CLBOSS thinks we are online or offline
  right now.
  We generally check connectivity every 10 minutes, so you
  could be offline for shorter than that before CLBOSS notices.
  CLBOSS does not perform uptime measurements on other nodes
  if we are offline.
* `onchain_feerate` - Sampled onchain feerate for `normal`, as
  well as whether CLBOSS currently thinks we are in a low-feerate
  or high-feerate time period.
  Also displayed is the various thresholds.
  If CLBOSS is in `high fees` judgment, then if sampled feerates
  fall below `hi_to_lo` it switches to `low fees`.
  Vice versa, if it is in `low fees` it switches to `high fees`
  if feerates go above `lo_to_hi`.
  `init_mid` is the boundary used when CLBOSS starts up.
  CLBOSS tries to hold off on onchain activity (e.g. opening
  channels, swapping offchain funds to onchain addresses) during
  high fee periods except in extremis (e.g. you have no channels
  at all, or you have no or very little incoming liquidity).
* `peer_metrics` - Various metrics on nodes we currently have
  channels to.
  Possibly the most interesting is `connect_rate`, which is the
  uptime we think they have.
  `age` is in seconds.
  The metrics shown are for the last 3 days, though CLBOSS stores
  the raw statistics for the past two months.

### `clboss-feerates`

Returns the same information printed by the `onchain_feerate` section of
`clboss-status`. It reports the current percentile thresholds, the last
observed feerate (in perkw), and whether CLBOSS currently judges fees to
be `low` or `high`.

### `clboss-externpay`

If CLBOSS is managing a node for a custodial service, then you should
`decode` the invoices provided by clients whose funds you are
custodying, and pass the `payment_hash` as the sole argument to
`clboss-externpay`.

CLBOSS gathers data for statistics by also monitoring `pay`
commands.
If `pay` tries to route to a payee via a peer, but the payment
fails, CLBOSS considers it a failing of that peer, which should
really be monitoring its own peers and consider them bad and close
channels with them if delivering by those fails too often, with
the logic applied transitively (so even a remote failure is always
blamed on the first hop, because it should be monitoring its own
next hops, which should be monitoring their own next hops...).

Knowing that CLBOSS does this, a client of a custodial service can
craft an unredeemable invoice in order to mess with those
statistics, making a particular peer of the custodial service appear
to be failing.

`clboss-externpay` closes this attack by specifically ignoring
payments that match the given `payment_hash`.

You should not use it when paying invoices to your employees or
stakeholders, as presumably those have incentives aligned with you.
Presumably they want to get paid, so would give you invoices they
can redeem perfectly well.

### `clboss-ignore-onchain`, `clboss-notice-onchain`

Suppose you have the following story:

* You want to pay to some onchain address.
* All your funds are locked in Lightning channels on an LN node
  managed with CLBOSS.

Here is another user story:

* You have a popular Core Lightning forwarding node that you are
  happily not managing because you are using CLBOSS The Automated
  Core Lightning Node Manager.
* A friend asks a favor to get some incoming liquidity.
  * You should really talk them into running Core Lightning and
    CLBOSS themselves to get incoming liquidity
* You decide to help them out and give them some capacity.
* You take some funds from cold storage and send it onchain to
  your Core Lightning node.
* CLBOSS is so awesome, it takes those onchain funds and puts
  them into channels *it* has chosen rather than channels *you*
  wanted to choose.
* You end up not being able to help your friend.
  * At this point you talk them into running Core Lightning and
    CLBOSS.

To help with these user stories, CLBOSS provides the
`clboss-ignore-onchain` command.
After executing this command, CLBOSS will temporarily ignore
onchain funds (with the side effect that it will not try to
get its own incoming liquidity by moving offchain funds to
onchain addresses, since those funds would end up being ignored
by CLBOSS instead of being managed).
CLBOSS will continue to rebalance your offchain funds, monitor
peers, check that channel candidates have high uptime, look for
new channel candidates, manage channel fees, and so on.

Then, you can perform onchain actions manually, such as moving
cold storage into your Lightning node and making a channel
manually for a friend, or closing some LN channels and withdrawing
those funds to an onchain address.

`clboss-ignore-onchain` accepts an optional `hours` argument, a
number of hours that it will ignore onchain funds.
If not specified, this defaults to 24 hours.
You can specify this again at a later time to extend the ignore
time if needed.

Once you have completed any manual onchain funds management,
you can run `clboss-notice-onchain` in order to let CLBOSS
resume normal operation.
In any case, `clboss-ignore-onchain` is temporary and even
if you forget to issue `clboss-notice-onchain` CLBOSS will
resume managing onchain funds at some point.

### `clboss-unmanage`

Continuing with the previous user story, suppose after the
channel has been established, as a favor to your friend you
decide not to charge LN fees towards their node.

Normally CLBOSS will automatically manage LN fees for every
channel.
To suppress this, you can use the `clboss-unmanage` command,
which has two parameters, `nodeid` and `tags`.

    lightning-cli clboss-unmanage ${NODEID} lnfee

After the above command, you can set the fee manually with
the normal Core Lightning `setchannel` command.

The second parameter, `tags`, is a string containing a
comma-separated set of unmanagement tags.
For example, you can require that opening channels to a
particular node is done only by your manual intervention,
even if CLBOSS decides later that opening channels to that
node is a good idea, and also require that CLBOSS not set
channel fees automatically:

    lightning-cli clboss-unmanage ${NODEID} lnfee,open

To resume full management of the node, give an empty string:

    lightning-cli clboss-unmanage ${NODEID} ""

The possible unmanagement tags are:

* `lnfee` - Do not manage the channel fee of channels to this
  node.
* `open` - Do not automatically open channels to this node.
* `close` - Do not automatically close channels to this node.
* `balance` - Do not automatically move funds (rebalance) to or
  from this node.

### `clboss-swaps`

CLBOSS will sometimes swap Lightning funds for onchain funds,
and *then* put the onchain funds into new channels.
This is generally done to acquire incoming capacity for a new
node, or if incoming capacity got closed.

This swapping is done via various online swap providers.
These providers charge for this swap service.

The `clboss-swaps` command provides the list of offchain-to-onchain
swaps, including how much was disbursed and how much got returned
in the swap.

This recording only started in 0.11D.
Earlier versions do not record, so if you have been using CLBOSS
before 0.11D, then historical offchain-to-onchain swaps are not
reported.

### `--clboss-min-onchain=<satoshis>`

Pass this option to `lightningd` in order to specify a target
amount that CLBOSS will leave onchain.
The amount specified must be an ordinary number, and must be
in satoshis unit, without any trailing units or other strings.

The default is "30000", or about 0.0003 BTC.
The intent is that this minimal amount will be used in the
future, by Core Lightning, to manage anchor-commitment channels,
or post-Taproot Decker-Russell-Osuntokun channels.
These channel types need some small amount of onchain funds
to unilaterally close, so it is not recommended to set it to 0.

The amount specified is a ballpark figure, and CLBOSS may leave
slightly lower or slightly higher than this amount.

### `--clboss-auto-close=<true|false>`

This version of CLBOSS has ***EXPERIMENTAL*** code to monitor
channels and close them if they are not good for your earnings.

This monitoring can be seen in `clboss-status`, under the
`peer_complaints` and `closed_peer_complaints` keys.

As this feature is experimental, it is currently disabled by
default.
You can enable it by adding `clboss-auto-close=true` in your
`lightningd` configuration.
Even if it is disabled, this monitoring is still performed and
reported in `clboss-status`, channels are simply not actually
closed, but most of the algorithm is still running (so you can
evaluate yourself if you agree with it and maybe enable it
yourself later).

Even if you have auto-closing enabled, you can use the
`clboss-unmanage` command with key `close` to ensure that
particular channels to particular nodes will not be auto-closed
by CLBOSS (they may still be closed by `lightningd` due to an
HTLC timeout, or by the peer for any reason, or by you; this
just suppresses CLBOSS).

### `--clboss-zerobasefee=<require|allow|disallow>`

Pass this option to `lightningd` to specify how this node will
advertise its `base_fee`.

* `require` - the `base_fee` must be always 0.
* `allow` - if the heuristics of CLBOSS think it might be a
  good idea to set `base_fee` to 0, let it be 0, but otherwise
  set it to whatever value the heuristics want.
* `disallow` - the `base_fee` must always be non-0.
  If the heuristics think it might be good to set it to 0,
  set it to 1 instead.

On 0.11C and earlier, CLBOSS had the `disallow` behavior.
In this version, the default is the `allow` behavior.

Some pathfinding algorithms under development may strongly
prefer 0 or low base fees, so you might want to set CLBOSS
to 0 base fee, or to allow a 0 base fee.

### `--clboss-min-channel=<satoshis>` / `--clboss-max-channel=<satoshis>`

Sets the minimum and maximum channel sizes that CLBOSS
will make.

The defaults are:

* Minimum: 500000sats = 5mBTC
* Maximum: 16777215sats = 167.77215mBTC

The channel-creation planner requires
`max-channel >= 3 * min-channel + 20000`.
If the configured pair violates this, CLBOSS keeps the
maximum and lowers the minimum to the largest value that
fits, logging a warning.

Specify the value in satoshis without adding any unit
suffix, e.g.

    lightningd --clboss-min-channel=1000000

### `--clboss-rebalance-mode=<xrebalance|off>`

Selects how CLBOSS rebalances channel liquidity:

* `xrebalance` (default): the circular askrene rebalancer, executing
  through the external `xrebalance` plugin, which must be loaded into
  `lightningd`.  Without the plugin, CLBOSS idles with a log hint.
* `off`: disable autonomous rebalancing entirely.

The `xrebalance` plugin is a separate CLN plugin; releases and install
instructions are at <https://github.com/ksedgwic/xrebalance>.

Rebalance cycles run on a Poisson clock (`clboss-xrebalance-per-hour`
below) and are additionally triggered on demand, when an observed
forward drains a channel that is low on local liquidity.

This is a *dynamic* option: set it in the `lightningd` config for the
startup default, or change it at runtime without a restart with

    lightning-cli setconfig clboss-rebalance-mode <mode>

### `--clboss-xrebalance-*` tuning options

Each xrebalance cycle selects fill candidates (channels low on local
liquidity) and drain candidates (channels high on it), admits and
sizes them from each peer's windowed net earnings, matches them into
one min-cost-flow transfer, and prices that transfer from what the
involved peers actually earn — so rebalancing never spends more on a
corridor than the corridor's own track record justifies.

All of the following are *dynamic* (`setconfig`) options:

* `clboss-xrebalance-per-hour` — average matched-cycle rate, Poisson
  paced; `0` pauses the matched loop (demand-triggered cycles still
  run).  Default `12`.
* `clboss-xrebalance-route-cost-floor` — the ppm floor at which the
  matched pool stops growing; also sets the cycle's amount and fee
  budget.  `auto` derives a ladder of floors and sweeps a random rung
  each cycle.  Default `auto`.
* `clboss-xrebalance-earnings-window-days` — trailing window over
  which per-peer net earnings rates are measured.  Default `90`.
* `clboss-xrebalance-fill-loc` / `clboss-xrebalance-drain-loc` — the
  band edges, in percent of local liquidity: channels at or below
  `fill-loc` are fill candidates, at or above `drain-loc` are drain
  candidates, and each transfer aims the channel back at its band
  edge.  Defaults `25` and `75`.
* `clboss-xrebalance-maxparts` — how many parts (paths) the
  min-cost-flow solve may split a transfer into.  Lower means fewer,
  fatter parts; higher means finer splitting and more learning but
  more refusals.  Default `80`.
* `clboss-xrebalance-grant` — assumed prior earnings rate (ppm),
  credited to every channeled peer as if already earned on one
  capacity-turn of volume; admits peers with no track record at that
  rate, and real volume dilutes the credit toward the measured rate.
  Default `0` (record-only).
* `clboss-xrebalance-gain` — multiplier on the measured earnings
  rates before candidacy and pricing; above `1` accepts routes
  costing up to gain times the measured rate.  Default `1` (strict).

### `--clboss-min-nodes-to-process=<number>`

Sets the minimum number of nodes that CLBOSS must know about before it
will try to propose channels to popular nodes.  Pass this option to
`lightningd` to override the default threshold.

The defaults depend on the network:

* Bitcoin: 800
* Testnet: 100
* Other networks: 10

Setting the option to `-1` reverts to the built-in network-specific
default.

### `--clboss-candidate-record-window-days=<days>`, `--clboss-candidate-keeper-tral-bps=<bps>`, `--clboss-candidate-min-record-days=<days>`, `--clboss-candidate-prefer-spliceable=<true|false>`

When CLBOSS selects which investigated candidates to actually open
channels to, it partitions them by their earnings *track record*: what
a previous (now closed) channel with that node earned for us, net of
rebalance costs.  Candidates with a proven good record ("keepers") are
funded first, candidates with no usable history next, and candidates
with a poor record are used only when no better candidate can absorb
the available funds.  Within the no-history tier, nodes announcing
splicing support are preferred, since their channels can be resized
later without a close+reopen.

The judgment metric is TRAL — annualized net return on liquidity, in
basis points — the same metric reported by the
`contrib/clboss-forwarding-stats` tool.  It is computed over a sliding
window from the daily earnings buckets and the (roughly hourly) fee
monitor records, counting only the days a channel actually operated
within the window, so partially-overlapping or mid-window-closed
channels are annualized fairly.

* `clboss-candidate-record-window-days` — how many days of history to
  consider.  Default `180`.
* `clboss-candidate-keeper-tral-bps` — TRAL at or above which a
  candidate's record marks it a keeper.  Default `50` (0.5%/year).
* `clboss-candidate-min-record-days` — minimum observed operational
  days within the window before the record is trusted; below this the
  candidate is treated as having no record.  Default `7`.
* `clboss-candidate-prefer-spliceable` — whether the no-history tier
  is ordered with splicing-capable nodes first.  Default `true`.

All four are *dynamic* options: set them in the `lightningd` config
for the startup default, or change them at runtime without a restart
with

    lightning-cli setconfig clboss-candidate-keeper-tral-bps <bps>

### `clboss-track-record`

Shows the earnings track record and verdict for a single node, exactly
as the channel-open candidate selection would judge it:

    lightning-cli clboss-track-record nodeid=<nodeid>

The output includes the verdict (`keeper`, `no-record`, or
`underperformer`), the computed `tral_bps`, the observed operational
days, the net earnings in the window, and the current values of the
three `clboss-candidate-*` options above — handy when tuning them at
runtime with `setconfig`.

### `clboss-recent-earnings`, `clboss-earnings-history`

As of CLBOSS version 0.14, earnings and expenditures are tracked on a daily basis.
The following commands have been added to observe the new data:

- **`clboss-recent-earnings`**:
  - **Purpose**: Returns a data structure equivalent to the
    `offchain_earnings_tracker` collection in `clboss-status`, but
    only includes recent earnings and expenditures.
  - **Arguments**:
    - `days` (optional): Specifies the number of days to include in
      the report. Defaults to a fortnight (14 days) if not provided.

- **`clboss-earnings-history`**:
  - **Purpose**: Provides a daily breakdown of earnings and expenditures.
  - **Arguments**:
    - `nodeid` (optional): Limits the history to a particular node if
      provided. Without this argument, the history is accumulated
      across all peers. Use `all` to return per-node buckets; each
      history entry then includes a `node` field.
  - **Output**: 
    - The history consists of an array of records showing the earnings
      and expenditures for each day.
    - The history includes an initial record with a time value of 0,
      which contains any legacy earnings and expenditures collected by
      CLBOSS before daily tracking was implemented.

These commands enhance the tracking of financial metrics, allowing for
detailed and recent analysis of earnings and expenditures on a daily
basis.

