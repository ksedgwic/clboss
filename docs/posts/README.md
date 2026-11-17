# CLBOSS posts

Dated writeups on CLBOSS: what a release changes and why, how to
operate and debug it, and design notes.  Newest first.  Each post
starts with the date it was last revised and the versions it
applies to.

- 2026-11-16 [What's new in CLBOSS v0.17.3](2026-11-16-whats-new-in-0.17.3.md)
- 2026-10-13 [What's new in CLBOSS v0.17.2](2026-10-13-whats-new-in-0.17.2.md)
- 2026-09-29 [What's new in CLBOSS v0.17.1](2026-09-29-whats-new-in-0.17.1.md)
- 2026-09-09 [Watching CLBOSS rebalance](2026-09-09-watching-clboss-rebalance.md)
- 2026-09-08 [What's new in CLBOSS v0.17.0](2026-09-08-whats-new-in-0.17.0.md)

## Earlier writing

ZmnSCPxj, CLBOSS's original author, published a six-part design
series in 2022 at <https://zmnscpxj.github.io/clboss/index.html>:
the overall architecture, inbound liquidity via swaps, connection
handling, and channel candidate investigation, selection, and
creation.  The architecture part still describes CLBOSS.  The swap
and candidate parts predate the `askrene` migration and the
track-record ranking, and the rebalancing they refer to was
replaced in 0.17.0.
