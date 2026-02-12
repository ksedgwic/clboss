#!/usr/bin/env python3
import json
import shutil
import sqlite3
import subprocess


COMMON_FIELDS = [
    "set_base",
    "set_ppm",
    "baseline_base",
    "baseline_ppm",
    "size_mult",
    "size_total_peers",
    "size_less_peers",
    "balance_mult",
    "balance_our_msat",
    "balance_total_msat",
    "price_level",
    "price_center",
    "price_mult",
    "mult_product",
    "est_base",
    "est_ppm",
]
FLOAT_FIELDS = {
    "size_mult",
    "balance_mult",
    "price_mult",
    "mult_product",
}


def epoch_arg(value):
    text = f"{value:.6f}"
    text = text.rstrip("0").rstrip(".")
    if text == "":
        return "0"
    return text


def _dt_to_epoch(dt):
    if dt is None:
        return None
    return float(dt.timestamp())


def _warn(warn, message):
    if warn is not None:
        warn(message)


def run_lightning_cli_command(lightning_dir, network_option, subcommand, *args):
    command = ["lightning-cli", network_option]
    if lightning_dir:
        command.append(f"--lightning-dir={lightning_dir}")
    command.append(subcommand)
    command.extend(args)
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=True)
    except FileNotFoundError:
        raise RuntimeError("lightning-cli not found in PATH")
    except subprocess.CalledProcessError as e:
        stderr = e.stderr.strip()
        if stderr:
            raise RuntimeError(f"command failed: {' '.join(command)}: {stderr}")
        raise RuntimeError(f"command failed: {' '.join(command)}")

    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"invalid JSON from lightning-cli: {e}")


def _normalize_field(name, value):
    if value is None:
        return None
    try:
        if name in FLOAT_FIELDS:
            return float(value)
        return int(value)
    except (TypeError, ValueError):
        return None


def _normalize_record(node_id, raw, source):
    if "ts" not in raw:
        return None
    try:
        ts = float(raw["ts"])
    except (TypeError, ValueError):
        return None

    fields = {}
    for field in COMMON_FIELDS:
        fields[field] = _normalize_field(field, raw.get(field))

    return {
        "node_id": node_id,
        "ts": ts,
        "fields": fields,
        "raw": raw,
        "source": source,
    }


def _record_sort_key(record):
    raw_id = record["raw"].get("id")
    try:
        raw_id = int(raw_id)
    except (TypeError, ValueError):
        raw_id = -1
    source_rank = 0 if record["source"] == "external" else 1
    return (record["ts"], source_rank, raw_id)


def _fetch_api_peer_history(
    node_id, since_ts, before_ts, lightning_dir, network_option, warn, api_enabled
):
    if not api_enabled:
        return []

    args = [node_id]
    if since_ts is not None:
        args.append(epoch_arg(since_ts))
    if before_ts is not None:
        args.append(epoch_arg(before_ts))

    try:
        response = run_lightning_cli_command(
            lightning_dir, network_option, "clboss-feemon-history", *args
        )
    except RuntimeError:
        _warn(
            warn,
            "unable to fetch API fee monitor data; using external DB fallback",
        )
        return []

    history = response.get("history") if isinstance(response, dict) else None
    if not isinstance(history, list):
        _warn(
            warn,
            "unexpected clboss-feemon-history response; using external DB fallback",
        )
        return []

    out = []
    for item in history:
        if not isinstance(item, dict):
            continue
        rec = _normalize_record(node_id, item, "api")
        if rec is None:
            continue
        out.append(rec)

    out.sort(key=_record_sort_key)
    return out


def _fetch_external_peer_history(
    conn, node_id, since_ts, before_ts, before_inclusive=True
):
    where = ["p.node_id = :node_id"]
    binds = {"node_id": node_id}
    if since_ts is not None:
        where.append("e.ts >= :since")
        binds["since"] = since_ts
    if before_ts is not None:
        op = "<=" if before_inclusive else "<"
        where.append(f"e.ts {op} :before")
        binds["before"] = before_ts

    query = f"""
        SELECT
            e.id,
            e.ts,
            e.set_base,
            e.set_ppm,
            e.baseline_base,
            e.baseline_ppm,
            e.size_mult,
            e.size_total_peers,
            e.size_less_peers,
            e.balance_mult,
            e.balance_our_msat,
            e.balance_total_msat,
            e.price_level,
            e.price_mult,
            e.mult_product,
            e.est_base,
            e.est_ppm
        FROM fee_change_events e
        JOIN peers p ON p.id = e.peer_id
        WHERE {" AND ".join(where)}
        ORDER BY e.ts ASC, e.id ASC
    """
    out = []
    for row in conn.execute(query, binds):
        rec = _normalize_record(node_id, dict(row), "external")
        if rec is None:
            continue
        out.append(rec)
    return out


def list_external_node_ids(db_path, since_dt=None, before_dt=None):
    if not db_path:
        return []

    since_ts = _dt_to_epoch(since_dt)
    before_ts = _dt_to_epoch(before_dt)
    where = []
    binds = {}
    if since_ts is not None:
        where.append("e.ts >= :since")
        binds["since"] = since_ts
    if before_ts is not None:
        where.append("e.ts <= :before")
        binds["before"] = before_ts

    query = [
        "SELECT DISTINCT p.node_id",
        "FROM fee_change_events e",
        "JOIN peers p ON p.id = e.peer_id",
    ]
    if where:
        query.append("WHERE " + " AND ".join(where))
    query.append("ORDER BY p.node_id ASC")
    sql = "\n".join(query)

    with sqlite3.connect(db_path) as conn:
        return [row[0] for row in conn.execute(sql, binds)]


def list_api_node_ids(lightning_dir, network_option, warn=None):
    if shutil.which("lightning-cli") is None:
        _warn(warn, "lightning-cli not found; using external DB data only")
        return []

    try:
        response = run_lightning_cli_command(
            lightning_dir, network_option, "listpeerchannels"
        )
    except RuntimeError as e:
        _warn(
            warn,
            f"unable to list peers from API ({e}); using external DB data only",
        )
        return []

    channels = response.get("channels") if isinstance(response, dict) else None
    if not isinstance(channels, list):
        _warn(warn, "unexpected listpeerchannels response; using external DB data only")
        return []

    peers = set()
    for ch in channels:
        if not isinstance(ch, dict):
            continue
        peer_id = ch.get("peer_id")
        if isinstance(peer_id, str) and peer_id:
            peers.add(peer_id)
    return sorted(peers)


def _merged_peer_records_from_conn(
    conn, node_id, since_ts, before_ts, lightning_dir, network_option, warn, api_enabled
):
    api_records = _fetch_api_peer_history(
        node_id,
        since_ts,
        before_ts,
        lightning_dir,
        network_option,
        warn,
        api_enabled,
    )

    if api_records:
        api_earliest_ts = api_records[0]["ts"]
        db_before_ts = api_earliest_ts
        if before_ts is not None:
            db_before_ts = min(db_before_ts, before_ts)
        external_records = _fetch_external_peer_history(
            conn, node_id, since_ts, db_before_ts, before_inclusive=False
        )
    else:
        external_records = _fetch_external_peer_history(
            conn, node_id, since_ts, before_ts, before_inclusive=True
        )

    merged = external_records + api_records
    merged.sort(key=_record_sort_key)
    return merged


def load_merged_peer_records(
    db_path,
    node_id,
    since_dt=None,
    before_dt=None,
    lightning_dir=None,
    network_option="--network=bitcoin",
    warn=None,
):
    since_ts = _dt_to_epoch(since_dt)
    before_ts = _dt_to_epoch(before_dt)
    api_enabled = shutil.which("lightning-cli") is not None
    if not api_enabled and not db_path:
        _warn(warn, "no API and no external DB provided")
        return []
    if not api_enabled and db_path:
        _warn(warn, "lightning-cli not found; using external DB data only")
    if not db_path:
        return _fetch_api_peer_history(
            node_id,
            since_ts,
            before_ts,
            lightning_dir,
            network_option,
            warn,
            api_enabled,
        )

    with sqlite3.connect(db_path) as conn:
        conn.row_factory = sqlite3.Row
        return _merged_peer_records_from_conn(
            conn,
            node_id,
            since_ts,
            before_ts,
            lightning_dir,
            network_option,
            warn,
            api_enabled,
        )


def load_merged_records_by_node(
    db_path,
    node_ids,
    api_node_ids=None,
    since_dt=None,
    before_dt=None,
    lightning_dir=None,
    network_option="--network=bitcoin",
    warn=None,
):
    since_ts = _dt_to_epoch(since_dt)
    before_ts = _dt_to_epoch(before_dt)
    api_enabled = shutil.which("lightning-cli") is not None
    if not api_enabled and not db_path:
        _warn(warn, "no API and no external DB provided")
        return {}
    if not api_enabled and db_path:
        _warn(warn, "lightning-cli not found; using external DB data only")

    if not db_path:
        out = {}
        api_nodes = None
        if api_node_ids is not None:
            api_nodes = set(api_node_ids)
        for node_id in sorted(set(node_ids)):
            node_api_enabled = api_enabled
            if api_nodes is not None and node_id not in api_nodes:
                node_api_enabled = False
            merged = _fetch_api_peer_history(
                node_id,
                since_ts,
                before_ts,
                lightning_dir,
                network_option,
                warn,
                node_api_enabled,
            )
            if merged:
                out[node_id] = merged
        return out

    out = {}
    with sqlite3.connect(db_path) as conn:
        conn.row_factory = sqlite3.Row
        api_nodes = None
        if api_node_ids is not None:
            api_nodes = set(api_node_ids)
        for node_id in sorted(set(node_ids)):
            node_api_enabled = api_enabled
            if api_nodes is not None and node_id not in api_nodes:
                node_api_enabled = False
            merged = _merged_peer_records_from_conn(
                conn,
                node_id,
                since_ts,
                before_ts,
                lightning_dir,
                network_option,
                warn,
                node_api_enabled,
            )
            if merged:
                out[node_id] = merged
    return out


def records_to_rows(records, fields):
    rows = []
    for record in records:
        row = [record["ts"]]
        for field in fields:
            if field == "node_id":
                row.append(record["node_id"])
                continue
            row.append(record["fields"].get(field))
        rows.append(tuple(row))
    return rows
