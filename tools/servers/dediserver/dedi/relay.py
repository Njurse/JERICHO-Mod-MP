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


def client_traffic_band(pid, clients):
    """The band client `pid` really uses (mp_session.c MpTrafficBandRefresh): its
    MpTrafficMachineCount is its playerCount, never below its own id + 1. On this
    server that is the roster rows only: the roster names no player 0, so the
    client drops the welcome's phantom "Host" at the first roster (MpHandleRoster),
    which this server sends on the first loop pass after the welcome."""
    return traffic_band(pid, min(m.MAX_PLAYERS, max(clients, pid + 1)))


def traffic_slot_owner(server, slot):
    """MpTrafficSlotOwnerRank as the clients see it: the client whose own band
    (client_traffic_band) holds the slot. Bands can overlap (each client counts
    with its own id), so the highest id wins, as it does for the frames."""
    with server.lock:
        ids = [c["id"] for c in server.clients.values()]
    for r in sorted(ids, reverse=True):
        f, last = client_traffic_band(r, len(ids))
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

    The sender is STAMPED from the connection, not trusted from the payload: after the
    relay the header is the only record of who sent the frame, and a client has one link
    (to this server) that cannot tell it. Without this every peer's carhacks pick
    arrived looking like player 0's and was discarded. Returns True when relayed."""
    if len(payload) < m.CHANNEL.size:
        return False
    size = m.CHANNEL.size + m.CHANNEL.unpack_from(payload, 0)[1]
    if len(payload) < size:
        return False
    frame = bytearray(payload[:size])
    frame[m.CHANNEL_FROM_OFFSET] = pid & 0xFF
    server.broadcast(bytes(frame), m.TAG["channel"])
    return True


def round_state(server, conn, pid, payload):
    """JPRD from a client. Only the host writes the round state, so the in-game host
    drops one a peer sends (MpHandleRound) and so does this server: it is never
    forwarded, or a client could write its own score or team. The sender is answered
    with the server's own snapshot instead, so its copy is the server's again.
    Returns False (nothing was relayed)."""
    server.log.debug("player %d sent a round state (%d bytes): ignored, only the server writes it"
                     % (pid, len(payload)))
    try:
        server.send_to(conn, m.TAG["round"], server.round_bytes())
    except OSError:
        pass
    return False


def mark_state(server, conn, pid, payload):
    """JPMK from a client (proto 23). Only the server referees the timer modes, so,
    like a JPRD, it is never forwarded -- a client could otherwise name itself the
    winner or hand the bomb on. In a timer mode the sender gets the server's own
    snapshot back. Returns False (nothing was relayed)."""
    server.log.debug("player %d sent a mark state (%d bytes): ignored, only the server writes it"
                     % (pid, len(payload)))
    if server.mark.active:
        try:
            server.send_to(conn, m.TAG["mark"], server.mark_bytes())
        except OSError:
            pass
    return False
