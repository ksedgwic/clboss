#!/usr/bin/env python3
import argparse
import json
import os
import re
import sqlite3
import sys


LINE_RE = re.compile(
    r"^(?:\d+:)?(\d{4}-\d{2}-\d{2}T[^ ]+)\s+\w+\s+[^:]+: (.*)$"
)
BASELINE_RE = re.compile(r"PeerCompetitorFeeMonitor: Weighted median fees: (.*)$")
BASELINE_ENTRY_RE = re.compile(r"([0-9a-f]{66})\(b = (\d+), p = (\d+)\)")
SIZE_RE = re.compile(
    r"FeeModderBySize: Peer ([0-9a-f]{66}) has .* Multiplier: ([0-9.]+)"
)
BALANCE_MOVED_RE = re.compile(
    r"FeeModderByBalance: Peer ([0-9a-f]{66}) moved from bin (\d+) to bin (\d+) "
    r"of (\d+) due to balance (\d+)msat / (\d+)msat: ([0-9.]+)"
)
BALANCE_SET_RE = re.compile(
    r"FeeModderByBalance: Peer ([0-9a-f]{66}) set to bin (\d+) of (\d+) "
    r"due to balance (\d+)msat / (\d+)msat: ([0-9.]+)"
)
PRICE_RE = re.compile(
    r"FeeModderByPriceTheory: ([0-9a-f]{66}): level = (-?\d+), mult = ([0-9.]+)"
)
RPC_IN_LISTCHANNELS_RE = re.compile(r"^Rpc in: listchannels .*=> (.*)$")
RPC_IN_GETROUTE_RE = re.compile(r"^Rpc in: getroute .*=> (.*)$")
RPC_OUT_SENDPAY_RE = re.compile(r"^Rpc out: sendpay (.*)$")
SHORT_CHANNEL_ID_RE = re.compile(r'"short_channel_id"\s*:\s*"([^"]+)"')
SOURCE_RE = re.compile(r'"source"\s*:\s*"([0-9a-f]{66})"')
DESTINATION_RE = re.compile(r'"destination"\s*:\s*"([0-9a-f]{66})"')
ROUTE_ID_RE = re.compile(r'"id"\s*:\s*"([0-9a-f]{66})"')
ROUTE_CHANNEL_RE = re.compile(r'"channel"\s*:\s*"([^"]+)"')
OBJECT_RE = re.compile(r"\{[^{}]*\}")


def parse_line(line):
    line = line.rstrip("\n")
    m = LINE_RE.match(line)
    if not m:
        return None
    ts, msg = m.group(1), m.group(2)
    return ts, msg, line


def ensure_schema(conn):
    cur = conn.cursor()
    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS fee_change_events (
            id INTEGER PRIMARY KEY,
            ts TEXT NOT NULL,
            peer TEXT NOT NULL,
            scid TEXT,
            baseline_base INTEGER,
            baseline_ppm INTEGER,
            size_mult REAL,
            balance_mult REAL,
            price_level INTEGER,
            price_mult REAL,
            mult_product REAL,
            est_base INTEGER,
            est_ppm INTEGER
        )
        """
    )
    cols = {row[1] for row in cur.execute("PRAGMA table_info(fee_change_events)")}
    if "price_level" not in cols:
        cur.execute("ALTER TABLE fee_change_events ADD COLUMN price_level INTEGER")
    conn.commit()


def update_state(state, peer, key, value):
    if peer not in state:
        state[peer] = {}
    state[peer][key] = value


def compute_estimate(state, peer):
    s = state.get(peer, {})
    base = s.get("baseline_base")
    ppm = s.get("baseline_ppm")
    if base is None or ppm is None:
        return None

    size_mult = s.get("size_mult")
    balance_mult = s.get("balance_mult")
    price_mult = s.get("price_mult")
    if size_mult is None or balance_mult is None or price_mult is None:
        return {
            "baseline_base": base,
            "baseline_ppm": ppm,
            "size_mult": size_mult,
            "balance_mult": balance_mult,
            "price_mult": price_mult,
            "mult_product": None,
            "est_base": None,
            "est_ppm": None,
        }

    mult_product = size_mult * balance_mult * price_mult

    est_base = int(round(base * mult_product))
    est_ppm = int(round(ppm * mult_product))

    if est_ppm == 0:
        est_ppm = 1

    return {
        "baseline_base": base,
        "baseline_ppm": ppm,
        "size_mult": size_mult,
        "balance_mult": balance_mult,
        "price_mult": price_mult,
        "mult_product": mult_product,
        "est_base": est_base,
        "est_ppm": est_ppm,
    }


class Output:
    def __init__(self, conn=None, commit_interval=1000):
        self.conn = conn
        self.cur = conn.cursor() if conn else None
        self.commit_interval = commit_interval
        self.pending_writes = 0

    def commit(self):
        if self.conn:
            self.conn.commit()

    def insert_fee_change(self, ts, peer, snapshot):
        if self.cur:
            self.cur.execute(
                """
                INSERT INTO fee_change_events (
                    ts, peer, scid, baseline_base, baseline_ppm,
                    size_mult, balance_mult, price_level, price_mult, mult_product,
                    est_base, est_ppm
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    ts,
                    peer,
                    snapshot["scid"],
                    snapshot["baseline_base"],
                    snapshot["baseline_ppm"],
                    snapshot["size_mult"],
                    snapshot["balance_mult"],
                    snapshot["price_level"],
                    snapshot["price_mult"],
                    snapshot["mult_product"],
                    snapshot["est_base"],
                    snapshot["est_ppm"],
                ),
            )
            self.pending_writes += 1
            if self.pending_writes % self.commit_interval == 0:
                self.conn.commit()
        else:
            sys.stdout.write(
                "{ts},{peer},{scid},{baseline_base},{baseline_ppm},{size_mult},"
                "{balance_mult},{price_level},{price_mult},{mult_product},"
                "{est_base},{est_ppm}\n".format(
                    ts=ts,
                    peer=peer,
                    scid=snapshot["scid"],
                    baseline_base=snapshot["baseline_base"],
                    baseline_ppm=snapshot["baseline_ppm"],
                    size_mult=snapshot["size_mult"],
                    balance_mult=snapshot["balance_mult"],
                    price_level=snapshot["price_level"],
                    price_mult=snapshot["price_mult"],
                    mult_product=snapshot["mult_product"],
                    est_base=snapshot["est_base"],
                    est_ppm=snapshot["est_ppm"],
                )
            )


def process_lines(lines, output):
    state = {}
    for line in lines:
        parsed = parse_line(line)
        if not parsed:
            continue
        ts, msg, raw_line = parsed

        m = RPC_IN_LISTCHANNELS_RE.match(msg)
        if m:
            payload = m.group(1)
            try:
                data = json.loads(payload)
            except json.JSONDecodeError:
                data = None
            if data and "channels" in data:
                channels = data["channels"]
                for ch in channels:
                    scid = ch.get("short_channel_id")
                    source = ch.get("source")
                    dest = ch.get("destination")
                    if scid and source:
                        update_state(state, source, "scid", scid)
                    if scid and dest:
                        update_state(state, dest, "scid", scid)
            else:
                for obj in OBJECT_RE.findall(msg):
                    scid_m = SHORT_CHANNEL_ID_RE.search(obj)
                    source_m = SOURCE_RE.search(obj)
                    dest_m = DESTINATION_RE.search(obj)
                    scid = scid_m.group(1) if scid_m else None
                    source = source_m.group(1) if source_m else None
                    dest = dest_m.group(1) if dest_m else None
                    if scid and source:
                        update_state(state, source, "scid", scid)
                    if scid and dest:
                        update_state(state, dest, "scid", scid)
            continue

        m = RPC_IN_GETROUTE_RE.match(msg)
        if m:
            payload = m.group(1)
            try:
                data = json.loads(payload)
            except json.JSONDecodeError:
                data = None
            if data and "route" in data:
                route = data["route"]
                for hop in route:
                    peer = hop.get("id")
                    scid = hop.get("channel")
                    if peer and scid:
                        update_state(state, peer, "scid", scid)
            else:
                for obj in OBJECT_RE.findall(msg):
                    peer_m = ROUTE_ID_RE.search(obj)
                    scid_m = ROUTE_CHANNEL_RE.search(obj)
                    peer = peer_m.group(1) if peer_m else None
                    scid = scid_m.group(1) if scid_m else None
                    if peer and scid:
                        update_state(state, peer, "scid", scid)
            continue

        m = RPC_OUT_SENDPAY_RE.match(msg)
        if m:
            payload = m.group(1)
            try:
                data = json.loads(payload)
            except json.JSONDecodeError:
                data = None
            if data and "route" in data:
                route = data["route"]
                for hop in route:
                    peer = hop.get("id")
                    scid = hop.get("channel")
                    if peer and scid:
                        update_state(state, peer, "scid", scid)
            else:
                for obj in OBJECT_RE.findall(msg):
                    peer_m = ROUTE_ID_RE.search(obj)
                    scid_m = ROUTE_CHANNEL_RE.search(obj)
                    peer = peer_m.group(1) if peer_m else None
                    scid = scid_m.group(1) if scid_m else None
                    if peer and scid:
                        update_state(state, peer, "scid", scid)
            continue

        m = BASELINE_RE.search(msg)
        if m:
            entries = BASELINE_ENTRY_RE.findall(m.group(1))
            for peer, base_s, ppm_s in entries:
                base = int(base_s)
                ppm = int(ppm_s)
                update_state(state, peer, "baseline_base", base)
                update_state(state, peer, "baseline_ppm", ppm)
                est = compute_estimate(state, peer)
                snapshot = {
                    "baseline_base": base,
                    "baseline_ppm": ppm,
                    "scid": state.get(peer, {}).get("scid"),
                    "size_mult": state.get(peer, {}).get("size_mult"),
                    "balance_mult": state.get(peer, {}).get("balance_mult"),
                    "price_level": state.get(peer, {}).get("price_level"),
                    "price_mult": state.get(peer, {}).get("price_mult"),
                    "mult_product": est.get("mult_product") if est else None,
                    "est_base": est.get("est_base") if est else None,
                    "est_ppm": est.get("est_ppm") if est else None,
                }
                output.insert_fee_change(ts, peer, snapshot)
            continue

        m = SIZE_RE.search(msg)
        if m:
            peer, mult_s = m.group(1), m.group(2)
            mult = float(mult_s)
            update_state(state, peer, "size_mult", mult)
            continue

        m = BALANCE_MOVED_RE.search(msg)
        if m:
            peer, prev_bin_s, bin_s, num_bins_s, to_us_s, total_s, mult_s = m.groups()
            mult = float(mult_s)
            update_state(state, peer, "balance_mult", mult)
            continue

        m = BALANCE_SET_RE.search(msg)
        if m:
            peer, bin_s, num_bins_s, to_us_s, total_s, mult_s = m.groups()
            mult = float(mult_s)
            update_state(state, peer, "balance_mult", mult)
            continue

        m = PRICE_RE.search(msg)
        if m:
            peer, level_s, mult_s = m.groups()
            mult = float(mult_s)
            level = int(level_s)
            update_state(state, peer, "price_level", level)
            update_state(state, peer, "price_mult", mult)
            continue

    output.commit()


def open_lines(paths):
    if not paths:
        for line in sys.stdin:
            yield line
        return

    for path in paths:
        if os.path.isdir(path):
            for root, _, files in os.walk(path):
                for name in files:
                    full = os.path.join(root, name)
                    with open(full, "r", encoding="utf-8", errors="replace") as f:
                        for line in f:
                            yield line
        else:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    yield line


def main():
    parser = argparse.ArgumentParser(
        description="Parse CLBOSS fee-setting logs into sqlite."
    )
    parser.add_argument(
        "--db",
        help="Path to sqlite database (created if missing).",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="Log files or directories (reads stdin if omitted).",
    )
    args = parser.parse_args()

    if args.db:
        conn = sqlite3.connect(args.db)
        ensure_schema(conn)
        output = Output(conn=conn)
        process_lines(open_lines(args.paths), output)
        conn.close()
        return

    sys.stdout.write(
        "ts,peer,scid,baseline_base,baseline_ppm,size_mult,balance_mult,"
        "price_level,price_mult,mult_product,est_base,est_ppm\n"
    )
    output = Output()
    process_lines(open_lines(args.paths), output)


if __name__ == "__main__":
    main()
