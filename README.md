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
* Rebalance open channels by self-payment (including JIT rebalancer).
* Set forwarding fees so that they're competitive to other nodes

You can read more information about CLBOSS here:
https://zmnscpxj.github.io/clboss/index.html
As of this release, this page is a work in progress, stay tuned
for updates!

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

From an [official source release](https://github.com/ZmnSCPxj/clboss/releases), just:

    ./configure && make
    sudo make install # or su first, then make install

This will install `clboss` as a standard executable, usually in
`/usr/local/bin/` by default.
You will then need to modify your `lightning.conf` to add the
path to `which clboss` as a `plugin` or `important-plugin` of
`lightningd`.

Usually, autotools-based projects like CLBOSS will default
to using `-g -O2` for compilation flags, where `-g` causes
the compiler to include debug info.
CLBOSS changes this default to `-O2` so that users by default
get a binary without debug symbols (a binary with debug symbols
would be 20x larger!), but if it matters to you, you can
override the CLBOSS default via `CXXFLAGS`, such as:

    ./configure CXXFLAGS="-g -O2"  # or whatever flags you like
    ./configure CXXFLAGS="-g -Og"  # recommended for debugging

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

### Contributed Utilities

There are some contributed utilities in the `contrib` directory.  See the
`contrib/README.md` for more details.


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

### `clboss-externpay`

If CLBOSS is managing a node for a custodial service, then you should
`decodepay` the invoices provided by clients whose funds you are
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
the normal Core Lightning `setchannelfee` command.

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

### `clboss-recent-earnings`, `clboss-earnings-history`

As of CLBOSS version [TBD], earnings and expenditures are tracked on a daily basis.
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
      provided. Without this argument, the values are accumulated
      across all peers.
  - **Output**: 
    - The history consists of an array of records showing the earnings
      and expenditures for each day.
    - The history includes an initial record with a time value of 0,
      which contains any legacy earnings and expenditures collected by
      CLBOSS before daily tracking was implemented.

These commands enhance the tracking of financial metrics, allowing for
detailed and recent analysis of earnings and expenditures on a daily
basis.

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

Specify the value in satoshis without adding any unit
suffix, e.g.

    lightningd --clboss-min-channel=1000000

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
      across all nodes.
  - **Output**: 
    - The history consists of an array of records showing the earnings
      and expenditures for each day.
    - The history includes an initial record with a time value of 0,
      which contains any legacy earnings and expenditures collected by
      CLBOSS before daily tracking was implemented.

These commands enhance the tracking of financial metrics, allowing for
detailed and recent analysis of earnings and expenditures on a daily
basis.
