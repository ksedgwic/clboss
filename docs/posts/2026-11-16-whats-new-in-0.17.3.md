# What's new in CLBOSS v0.17.3

2026-11-16.  Applies to CLBOSS v0.17.3 with the `xrebalance` plugin
v0.4.5 or later, on Core Lightning v26.04 or later.

CLBOSS v0.17.3 is a maintenance release for swaps.  The requirements
are those of v0.17.0.

## In short

- Swaps on mainnet are not available at present.  Boltz, the swap
  provider compiled into CLBOSS, suspended all swaps on 2026-08-03.
  A mainnet node gets no quotation and pays nothing.
- Swaps use the provider's API v2 with taproot lockups.  The
  preimage leaves the node only after our own node has the lockup
  three blocks deep.
- A swap invoice is checked before it is paid: its payment hash and
  its amount must be the ones the swap asked for.
- A swap request that gets no answer is restarted after 30 minutes
  instead of blocking the swaps after it.
- A peer with several channels is handled correctly in the fee by
  balance, in probes and in `clboss-auto-close`.
- A new signet node can find peers again.
- `make check` passes under valgrind on Debian and Ubuntu.

## Upgrading from v0.17.2

Finish or wait out any swap in flight before upgrading, and before
downgrading again: a swap created by one version cannot be claimed
by the other.  Then install and restart.  No option was added or
removed.

## Details

The [release notes](https://github.com/ksedgwic/clboss/releases/tag/v0.17.3)
describe each change.  The [CHANGELOG](../../CHANGELOG.md) has the
full text.

## Credits

- The contributor who sent the API v2 draft provided the first
  version of the Boltz API v2 code, with taproot reverse swaps and
  MuSig2 claims.
- [Mohil-Ahuja](https://github.com/Mohil-Ahuja) contributed the
  three fixes for several channels to one peer, the balance-sample
  fix in `contrib/clboss-channel-sizing` and the replacement signet
  seeds.
- [Amperstrand](https://github.com/Amperstrand) reported that a swap
  invoice was paid without a check of its payment hash.
- [tsjk](https://github.com/tsjk) runs the signet swap backend that
  the swap changes were tested against.
