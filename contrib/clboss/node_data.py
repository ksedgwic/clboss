"""Node data for the contrib liquidity tools, with record and replay.

The loaders here read what clboss-channel-sizing and
clboss-forwarding-stats read: the open channels, the hourly balance
samples CLBOSS keeps per peer, the settled and refused forwards over a
window, and CLBOSS's per-peer earnings day buckets.  Each loader takes a
Store: live, it calls lightning-cli and, with --record, writes what it
got; with --replay it reads those files instead, so a run can be
repeated on another machine, or later, from the data it actually used.

Record layout (one directory):
  meta.json                 since, before, recorded_at (unix seconds)
  channels.json             listpeerchannels
  closed.json               listclosedchannels
  aliases.json              peer id -> alias
  feemon/<peer id>.json     clboss-feemon-history <peer> <since> <before>
  earnings-all.json         clboss-earnings-history all
  forwards-<status>.json    the forwards of that status in the window
"""

import json
import os
import subprocess
import sys
import time


def run_lightning_cli_command(lightning_dir, network_option, command, *args,
                              keyword=False):
    argv = ["lightning-cli", network_option]
    if lightning_dir:
        argv.append(f"--lightning-dir={lightning_dir}")
    if keyword:
        argv.append("-k")
    argv += [command, *args]
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                check=True)
        return json.loads(result.stdout)
    except subprocess.CalledProcessError as e:
        print(f"Command '{argv}' failed with error: {e}", file=sys.stderr)
        print(e.stderr, file=sys.stderr)
    except json.JSONDecodeError as e:
        print(f"Failed to parse JSON from command '{argv}': {e}",
              file=sys.stderr)
    return None


def msat(value):
    """Amounts arrive as ints or 'Nmsat' strings."""
    if value is None:
        return 0
    if isinstance(value, (int, float)):
        return int(value)
    return int(str(value).replace("msat", ""))


def add_network_args(parser):
    parser.add_argument("--mainnet", action="store_true",
                        help="Run on mainnet")
    parser.add_argument("--testnet", action="store_true",
                        help="Run on testnet")
    parser.add_argument("--signet", action="store_true", help="Run on signet")
    parser.add_argument("--regtest", action="store_true",
                        help="Run on regtest")
    parser.add_argument("--network", help="Set the network explicitly")
    parser.add_argument("--lightning-dir", help="lightning data location")


def add_capture_args(parser):
    parser.add_argument("--record", metavar="DIR",
                        help="Run live and save the data the run used "
                             "under DIR.")
    parser.add_argument("--replay", metavar="DIR",
                        help="Run from the data saved by --record instead "
                             "of lightning-cli; the window is the recorded "
                             "one.")


def network_option(args):
    if args.network:
        return f"--network={args.network}"
    if args.testnet:
        return "--network=testnet"
    if args.signet:
        return "--network=signet"
    if args.regtest:
        return "--network=regtest"
    return "--network=bitcoin"


def lightning_dir(args):
    if not args.lightning_dir:
        return None
    if not os.path.isdir(args.lightning_dir):
        raise SystemExit(f'"{args.lightning_dir}" is not a valid directory')
    return args.lightning_dir


class Cli:
    def __init__(self, lightning_dir, network_option):
        self.lightning_dir = lightning_dir
        self.network_option = network_option

    def __call__(self, command, *args, keyword=False):
        return run_lightning_cli_command(self.lightning_dir,
                                         self.network_option, command, *args,
                                         keyword=keyword)


class Store:
    """Where a loader gets its data: lightning-cli, a record directory
    being written, or a replay directory being read."""

    def __init__(self, cli, record=None, replay=None):
        if record and replay:
            raise SystemExit("--record and --replay are exclusive")
        self.cli = cli
        self.record = record
        self.replay = replay
        if record:
            os.makedirs(os.path.join(record, "feemon"), exist_ok=True)
        if replay and not os.path.isdir(replay):
            raise SystemExit(f"--replay: {replay} is not a directory")

    def _path(self, name):
        return os.path.join(self.replay or self.record, name)

    def read(self, name):
        path = self._path(name)
        if not os.path.exists(path):
            print(f"replay: {name} is missing from {self.replay}",
                  file=sys.stderr)
            return None
        with open(path) as f:
            return json.load(f)

    def write(self, name, data):
        if not self.record:
            return
        with open(self._path(name), "w") as f:
            json.dump(data, f)

    def fetch(self, name, call):
        """The data under name: from the replay directory, or from call()
        (recorded when recording)."""
        if self.replay:
            return self.read(name)
        data = call()
        if data is not None:
            self.write(name, data)
        return data


def window(store, args, now=None):
    """(since, before) for the run.  Live: from --days/--since/--before,
    recorded under meta.json.  Replay: the recorded window, whatever the
    flags say (the data covers only that)."""
    if store.replay:
        meta = store.read("meta.json") or {}
        if "since" not in meta or "before" not in meta:
            raise SystemExit("replay: meta.json has no window")
        return int(meta["since"]), int(meta["before"])
    now = now or int(time.time())
    before = args.before or now
    since = args.since or before - args.days * 86400
    if since >= before:
        raise SystemExit("--since must be earlier than --before")
    store.write("meta.json", {"since": since, "before": before,
                              "recorded_at": now})
    return since, before


# channels

def open_channels(store):
    """Open channels by short channel id: peer, capacity, our side,
    the peer's fee toward us, who opened it, whether the peer is
    connected."""
    data = store.fetch("channels.json", lambda: store.cli("listpeerchannels"))
    channels = {}
    for ch in (data or {}).get("channels", []):
        scid = ch.get("short_channel_id")
        if not scid:
            continue
        if ch.get("state") not in ("CHANNELD_NORMAL",
                                   "CHANNELD_AWAITING_SPLICE"):
            continue
        remote = (ch.get("updates") or {}).get("remote") or {}
        cap = msat(ch.get("total_msat"))
        ours = msat(ch.get("to_us_msat"))
        channels[scid] = {
            "peer_id": ch["peer_id"],
            "cap": cap,
            "ours": ours,
            "inbound": cap - ours,
            "peer_ppm": remote.get("fee_proportional_millionths"),
            "connected": bool(ch.get("peer_connected")),
            "opener": ch.get("opener"),
            "channel_id": ch.get("channel_id"),
        }
    return channels


def closed_channels(store):
    """Short channel id -> peer id for the channels CLN remembers as
    closed, so forwards through a channel since closed, or replaced by a
    resize, still count for its peer."""
    data = store.fetch("closed.json",
                       lambda: store.cli("listclosedchannels"))
    out = {}
    for ch in (data or {}).get("closedchannels", []):
        scid = ch.get("short_channel_id")
        if scid and ch.get("peer_id"):
            out[scid] = ch["peer_id"]
    return out


def aliases(store, peer_ids):
    """Alias per peer, through the alias cache; the id itself when the
    peer announces none."""
    def live():
        from clboss.alias_cache import lookup_alias
        out = {}
        for peer_id in peer_ids:
            alias = lookup_alias(run_lightning_cli_command,
                                 store.cli.lightning_dir,
                                 store.cli.network_option, peer_id)
            out[peer_id] = alias or peer_id
        return out
    data = store.fetch("aliases.json", live) or {}
    # a peer without an alias prints as the first 12 hex digits of its
    # id (--ids on the tools prints the whole id)
    return {p: (data.get(p) if data.get(p) and data.get(p) != p
                else p[:12]) for p in peer_ids}


# balance samples

def feemon_history(store, peer_id, since, before):
    """CLBOSS's fee monitor records for the peer in the window: hourly
    samples with balance_our_msat and balance_total_msat (the peer's
    channels summed)."""
    def live():
        return store.cli("clboss-feemon-history", peer_id, str(since),
                         str(before))
    data = store.fetch(os.path.join("feemon", f"{peer_id}.json"), live)
    return (data or {}).get("history", [])


# earnings

def earnings_history_all(store):
    """CLBOSS's per-peer day buckets: in/out earnings, expenditures,
    forwarded and rebalanced volumes, keyed by node and bucket_time."""
    data = store.fetch("earnings-all.json",
                       lambda: store.cli("clboss-earnings-history", "all"))
    return (data or {}).get("history", [])


# forwards

def forwards_last_index(cli):
    """Current created_index of the forwards table (wait with nextvalue=0
    returns immediately)."""
    data = cli("wait", "forwards", "created", "0")
    if data and "created" in data:
        return int(data["created"])
    return None


def forward_at(cli, index):
    data = cli("listforwards", "index=created", f"start={index}", "limit=1",
               keyword=True)
    fwds = data.get("forwards", []) if data else []
    return fwds[0] if fwds else None


def first_index_since(cli, since, last_index):
    """Smallest created_index whose forward was received at or after
    since."""
    lo, hi = 1, last_index + 1
    while lo < hi:
        mid = (lo + hi) // 2
        fwd = forward_at(cli, mid)
        if fwd is None:
            hi = mid
        elif fwd.get("received_time", 0) < since:
            lo = mid + 1
        else:
            hi = mid
    return lo


def iter_forwards_live(cli, status, start, before, page):
    """Forwards of one status from created_index start, received before
    'before'."""
    while True:
        data = cli("listforwards", f"status={status}", "index=created",
                   f"start={start}", f"limit={page}", keyword=True)
        fwds = data.get("forwards", []) if data else []
        if not fwds:
            return
        for fwd in fwds:
            if fwd.get("received_time", 0) >= before:
                return
            yield fwd
        start = fwds[-1]["created_index"] + 1


def forwards(store, status, since, before, page=10000):
    """The forwards of one status received in the window, as a list.
    Live, the walk starts at the first index in the window (binary
    search over the created index) and reads pages of `page`."""
    name = f"forwards-{status}.json"
    if store.replay:
        return (store.read(name) or {}).get("forwards", [])
    last_index = forwards_last_index(store.cli)
    if last_index is None:
        print("could not read the forwards index", file=sys.stderr)
        return []
    start = first_index_since(store.cli, since, last_index)
    rows = [f for f in iter_forwards_live(store.cli, status, start, before,
                                         page)
            if f.get("received_time", 0) >= since]
    store.write(name, {"forwards": rows})
    return rows
