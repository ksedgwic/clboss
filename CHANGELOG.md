# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [0.16.0] - 2026-04-21: "Darkness on the Edge of the Mempool"

### Added

- **Internal**:
  - Fee Monitor module records per-channel fee-setting context and
    statistics into SQLite (`feemon_peers`, `feemon_change_events`
    tables). ([#296])
  - C++20 coroutine support for cleaner async code. ([#272])
- **RPC**:
  - `clboss-feemon-history`: returns per-peer fee modifier history
    (baseline, size/balance/price multipliers, earnings). ([#296])
  - `clboss-feemon-peers`: returns peer node IDs with fee monitor data,
    with optional time window filtering. ([#296])
  - `clboss-earnings-history`: added "all" (by-node) aggregation mode.
    ([#292])
- **Contrib scripts**:
  - `fee-log-parser`: parses DEBUG-level logs into SQLite. ([#291])
  - `plot-fees`: plots per-peer fee time series. ([#291])
  - `plot-aggregate`: plots aggregate percentile summaries across
    peers. ([#291])
  - `plot-balance-price`, `plot-size-balance`, `plot-size-price`: fee
    modifier analysis visualizations. ([#295])
  - `feemon-validate`: validates fee monitor data integrity. ([#296])
- **Build**:
  - `install-versioned` Makefile target for fast roll-forward/roll-back.
    ([#289])
  - Code coverage reporting in CI. ([#283])
  - Clang C++20 build job in CI. ([#307])
  - Nix flake packaging improvements. ([#285])

### Changed

- `InvoicePayer` now uses CLN's native `decode` RPC instead of manual
  parsing. ([#301])

### Fixed

- `fundpsbt` deprecated integer argument handling. ([#288])
- `Initiator` module handles both old and new `listconfigs` RPC output
  formats. ([#300])
- Backtrace capture disabled under valgrind to fix `make check`
  failures. ([#279])
- Fixed duplicate earnings report sections in README. ([#277])
- Fixed outdated `setchannelfee` reference to `setchannel` in README.
  ([#290])
- Replaced deprecated `std::result_of` with `std::invoke_result_t` for
  C++20 compatibility (fixes build on FreeBSD 14+ / clang 18). ([#305])
- Added missing `#include <cstdint>` for clang strict mode. ([#307])
- Added `-lexecinfo` for FreeBSD in `configure.ac`. ([#307])
- Fixed uninitialized `ReservoirSampler::wsum` warning at `-O2`.
- Fixed crash on non-SRV DNS records (e.g. SOA) in `parse_dig_srv()`;
  removed defunct DNS seeds `lseed.bitcoinstats.com` and
  `lseed.darosior.ninja`. ([#309])

### Credits

Many thanks (!!) to contributors to this release:
- @JosephGoulden
- @lduchosal
- @raphaellueckl
- @smolting
- @tank-welder
- @ZmnSCPxj

## [0.15.1] - 2025-10-07: "Dancing in the Dark Liquidity"

### Fixed

- Fixed the release build CI workflow

## [0.15.0] - 2025-10-07: "Dancing in the Dark Liquidity"

### Added

- **RPC**:
  - Added the `clboss-feerates` command to report percentile thresholds,
    last observed feerate (perkw), and the current low/high judgment.
- **Configuration**:
  - Added `--clboss-min-nodes-to-process` to control how many nodes
    CLBOSS must know before proposing channels. Defaults: 800 (Bitcoin),
    100 (Testnet), 10 (other). Setting `-1` uses network-specific defaults.
  - Added `--clboss-max-rebalance-fee-ppm` to cap the fee for a single
    internal rebalance (honored by both JitRebalancer and
    EarningsRebalancer). Default is 1000 ppm (0.1%).
- **Contrib scripts**:
  - New `clboss-forwarding-stats` script.
  - New `recently-closed` helper.
  - New `sys_stats_report` to generate heartbeat status reports and
    append metrics (e.g., `utxo_msat`, `avail_msat`, `current_msat`,
    `avail_out`, `utxo_amount`) to a `STATS` file; includes a baseline
    checksum and appends fee data from `clboss-feerates`.
  - `clboss-earnings-history`: added `--csv-file`, `--graph-file` and
    `--bucket` (day|week|fortnight|month|quarter); estimates the last
    bucket based on remaining time.
  - `clboss-routing-stats`: added `--days` to limit the time window.
- **Docs**:
  - Updated diagrams (rebalancer flow, channel balancing orientation) and
    README/contrib docs; refreshed dependencies for contrib tooling.

### Changed

- Reduced the default `--clboss-max-rebalance-fee-ppm` from 5000 ppm
  (0.5%) to 1000 ppm (0.1%).

### Fixed

- `ChannelFinderByPopularity`: ignore our own node when sampling and
  enumerating peers ([#266]).
- Swaps: avoid blank addresses in the cache and clean any that slipped
  in; validate the initial Boltz claim destination address.

## [0.14.1] - 2024-12-05: "Hand at the Grindstone"

### Added

- **Contrib Script Enhancements**:
  - Added `--lightning-dir` option to the contrib scripts:
    - `clboss-earnings-history`
    - `clboss-recent-earnings`
    - `clboss-routing-stats`

    This allows users with non-default configurations to specify their
    `lightning-dir` when running these scripts. ([#243])

  - **Nix Support**:
    - Introduced `contrib-shell.nix` to facilitate running contrib
      scripts within Nix environments. Users can now execute
      `nix-shell contrib-shell.nix` and run any Python scripts in
      `contrib/`. ([#241])
    - Updated `contrib/README.md` with detailed instructions for
      Python dependencies installation, including a section on using
      Nix.

- **Stack Unwinding Support**:
  - Implemented `libunwind` for stack unwinding. This replaces the use
    of `backtrace()`, which is not available on Alpine Linux. This
    improves compatibility with Alpine and other systems lacking
    `backtrace()`. ([#245], [#249])
  - Replaced the use of `program_invocation_name` (only available on
    Linux) with a custom global variable to store the program name,
    improving portability to systems like FreeBSD and other Unix-like
    systems. ([#242])

- **Configurable Exception Backtrace Support**:
  - Added the `--disable-exception-backtrace` option to
    `configure`. This allows disabling the inclusion of backtrace
    information in exception wrappers. ([#256])
  - The `Util::BacktraceException` class now provides a no-op wrapper
    when exception backtraces are disabled via
    `--disable-exception-backtrace`. This ensures minimal overhead in
    configurations where backtraces are not needed. ([#256])

### Fixed

- **Build System**:
  - Fixed issues when building CLBOSS as a git submodule. ([#247], [#250])
  - Improved diagnostic messages for missing `commit_hash.h` in
    tarball builds. This helps users identify and resolve build issues
    when `commit_hash.h` is not present. ([#244]), [#251])

## [0.14.0] - 2024-09-25: "Hand at the Grindstone"

### Added

- **EarningsTracker Upgrade**: Upgraded `EarningsTracker` to a time
  bucket schema, allowing storage and access to earnings and
  expenditure data over specific time ranges. This prepares for future
  enhancements in balancing strategies based on time-based data. Note
  that this update includes automatic database schema changes;
  downgrading to previous versions will require manual database
  migration.

- **Exception Backtraces**: Added `Util::BacktraceException` which
  captures backtraces where an exception is thrown and then formats
  them for debugging when they are displayed with `what()`.  The
  backtraces are more useful if the following configuration is used:
  `./configure CXXFLAGS="-g -Og"` but this results in larger, less
  optimized binaries.

- **New Scripts in Contrib**:
  - `clboss-routing-stats`: A script that summarizes routing
    performance of channels, displaying PeerID, SCID, and Alias. It
    sorts channels by net fees (income - expenses), success per day,
    and age.
  - `clboss-earnings-history` and `clboss-recent-earnings`: Scripts to
    display historical and recent earnings.

  - Added `contrib/README.md` to provide information about the scripts
    and tools available in the `contrib` directory.
  - Introduced a Poetry project to manage Python dependencies in `contrib`.

- **Testing and Debugging Enhancements**:
  - Added `get_now()` and `mock_get_now()` functions to
    `EarningsTracker` and its tests to support time-based
    functionalities.
  - Implemented `Either::operator<<` and `Jsmn::Object::operator==` to
    facilitate debugging and writing test cases.
  - Factored `parse_json` into a `Jsmn::Object` static method to
    simplify test case generation using literal JSON.

### Changed

- **Build System**:
  - Updated `configure` to use the C++17 standard, fixing compilation
    issues on platforms like Raspiblitz.
  - Improved `commit_hash.h` dependencies and generation to ensure
    correct regeneration when the development tree is modified.

- **Contrib Script Enhancements**:
  - Generalized network parameter handling in `clboss-routing-stats`
    to support multiple networks.
  - Updated `clboss-routing-stats` to utilize an alias cache for
    better performance.

### Fixed

- **Testing**:
  - Increased the timeout for `jsmn/test_performance` tests to prevent
    premature failures during testing.

- **Logging Improvements**:
  - Inserted exception `what()` values into logging messages to
    enhance debugging output and provide more detailed error
    information.

- **Miscellaneous**:
  - Resolved issues with the regeneration of `commit_hash.h` when the
    development tree is altered by Git operations.

### Added

## [0.13.3] - 2024-08-09: "Blinded by the Light"

This point release fixes an important bug by restoring the earned fee
information in CLBOSS.

### Added

- The version string is now logged on startup and in the
  `clboss-status` output ([#205]).
- Added an earnings_tracker diagram.

### Fixed

- The `ForwardFeeMonitor` (and subsequently the `EarningsTracker`) have
  restored ability see fee income ([#222], [#223]).
- A possible vector out of bounds access was removed ([#219]).
- Added totals to clboss-status offchain_earnings_tracker ([#223]).

## [0.13.2] - 2024-07-18: "Bwahaha's Dominion"

### Added

- Added `signet` support ([#148]).
- Updated the seeds list ([#208], 
- Added module diagrams for channel creation, offchain to onchain
  swaps, and channel balancing ([#200], [#203]).

### Changed

- testnet: Reduce the `min_nodes_to_process` because testnet is shrinking ([#209]).
- Improve listpeers handling diagnostics ([#214], [#215]).
- Improve Initialization of OnchainFeeMonitor with Conservative
  Synthetic History ([#210]).

### Fixed

- Converted deprecated listpeer usage to listpeerchannels ([#213], [#198]).
- Recognize `--developer` CLI flag and don't exit giving usage ([#185], [#216])).

## [0.13.1] - 2024-04-16: "E Street Fix"

### Fixed

- CLN `v24.02` deprecated the RPC `msatoshi` fields which needed to be
  converted to `amount_msat` instead.  This caused channel candidates
  to not be found ([#189]) (and maybe other problems).  Fixed in
  ([#190]).
- CLN `v24.02` deprecated the RPC `private` field in the channel info
  RPC data because private channels are no longer present.  Remove
  references to the field because we only want to skip these channels
  anyway.  Fixes ([192])

### Changed

- The minimum number of network nodes seen before initiating certain
  actions is 800 in the bitcoin network.  ([#173]) changes this
  threshold for the testnet (300) and other networks (10).  The new
  thresholds should allow CLBOSS to act when there are fewer available
  nodes.  The bitcoin limit remains 800.

## [0.13] - 2023-09-08: "Born to Run"

### Added

- Continuous Integration (CI) for pull requests!
- Support string "id" fields in the plugin interface.
- Enable SQLITE3 extended error codes.

### Changed

- Disable compiling debug information by default; if you need this,
  explicitly include `-g` in your `configure` command, like so:
  `./configure CXXFLAGS="-O2 -g"`.  This reduces binary size by 20x.
- Avoid parameters/commands deprecated in Core Lightning 0.11.0.

### Fixed

- Can now handle JSON-RPC amounts in either the old convention
  (string, "msat" suffix) or the post 23.05 convention (json number). ([#157], [#164])
- Fixed non-integer blockheights (testnet) ([#170])
- Upgraded libraries and compiler to fix build. ([#169])


## Prior `ChangeLog` entries [formatting change]

- Support string "id" fields in the plugin interface.
- Disable compiling debug information by default; if you need this, explicitly include `-g` in your `configure` command, like so: `./configure CXXFLAGS="-O2 -g"`.  This reduces binary size by 20x.
- Enable SQLITE3 extended error codes.
- Avoid parameters/commands deprecated in Core Lightning 0.11.0.

0.13A
- Disable `InitialRebalancer`, as it is not based on economic rationality.
- You can now disable rebalancing to or from specific peers by using `clboss-unmanage` with the key `balance`.
- Use a single giant `listchannels` call in `FeeModderBySize`.  This should now make CLBOSS usable on nodes with >100 channels.
- Limit the number of concurrent RPC calls we make, to prevent overloading the poor Core Lightning daemon.
- Make `PeerCompetitorFeeMonitor::Surveyor` more efficient by using a new parameter for `listchannels` that was introduced in C-Lightning 0.10.1.  Fall back to the old inefficient algo if the C-Lightning node is < 0.10.1.

0.12 Not Completely Useless
0.11E
- Fix a bug which *removed* `--clboss-min-onchain` instead of adding `--clboss-min-channel` and `--clboss-max-channel`.  LOL.

0.11D
- We now check dowsed channel sizes during preinvestigation and investigation as well, making sure the minimum channel size is respected.
- Add `--clboss-min-channel` and `--clboss-max-channel` settings.
- Make sure `ChannelFinderByPopularity` becomes aggressive at least once, to handle the case where the node was previously (poorly?) managed by a human and might not have good liquidity to the network.
- If our total funds is increased by +25% or more, have `ChannelFinderByPopularity` become more aggressive.
- Support `--clboss-zerobasefee=<require|allow|disallow>`.
- Fix incompatibility with C-Lightning 0.11.x by explicitly using "style": "tlv" instead of "legacy".
- Record offchain-to-onchain swaps, accessible via new `clboss-swaps` command.

0.11C
- Disable `ChannelComplainerByLowSuccessPerDay` for now.
- Adjust our judgment of "low onchain fee" downward (i.e. cheaper) slightly, from 25% +/-5% to 20% +/-3%.
- `ActiveProber` now only does a 2-hop probe always.
- `ChannelComplainerByLowSuccessPerDay` logs a little more on debug prints.
- Tweak parameters for auto-close slightly, being more lenient.
- Fix FreeBSD compile.

0.11B
- `ActiveProber` now also has a background cleaner of its payments.
- `ChannelCreationDecider` now holds off on creating channels if the onchain amount is small relative to all your funds and is small for a "large" channel (~0.16777215 BTC).  This should prevent CLBOSS from making lots of 10mBTC channels when your node is well-funded.
- CLBOSS can now close bad channels.  Enable this ***EXPERIMENTAL*** feature by passing `--clboss-auto-close=true` option to `lightningd`.  If enabled, the `clboss-unmanage` command can disable this for particular peers using the `close` key.
- Remove `libsodium` library, instead use SHA256 implementation from Bitcoin and our own code for basic securtiy issues.
- Add `clboss-unmanage` command to suppress certain aspects of auto-management.
- Fix `InitialRebalancer` bug (introduced in 0.11A) which completely disables it instead of throttling it.

0.11A
- Make `FundsMover` much less willing to pay extra for more private randomized routes.
- New `EarningsRebalancer` is now the primary rebalancer to replace the role of `InitialRebalancer` in previous releases; it will base its rebalancing decisions on earnings of each channel.
- `InitialRebalancer` will now limit how much it will spend on rebalances, as it is intended for the *initial* rebalancing of new channels.
- `FundsMover` is now more parsimonious about its fee budget when it splits moved funding attempts.
- Avoid making multiple channels to nodes with the same IP bin.
- Fix MacOS compile.
- Use `dig -v` to check for `dig` install, instead of `dig localhost`, as the latter may trigger a "real" lookup that will inevitably fail.

0.10 Made of Explodium
0.9A
- Avoid `DELETE ... ORDER BY`, which might not be enabled on the SQLITE3 available on some systems.
- Fix a roundoff error with command `id`s, which would lead to `clboss` eventually crashing after a few days or weeks.

0.8 Facepalm of Doom
0.7D
- Fix latent `printf`-formatting bugs in `SendpayResultMonitor`, which would crash on 32-bit systems.

0.7C
- `FundsMover` now deletes its failing payments immediately instead of letting them languish in your db until the cleanup process gets to them.

0.7B
- Ensure `InitialRebalancer` does not put the destination node at the edge of triggering `InitialRebalancer` again in the next cycle, which was causing multiple rebalances in sequence.
- New option `--clboss-min-onchain=<satoshi>` to indicate how much to leave onchain; defaults to 30000 satoshi, which is suggested to leave onchain in preparation for anchor commitments, but you can leave more (or less) now.
- Document `clboss-status` and `clboss-externpay` commands.
- New commands `clboss-ignore-onchain` and `clboss-notice-onchain` let you temporarily manage onchain funds manually.
- Change onchain fee judgment to use percentile based on the previous 2 weeks of feerates.
- Support MacOS compilation, also checked FreeBSD compilation still works.
- Correct calculation of spendable vs receivable in `NodeBalanceSwapper`.

0.7A
- Properly consider direction of flow when estimating capacities of nodes.
- Properly rebalance channels greater than 42.94mBTC payment limit.
- Use `payment_secret` in rebalances.
- Work around a timing bug in Tor SOCKS5 implementation.
- CLBOSS can now be started and stopped with the `lightningd` `plugin` command.
- Do not use `proxy` if `always-use-proxy` is not `true`.
- New `ChannelFinderByEarnedFee` module proposes peers of our most lucrative peers, to improve alternate routes to popular destinations.

0.6 Nice Job Breaking It, Hero!
0.5E
- Remove busy-wait loop in `FeeModderBySize`.

0.5D
- Tone down `FundsMover` payment cleanup.
- Batch up RPC socket response parsing.

0.5C
- `FundsMover` now has a backup process to clean up its payments.
- `ChannelFinderByListpays` now ignores self-payments instead of possibly proposing self.
- Optimize traversing JSON results for channel finders.
- Reduce processing load when printing really long RPC logs.
- Correctly handle sudden death of `lightningd` process.
- Really, do not delay response to `init`, for reals.

0.5B
- Do not delay response to `init`.

0.5A
- Handle `rpc_command` specially for better RPC response times even when CLBOSS is busy.
- Make compilable on FreeBSD.
- Print more debug logs for internet connection monitoring.
- Limit resources used by rebalancing attempts.
- Long-running processes (channel finders, peer fee competitor measuring) now print progress reports.
- Lowered execution priority of RPC socket reading and parsing, hopefully this will make us more responsive to our hooks.

0.4 Failed a Spot Check
0.3B
- `ChannelFinderByPopularity` now reduces its participation instead of not participating if we have many channels already.
- Channel finders now ensure they only run once even if multiple triggers occur while they are running.

0.3A
- Fixed missing initializations and some checks.
- Fixed build errors in Debian.

0.2 TV Tropes Will Ruin Your Life

0.1A Initial Alpha Release

Local Variables:
mode: markdown
End:
