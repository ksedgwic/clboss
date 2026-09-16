import os
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

# Per-run memo of each peer's newest channel, keyed by the node the
# process talks to: one listpeerchannels call, and one
# listclosedchannels call only when a peer has no open channel.
# Never written to the alias cache file: a peer's newest channel
# changes with every open, and a peer that announces an alias later
# should get it.
_open_scids = {}
_closed_scids = {}

def _scid_key(scid):
    """Sort key for a short channel id: block, tx index, output."""
    try:
        block, tx, out = scid.split('x')
        return (int(block), int(tx), int(out))
    except (ValueError, AttributeError):
        return (0, 0, 0)

def _newest_by_peer(channels):
    newest = {}
    for ch in channels:
        scid = ch.get('short_channel_id')
        peer_id = ch.get('peer_id')
        if not scid or not peer_id:
            continue
        if _scid_key(scid) > _scid_key(newest.get(peer_id, '')):
            newest[peer_id] = scid
    return newest

def lookup_recent_scid(run_lightning_cli_command, lightning_dir, network_option, peer_id):
    """The peer's newest channel: the highest scid among its open
    channels, else among its closed ones; None if it never had one."""
    key = (lightning_dir, network_option)
    if key not in _open_scids:
        data = run_lightning_cli_command(lightning_dir, network_option, 'listpeerchannels')
        _open_scids[key] = _newest_by_peer((data or {}).get('channels', []))
    scid = _open_scids[key].get(peer_id)
    if scid:
        return scid
    if key not in _closed_scids:
        data = run_lightning_cli_command(lightning_dir, network_option, 'listclosedchannels')
        _closed_scids[key] = _newest_by_peer((data or {}).get('closedchannels', []))
    return _closed_scids[key].get(peer_id)

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
