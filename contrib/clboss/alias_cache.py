import os
import re
import json

# Define the cache directory and file path
CACHE_DIR = os.path.join(os.path.expanduser("~"), ".clboss")
CACHE_FILE = os.path.join(CACHE_DIR, "alias_cache.json")

def load_cache():
    if os.path.exists(CACHE_FILE):
        with open(CACHE_FILE, 'r') as f:
            return json.load(f)
    return {}

def save_cache(cache):
    # Ensure the cache directory exists
    if not os.path.exists(CACHE_DIR):
        os.makedirs(CACHE_DIR)

    with open(CACHE_FILE, 'w') as f:
        json.dump(cache, f)

# Per-run index of the node's channels, keyed by the node the process
# talks to: one listpeerchannels call, and one listclosedchannels
# call only when the open channels do not answer.  Each index is
# (newest scid by peer, peer by scid).  Never written to the alias
# cache file: a peer's newest channel changes with every open, and a
# peer that announces an alias later should get it.
_open_index = {}
_closed_index = {}

def _scid_key(scid):
    """Sort key for a short channel id: block, tx index, output."""
    try:
        block, tx, out = scid.split('x')
        return (int(block), int(tx), int(out))
    except (ValueError, AttributeError):
        return (0, 0, 0)

def _index_channels(channels):
    newest = {}
    by_scid = {}
    for ch in channels:
        scid = ch.get('short_channel_id')
        peer_id = ch.get('peer_id')
        if not scid or not peer_id:
            continue
        by_scid[scid] = peer_id
        if _scid_key(scid) > _scid_key(newest.get(peer_id, '')):
            newest[peer_id] = scid
    return newest, by_scid

def _open_channels(run_lightning_cli_command, lightning_dir, network_option):
    key = (lightning_dir, network_option)
    if key not in _open_index:
        data = run_lightning_cli_command(lightning_dir, network_option, 'listpeerchannels')
        _open_index[key] = _index_channels((data or {}).get('channels', []))
    return _open_index[key]

def _closed_channels(run_lightning_cli_command, lightning_dir, network_option):
    key = (lightning_dir, network_option)
    if key not in _closed_index:
        data = run_lightning_cli_command(lightning_dir, network_option, 'listclosedchannels')
        _closed_index[key] = _index_channels((data or {}).get('closedchannels', []))
    return _closed_index[key]

def lookup_recent_scid(run_lightning_cli_command, lightning_dir, network_option, peer_id):
    """The peer's newest channel: the highest scid among its open
    channels, else among its closed ones; None if it never had one."""
    scid = _open_channels(run_lightning_cli_command, lightning_dir, network_option)[0].get(peer_id)
    if scid:
        return scid
    return _closed_channels(run_lightning_cli_command, lightning_dir, network_option)[0].get(peer_id)

def lookup_nodeid_by_scid(run_lightning_cli_command, lightning_dir, network_option, scid):
    """The peer on the far end of one of our channels, open or
    closed; None if no channel has that scid."""
    peer_id = _open_channels(run_lightning_cli_command, lightning_dir, network_option)[1].get(scid)
    if peer_id:
        return peer_id
    return _closed_channels(run_lightning_cli_command, lightning_dir, network_option)[1].get(scid)

def lookup_alias(run_lightning_cli_command, lightning_dir, network_option, peer_id):
    """The peer's alias from gossip, cached; without one, its newest
    channel's scid; without that, the peer id.  Only a found alias
    is cached, so an older cache entry holding the bare peer id is
    looked up again."""
    cache = load_cache()

    alias = cache.get(peer_id)
    if alias and alias != peer_id:
        return alias

    listnodes_data = run_lightning_cli_command(lightning_dir, network_option, 'listnodes', peer_id)
    if listnodes_data:
        for node in listnodes_data.get("nodes", []):
            alias = node.get("alias")
            if alias:
                cache[peer_id] = alias
                save_cache(cache)
                return alias

    scid = lookup_recent_scid(run_lightning_cli_command, lightning_dir, network_option, peer_id)
    return scid or peer_id

def lookup_nodeid_by_alias(run_lightning_cli_command, lightning_dir, network_option, alias):
    # Load the cache
    cache = load_cache()

    # Reverse search in the cache (alias -> node ID)
    for peer_id, cached_alias in cache.items():
        if cached_alias == alias:
            return peer_id

    # Perform exhaustive search using `listnodes`
    listnodes_data = run_lightning_cli_command(lightning_dir, network_option, 'listnodes')
    if listnodes_data:
        nodes = listnodes_data.get("nodes", [])
        for node in nodes:
            if node.get("alias") == alias:
                peer_id = node.get("nodeid")
                # Cache the result
                cache[peer_id] = alias
                save_cache(cache)
                return peer_id

    return None  # If alias not found

def is_nodeid(nodeid_or_alias):
    return (len(nodeid_or_alias) == 66 and
            all(c in '0123456789abcdefABCDEF' for c in nodeid_or_alias))

def is_scid(text):
    return re.fullmatch(r'\d+x\d+x\d+', text) is not None
