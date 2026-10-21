"""Routing for the owner -> peers frames the in-game host forwards for its clients.

Each function mirrors one branch of mp_session.c's host side, so a dedicated
server routes exactly as a player's game would when it hosts. `server` is the
Dedicated instance (clients / lock / broadcast / send_to / log / colors).
"""
import mp_test as m


def traffic_band(rank, machines):
    """mp_session.c MpTrafficBandFor: the inclusive car_data slots machine `rank` owns."""
    if rank < 0 or rank >= m.TRAFFIC_SLOT_COUNT or machines > m.TRAFFIC_SLOT_COUNT:
        return -1, -1
    per = max(1, m.TRAFFIC_SLOT_COUNT // machines)
    f = m.TRAFFIC_SLOT_FIRST + rank * per
    last = f + per - 1
    if rank == machines - 1:
        last = m.MAX_CARS - 1
    f, last = min(f, m.MAX_CARS - 1), min(last, m.MAX_CARS - 1)
    return f, max(f, last)


def traffic_slot_owner(server, slot):
    """MpTrafficSlotOwnerRank as the clients see it. A client's machine count is its
    playerCount -- the roster plus the phantom player 0 this server stands in for --
    and never below its own id + 1, so take the largest any client can hold."""
    with server.lock:
        ids = [c["id"] for c in server.clients.values()]
    n = min(m.MAX_PLAYERS, max([len(ids) + 1] + [i + 1 for i in ids]))
    for r in range(n):
        f, last = traffic_band(r, n)
        if f <= slot <= last:
            return r
    return -1


def _conn_of(server, pid):
    with server.lock:
        for c, info in server.clients.items():
            if info["id"] == pid:
                return c
    return None


def hit(server, conn, pid, payload):
    """JPHI: a bump goes ONLY to the player whose car takes it (MpHandleHit's host
    branch): the player named by targetId, or for a traffic car (MP_HIT_F_TRAFFIC)
    the machine that owns that slot. Never back to the sender. Returns the target
    player id, or None when dropped."""
    if len(payload) < m.HIT.size:
        return None
    target, flags = m.HIT.unpack_from(payload, 0)[:2]
    if flags & m.HIT_F_TRAFFIC:
        target = traffic_slot_owner(server, target)
    dest = _conn_of(server, target) if target >= 0 else None
    if dest is None or dest is conn:
        return None
    try:
        server.send_to(dest, m.TAG["hit"], payload)
    except OSError:
        return None
    return target


def color_table(server):
    """MP_COLOR with every connected player's row we have, in id order."""
    with server.lock:
        live = {c["id"] for c in server.clients.values()}
        rows = [server.colors[i] for i in sorted(server.colors) if i in live]
    if not rows:
        return None
    return m.COLOR.pack(len(rows)) + b"".join(rows)


def color(server, conn, pid, payload):
    """JPCL: keep the rows the sender owns (MpRowOwnerOk: a row may only describe its
    sender), then send everyone the whole table -- the host's MpSendColors(1). The
    host also resends it on its roster cadence, which is how a late joiner learns
    everyone (Dedicated._send_roster_if_due). Returns the number of rows kept."""
    if len(payload) < m.COLOR.size:
        return 0
    n = min(m.COLOR.unpack_from(payload, 0)[0], m.MAX_PLAYERS)
    if len(payload) < m.COLOR.size + n * m.COLOR_ENTRY.size:
        return 0
    kept = 0
    for i in range(n):
        off = m.COLOR.size + i * m.COLOR_ENTRY.size
        row = payload[off:off + m.COLOR_ENTRY.size]
        if row[0] == pid:
            with server.lock:
                server.colors[pid] = bytes(row)
                # The same row carries the owner's character (byte 5). It is stored
                # next to the colour so the roster snapshot and the rebroadcast table
                # both carry it, exactly the way colour already travels.
                server.ped_models[pid] = row[5]
            kept += 1
    table = color_table(server)
    if kept and table:
        server.broadcast(table, m.TAG["color"])
    return kept


def channel(server, conn, pid, payload):
    """JPCH: an add-on's net-bridge frame. The game host consumes nothing itself on
    the wire: it broadcasts the frame (header + declared payload, trailing bytes cut)
    to EVERY connection, the sender included (MpHandleChannel -> MpHostBroadcast).
    Returns True when relayed."""
    if len(payload) < m.CHANNEL.size:
        return False
    size = m.CHANNEL.size + m.CHANNEL.unpack_from(payload, 0)[1]
    if len(payload) < size:
        return False
    server.broadcast(payload[:size], m.TAG["channel"])
    return True
