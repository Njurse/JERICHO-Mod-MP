"""The dedicated server's console, driven without a terminal or a game.

Two halves:

  * every command against a real Dedicated object -- status, list, say, beacon,
    master, stop, help, and the unknown-command path;
  * kick against a REAL socket pair, so the wire bytes (a JPKK frame carrying the
    kicked player's id) and the shutdown are both checked, not just the reply.

Run it from the servers directory or from here; it fixes up its own path.
"""

import os
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # .../tools/servers (serverkit)
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))   # .../tools (mp_test)

import dediserver as d          # noqa: E402
from serverkit import Console   # noqa: E402
import mp_test as m             # noqa: E402


def build():
    cfg = dict(d.DEFAULTS)
    cfg.update({
        "name": "Console Test", "port": 1400, "city": 2, "max_players": 8,
        "cops": True, "cop_difficulty": 2, "beacon": True, "master": "",       # no master: 'master' says so
    })
    server = d.Dedicated(cfg, d.Log("warning"))
    console = Console(d.Log("warning"), server.console_commands())
    server.console = console
    return server, console


def main():
    fails = 0

    def check(name, ok, detail=""):
        nonlocal fails
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))
        fails += 0 if ok else 1

    server, console = build()

    # -- help is generated from the table, so every command must appear in it
    text = console.help_text()
    names = ["status", "list", "say", "kick", "beacon", "master", "stop", "help"]
    check("help lists every command", all(n in text for n in names),
          "%d commands" % len(console.order))

    # -- status reports the configured session
    st = server.cmd_status("")
    check("status names the server", "Console Test" in st)
    check("status shows readable rules", "cops on, difficulty 2" in st, st)
    check("rules packed as MP_RULES_PACK", server.cfg["rules"] == (0x1 | (2 << 2)))
    legacy = {"rules": 0}
    check("a legacy 'rules' int is honoured", d.resolve_rules(legacy, {"rules"}) and legacy["rules"] == 0)
    fresh = dict(d.DEFAULTS)
    d.resolve_rules(fresh, set())
    check("defaults: cops on, difficulty 1", fresh["rules"] == 0x5, str(fresh["rules"]))
    check("status shows the session port", "TCP/1400" in st)
    check("status shows the city", "city:      2" in st)
    check("status shows the player count", "players:   0/8" in st)

    # -- list with nobody connected
    check("list says nobody is connected", server.cmd_list("") == "no players connected")

    # -- say with nobody listening still reports honestly
    said = server.cmd_say("hello world")
    check("say reports the audience", said == "said to 0 player(s)", said)

    # -- kick refuses an unknown id, and refuses to guess
    check("kick with no id is a usage error", "kick who?" in server.cmd_kick(""))
    check("kick of an unknown id says so", "no player 7 connected" in server.cmd_kick("7"))

    # -- beacon toggles and reports
    check("beacon off", server.cmd_beacon("off") == "beacon off")
    check("beacon state reads off", "beacon is off" in server.cmd_beacon(""))
    check("beacon on", server.cmd_beacon("on") == "beacon on")
    check("beacon state reads on", "beacon is on" in server.cmd_beacon(""))
    check("beacon rejects nonsense", "usage: beacon" in server.cmd_beacon("maybe"))

    # -- master with no discovery server configured
    check("master says it is not configured",
          "no discovery server configured" in server.cmd_master(""))
    server.master_state.update(base="http://m:8100", registered=True, last="registered at 10:00")
    check("master shows the registration",
          "registered: yes" in server.cmd_master("") and "http://m:8100" in server.cmd_master(""))

    # -- the unknown-command path goes through dispatch, not the handler
    console.dispatch("bogus")
    check("unknown command did not raise", True)

    # -- ON-FOOT and TRAFFIC relay: owner -> everyone else, verbatim, like the
    # in-game host's MpHostRelay (the server used to drop both tags)
    pairs = {pid: socket.socketpair() for pid in (4, 5, 6)}
    for pid, (srv, _cli) in pairs.items():
        server.clients[srv] = {"id": pid, "name": "P%d" % pid, "addr": "127.0.0.1:%d" % pid}
        _cli.settimeout(1.0)
    ped = m.PEDSTATE.pack(1) + m.PEDSTATE_ENTRY.pack(4, 1, m.PED_MODEL_JERICHO, 0, 100, -20, 300, 1024, 40)
    check("an on-foot frame is relayed", server.relay_peer_state(pairs[4][0], m.TAG["ped"], ped, 4))
    for pid in (5, 6):
        tag, payload = m.recv_frame(pairs[pid][1])
        check("player %d got the on-foot pose verbatim" % pid, tag == m.TAG["ped"] and payload == ped)
    pairs[4][1].settimeout(0.2)
    try:
        echoed = m.recv_frame(pairs[4][1])
    except (socket.timeout, OSError):
        echoed = None
    check("the sender does not get its own pose back", echoed is None, repr(echoed))
    check("a pose naming another player is dropped",
          not server.relay_peer_state(pairs[4][0], m.TAG["ped"], ped, 5))
    check("a short on-foot frame is dropped",
          not server.relay_peer_state(pairs[4][0], m.TAG["ped"], ped[:10], 4))
    traffic = b"\x01\x00\x00\x00" + bytes(range(40))
    check("a traffic frame is relayed", server.relay_peer_state(pairs[5][0], m.TAG["traffic"], traffic, 5))
    pairs[4][1].settimeout(1.0)
    tag, payload = m.recv_frame(pairs[4][1])
    check("player 4 got the traffic frame verbatim", tag == m.TAG["traffic"] and payload == traffic)
    def got(pid, wait=0.2):
        cli = pairs[pid][1]
        cli.settimeout(wait)
        try:
            return m.recv_frame(cli)
        except (socket.timeout, OSError):
            return None

    from dedi import relay
    for p in (4, 5, 6):          # drain the traffic frame above (5 and 6 got it too)
        while got(p) is not None:
            pass
    # -- HIT: only the car that takes it; a traffic hit goes to the slot's owner
    hit = m.HIT.pack(6, 0, 10, 0, -10, 0, 0, 0, 0)
    check("a bump is routed to its target", relay.hit(server, pairs[4][0], 4, hit) == 6)
    r6 = got(6, 1.0)
    check("the target got the bump verbatim", r6 == (m.TAG["hit"], hit), repr(r6))
    check("nobody else got it", got(5) is None and got(4) is None)
    check("a bump aimed at yourself is dropped", relay.hit(server, pairs[4][0], 4, m.HIT.pack(4, 0, 1, 1, 1, 0, 0, 0, 0)) is None)
    check("a short bump is dropped", relay.hit(server, pairs[4][0], 4, hit[:8]) is None)
    check("a bump on an absent player is dropped", relay.hit(server, pairs[4][0], 4, m.HIT.pack(9, 0, 1, 1, 1, 0, 0, 0, 0)) is None)
    # 3 clients (ids 4..6) -> clients count 7 machines (ids + 1): 12 // 7 = 1 slot each,
    # rank 5 owns slot 25 (mp_session.c MpTrafficBandFor)
    check("band maths matches the game", relay.traffic_band(5, 7) == (25, 25) and relay.traffic_band(6, 7) == (26, 31))
    thit = m.HIT.pack(25, m.HIT_F_TRAFFIC, 1, 2, 3, 0, 0, 0, 0)
    check("a traffic bump goes to the slot's owner", relay.hit(server, pairs[4][0], 4, thit) == 5)
    check("the owner got it", got(5, 1.0) == (m.TAG["hit"], thit))

    # -- COLOR: own rows only, whole table to everyone, dropped on leave
    mine = m.COLOR.pack(2) + m.COLOR_ENTRY.pack(4, 1, 255, 0, 0, 0) + m.COLOR_ENTRY.pack(5, 1, 0, 0, 255, 0)
    check("only the sender's own colour row is kept", relay.color(server, pairs[4][0], 4, mine) == 1)
    table = m.COLOR.pack(1) + m.COLOR_ENTRY.pack(4, 1, 255, 0, 0, 0)
    check("everyone got the table, sender included",
          all(got(p, 1.0) == (m.TAG["color"], table) for p in (4, 5, 6)))
    relay.color(server, pairs[6][0], 6, m.COLOR.pack(1) + m.COLOR_ENTRY.pack(6, 0, 1, 2, 3, 0))
    both = m.COLOR.pack(2) + m.COLOR_ENTRY.pack(4, 1, 255, 0, 0, 0) + m.COLOR_ENTRY.pack(6, 0, 1, 2, 3, 0)
    check("the table holds every player, in id order", relay.color_table(server) == both)
    for p in (4, 5, 6):
        got(p, 1.0)
    alive, _ = server._send_roster_if_due(pairs[5][0], 0, 5)
    first, second = got(5, 1.0), got(5, 1.0)
    check("the roster cadence also sends the colour table (late joiners)",
          alive and first[0] == m.TAG["roster"] and second == (m.TAG["color"], both))
    stats = got(5, 1.0)
    check("the roster cadence also sends link stats (MP_TAG_STATS)",
          stats is not None and stats[0] == m.TAG["stats"], repr(stats))
    third = got(5, 1.0)
    check("the roster cadence also PINGs the client", third is not None and third[0] == m.TAG["ping"]
          and len(third[1]) == 4, repr(third))
    # the client echoes the tick in a PONG; the round trip becomes its roster ping
    import struct as _st
    from dedi import core as _core
    tick = (_core._ms32() - 42) & 0xFFFFFFFF
    rtt = server.note_pong(pairs[5][0], _st.pack("<I", tick))
    check("a PONG gives a round trip", rtt is not None and 40 <= rtt < 1000, repr(rtt))
    rows = server.roster_bytes()
    pings = {}
    for i in range(m.ROSTER.unpack_from(rows, 0)[0]):
        e = m.ROSTER_ENTRY.unpack_from(rows, m.ROSTER.size + i * m.ROSTER_ENTRY.size)
        pings[e[0]] = e[6]
    check("the roster carries the measured ping", pings.get(5) == rtt and pings.get(4) == 0, repr(pings))
    check("a stale or foreign PONG is ignored",
          server.note_pong(pairs[5][0], _st.pack("<I", (_core._ms32() + 100000) & 0xFFFFFFFF)) is None)
    # -- CAR PICK (JPCC): in the roster at once, CAR_READY, before any carstate
    check("a car pick is accepted", server.note_car_pick(pairs[6][0], m.CAR_PICK.pack(9, 1)))
    rows = server.roster_bytes()
    picked = {}
    for i in range(m.ROSTER.unpack_from(rows, 0)[0]):
        e = m.ROSTER_ENTRY.unpack_from(rows, m.ROSTER.size + i * m.ROSTER_ENTRY.size)
        picked[e[0]] = (e[2], e[3], e[4])
    check("the roster lists the picked car as ready",
          picked.get(6) == (9, 1, m.ROSTER_FLAG_CAR_READY) and picked.get(4) == (0xFF, 0xFF, 0), repr(picked))
    check("a short car pick is ignored", not server.note_car_pick(pairs[6][0], b"\x01"))
    # -- PALETTE: a pick that carries it (reserved[0], flagged by reserved[1]) is in the roster
    server.note_car_pick(pairs[6][0], bytes([9, 1, 4, 1]))
    rows = server.roster_bytes()
    pals = {}
    for i in range(m.ROSTER.unpack_from(rows, 0)[0]):
        e = m.ROSTER_ENTRY.unpack_from(rows, m.ROSTER.size + i * m.ROSTER_ENTRY.size)
        pals[e[0]] = e[7]
    check("the roster carries the picked palette", pals.get(6) == 4, repr(pals))
    server.note_car_pick(pairs[6][0], bytes([9, 1, 2, 0]))
    rows = server.roster_bytes()
    e = [m.ROSTER_ENTRY.unpack_from(rows, m.ROSTER.size + i * m.ROSTER_ENTRY.size)
         for i in range(m.ROSTER.unpack_from(rows, 0)[0])]
    check("an unflagged palette byte is ignored (old clients)",
          [r[7] for r in e if r[0] == 6] == [4], repr(e))
    # -- LINK STATS: one row per client, rates from the bytes each way, loss unknown
    body = server.stats_bytes()
    n = m.STATS.unpack_from(body, 0)[0]
    rows = {}
    for i in range(n):
        e = m.STATS_ENTRY.unpack_from(body, m.STATS.size + i * m.STATS_ENTRY.size)
        rows[e[0]] = e
    check("stats has a row for every client that has traffic", set(rows) >= {4, 5, 6} and
          len(body) == m.STATS.size + n * m.STATS_ENTRY.size, repr(sorted(rows)))
    check("the stats rows say loss unknown and carry tx > 0",
          all(r[1] == m.STATS_LOSS_UNKNOWN and r[4] > 0 for r in rows.values()), repr(rows))
    check("the stats carry the measured ping", rows[5][2] == rtt, repr(rows[5]))
    check("a short colour frame is dropped", relay.color(server, pairs[4][0], 4, m.COLOR.pack(3)) == 0)

    # -- CHANNEL: header + declared payload, to everyone (the sender too)
    ch = m.CHANNEL.pack(b"jerai", 3, 7, 1, 4, 0, 0) + b"abc"
    check("a channel frame is relayed", relay.channel(server, pairs[4][0], 4, ch + b"junk"))
    check("all three got it, trailing bytes cut",
          all(got(p, 1.0) == (m.TAG["channel"], ch) for p in (4, 5, 6)))
    check("a channel frame shorter than its len is dropped",
          not relay.channel(server, pairs[4][0], 4, ch[:-1]))

    for pid, (srv, cli) in pairs.items():
        server.clients.pop(srv, None)
        srv.close()
        cli.close()
    server.colors.clear()

    # -- dediserver.json is created from the example once, and never overwritten
    import tempfile
    from dedi import core as _c
    td = tempfile.mkdtemp()
    ex, cf = os.path.join(td, "ex.json"), os.path.join(td, "cfg.json")
    with open(ex, "w") as f:
        f.write('{"name": "From Example"}')
    check("a missing config is created from the example", _c.ensure_config(cf, ex) is not None
          and open(cf).read() == '{"name": "From Example"}')
    with open(cf, "w") as f:
        f.write('{"name": "Mine"}')
    check("an existing config is never overwritten", _c.ensure_config(cf, ex) is None
          and open(cf).read() == '{"name": "Mine"}')
    check("the repo ships the example", os.path.exists(os.path.join(HERE, _c.EXAMPLE_NAME)))

    # -- KICK over a real socket pair: the frame, then the connection closing
    a, b = socket.socketpair()
    server.clients[a] = {"id": 3, "name": "KickMe", "addr": "127.0.0.1:5000"}
    reply = server.cmd_kick("3")
    check("kick reports the player", "kicked player 3 'KickMe'" in reply, reply)

    tag, payload = m.recv_frame(b)
    check("a JPKK frame arrived", tag == m.TAG["kick"], repr(tag))
    kicked, = m.KICK.unpack_from(payload, 0)[:1]
    check("the frame names the kicked player", kicked == 3, str(kicked))

    b.settimeout(2.0)
    closed = False
    how = ""
    try:
        closed = (b.recv(16) == b"")     # EOF is b"", not ""
        how = "eof" if closed else "still receiving"
    except socket.timeout:
        how = "timeout (still open)"
    except (ConnectionResetError, ConnectionAbortedError, OSError) as e:
        # Windows surfaces the peer's shutdown as a reset rather than a clean EOF;
        # either way the link is gone, which is what the kick is for.
        closed = True
        how = "reset (%s)" % e.__class__.__name__
    check("the connection was closed after the kick", closed, how)

    a.close()
    b.close()

    # -- KICK with a reason: the text tail, truncated safely, and the old head intact
    check("kick with no reason sends the bare 4-byte head", len(payload) == m.KICK.size, str(len(payload)))
    a, b = socket.socketpair()
    server.clients[a] = {"id": 4, "name": "Rude", "addr": "127.0.0.1:5001"}
    reply = server.cmd_kick("4 spamming the chat")
    check("kick with a reason reports it", reply.endswith(": spamming the chat"), reply)
    tag, payload = m.recv_frame(b)
    text = payload[m.KICK.size:].split(b"\0")[0].decode()
    check("the kick carries the reason", len(payload) == m.KICK.size + m.KICK_TEXT_LEN
          and text == "Kicked by admin: spamming the chat", repr(payload))
    a.close()
    b.close()
    check("a plain kick has no rejoin flag", payload[1] == 0, repr(payload[:4]))
    a, b = socket.socketpair()
    server.clients[a] = {"id": 6, "name": "Mover", "addr": "127.0.0.1:5002"}
    server.kick_player(6, "city change", by="vote", rejoin=True)
    tag, payload = m.recv_frame(b)
    check("kick(rejoin=True) sets KICK_FLAG_REJOIN", payload[1] == m.KICK_FLAG_REJOIN, repr(payload[:4]))
    a.close()
    b.close()
    long = server.kick_text("admin", "\u00e9" * 80)
    check("a long reason is cut to 63 bytes without splitting UTF-8",
          len(long.encode()) <= m.KICK_TEXT_LEN - 1 and long.startswith("Kicked by admin: "), repr(long))

    # -- config values that would otherwise fail QUIETLY
    check("a scheme is stripped from advertise_host",
          d.normalise_host("http://jerichomp.animetiddi.es") == "jerichomp.animetiddi.es")
    check("a path, port and trailing dot go too",
          d.normalise_host("https://play.example.net:1318/x.") == "play.example.net")
    check("a bare host is left exactly alone",
          d.normalise_host("203.0.113.5") == "203.0.113.5" and d.normalise_host("") == "")

    # -- the mod-check policy, mirroring the real host's MpModsMatchHost
    check("level 0 admits anyone",
          d.mods_match([("mp", "9", 1)], ["mp", "carhacks"], {}, 0))
    check("level 1 wants the same enabled ids",
          d.mods_match([("mp", "1", 1), ("carhacks", "2", 1)], ["mp", "carhacks"], {}, 1)
          and not d.mods_match([("mp", "1", 1)], ["mp", "carhacks"], {}, 1))
    check("level 1 ignores versions",
          d.mods_match([("mp", "9.9", 1)], ["mp"], {"mp": "1.0"}, 1))
    check("level 2 compares versions",
          not d.mods_match([("mp", "9.9", 1)], ["mp"], {"mp": "1.0"}, 2))
    check("a disabled mod does not count",
          not d.mods_match([("mp", "1", 1), ("carhacks", "1", 0)], ["mp", "carhacks"], {}, 1))
    check("a server naming no mods matches everyone", d.mods_match([], [], {}, 2))

    # -- the manifest rides after the hello header, in the same frame
    hello = m.HELLO.pack(m.PROTO, m.SDK, 0, 0, b"X", 1, 0, 0, 0)
    check("a manifest is parsed off the hello",
          d.parse_manifest(hello + m.MODI.pack(b"mp", b"1.0", 1, 0)) == [("mp", "1.0", 1)])
    hello2 = m.HELLO.pack(m.PROTO, m.SDK, 0, 0, b"X", 2, 0, 0, 0)
    check("two rows parse", len(d.parse_manifest(
        hello2 + m.MODI.pack(b"mp", b"1.0", 1, 0) + m.MODI.pack(b"carhacks", b"2", 1, 0))) == 2)
    check("only modCount rows are read (a trailer is never a mod)", len(d.parse_manifest(
        hello + m.MODI.pack(b"mp", b"1.0", 1, 0) + m.MODI.pack(b"carhacks", b"2", 1, 0))) == 1)
    check("a hello with no manifest parses as none", d.parse_manifest(hello) == [])

    # -- a co-hosted dedi must not list itself under an address nobody can dial
    check("loopback is caught", d.is_private_host("127.0.0.1") and d.is_private_host("localhost"))
    check("RFC1918 is caught",
          all(d.is_private_host(h) for h in ("10.1.2.3", "172.16.5.4", "192.168.1.9")))
    check("a public address is not",
          not d.is_private_host("143.198.78.241") and not d.is_private_host("172.32.5.4"))
    check("a name is the operator's business, not a private address",
          not d.is_private_host("jerichomp.animetiddi.es") and not d.is_private_host(""))

    # -- CHARACTER: the model id rides in the colour row, the pose and the roster
    from dedi import relay as _relay
    _relay.color(server, pairs[6][0], 6,
                 m.COLOR.pack(1) + m.COLOR_ENTRY.pack(6, 0, 1, 2, 3, m.PED_MODEL_JERICHO))
    check("the colour row's pedModel is kept", server.ped_models.get(6) == m.PED_MODEL_JERICHO,
          repr(server.ped_models))
    raw = server.roster_bytes()
    roster_models = {}
    for i in range(m.ROSTER.unpack_from(raw, 0)[0]):
        e = m.ROSTER_ENTRY.unpack_from(raw, m.ROSTER.size + i * m.ROSTER_ENTRY.size)
        roster_models[e[0]] = e[5]
    check("the roster carries the owner's character",
          roster_models.get(6) == m.PED_MODEL_JERICHO, repr(roster_models))
    pose = m.PEDSTATE.pack(1) + m.PEDSTATE_ENTRY.pack(
        6, 0, m.PED_MODEL_TANNER, 0, 0, 0, 0, 0, 0)
    check("an on-foot pose is relayed",
          server.relay_peer_state(pairs[6][0], m.TAG["ped"], pose, 6))
    check("the pose's pedModel is kept too", server.ped_models.get(6) == m.PED_MODEL_TANNER,
          repr(server.ped_models))

    # -- stop sets the event the run loop watches
    server.cmd_stop("")
    check("stop sets the stop event", server.stop.is_set())

    print("\ndedi console: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
