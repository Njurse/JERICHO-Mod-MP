"""The round state (MP_TAG_ROUND, proto 22; the wire is proto 24 since round lifecycle v2) on the
dedicated server, over a real socket.

  * a joiner gets ROSTER, WELCOME, START and then the round state: Take a Ride's
    round 1, live, every player playing, no team, score 0;
  * a second joiner is announced to the first at once, and a leaver's row goes;
  * a client cannot write the round state: its JPRD is never forwarded, and the
    sender is answered with the server's own;
  * every envelope says proto 24, and proto-21, -22 and -23 HELLOs are refused
    (REJECT VERSION);
  * proto 24: an admin's row carries ROUND_ENTRY_F_ADMIN; the arena is 0..3 in
    the session spare's bits 8-9;
  * a config with no "gamemode" key referees mode 0.

The server's serve() runs on one end of a socketpair; no ports, no game.
"""
import os
import socket
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)

import dediserver as d          # noqa: E402
import mp_test as m             # noqa: E402

fails = 0


def check(name, ok, detail=""):
    global fails
    print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail else ""))
    fails += 0 if ok else 1


def recv_env(sock):
    """(version, tag, payload) for one frame -- the envelope's version included."""
    head = b""
    while len(head) < m.ENV.size:
        chunk = sock.recv(m.ENV.size - len(head))
        if not chunk:
            return None
        head += chunk
    ln, tag, ver, _fl, _sp = m.ENV.unpack(head)
    body = b""
    while len(body) < ln - 8:
        chunk = sock.recv(ln - 8 - len(body))
        if not chunk:
            return None
        body += chunk
    return ver, tag, body


def frames_until(sock, tag, limit=40):
    """Every frame up to and including the first `tag` one (skipping chat/pings)."""
    seen = []
    for _ in range(limit):
        f = recv_env(sock)
        if f is None:
            break
        seen.append(f)
        if f[1] == tag:
            break
    return seen


def server_for(tmp, **extra):
    cfg = dict(d.DEFAULTS)
    cfg.update({"name": "Round Test", "master": "", "beacon": False, "plugins_dir": tmp,
                "_config_path": os.path.join(tmp, "dedi.json"), "motd": ""})
    cfg.update(extra)
    return d.Dedicated(cfg, d.Log("error"))


def connect(server, name, proto=None):
    ours, theirs = socket.socketpair()
    ours.settimeout(5.0)
    t = threading.Thread(target=server.serve, args=(theirs, ("127.0.0.1", 40000 + len(name))),
                         daemon=True)
    t.start()
    hello = m.build_hello(name, [("mp", m.MP_VER)])
    if proto is not None:
        hello = m.HELLO.pack(proto, m.SDK, 0, 0, name.encode()[:m.NAME_MAX], 1, 0, 0, 0) + hello[m.HELLO.size:]
    m.send_frame(ours, m.TAG["hello"], hello)
    return ours, t


def main():
    tmp = tempfile.mkdtemp()
    server = server_for(tmp)
    try:
        check("the server starts round 1, live", (server.round.number, server.round.phase)
              == (1, m.ROUND_PHASE_LIVE), (server.round.number, server.round.phase))

        # -- the first joiner
        alice, _ta = connect(server, "Alice")
        seen = frames_until(alice, m.TAG["round"])
        tags = [f[1] for f in seen]
        check("a joiner gets ROSTER, WELCOME, START, then the round state",
              tags[:4] == [m.TAG["roster"], m.TAG["welcome"], m.TAG["start"], m.TAG["round"]], tags)
        check("every envelope says proto 24", m.PROTO == 24 and all(f[0] == 24 for f in seen),
              [f[0] for f in seen])
        rd = m.parse_round(seen[-1][2]) if seen and seen[-1][1] == m.TAG["round"] else None
        check("Take a Ride snapshot: mode 0, round 1, live, Alice playing, no team, score 0",
              rd == (0, m.ROUND_PHASE_LIVE, 1, [(1, m.ROUND_PLAYER_PLAYING, -1, 0)]), rd)
        check("the snapshot is header + one 8-byte row",
              len(seen[-1][2]) == m.ROUND.size + m.ROUND_ENTRY.size == 16, len(seen[-1][2]))

        # -- a second joiner: announced to the first at once
        bob, _tb = connect(server, "Bob")
        bob_seen = frames_until(bob, m.TAG["round"])
        rd_b = m.parse_round(bob_seen[-1][2]) if bob_seen else None
        check("the late joiner is playing too (late join is still live)",
              rd_b is not None and rd_b[1:3] == (m.ROUND_PHASE_LIVE, 1) and
              rd_b[3] == [(1, 0, -1, 0), (2, 0, -1, 0)], rd_b)
        a2 = frames_until(alice, m.TAG["round"])
        rd_a = m.parse_round(a2[-1][2]) if a2 and a2[-1][1] == m.TAG["round"] else None
        check("the first player hears of the joiner now", rd_a == rd_b, rd_a)

        # -- a client cannot write it
        forged = m.build_round(0, m.ROUND_PHASE_ENDED, 9, [(2, m.ROUND_PLAYER_PLAYING, 1, 999)])
        m.send_frame(bob, m.TAG["round"], forged)
        back = frames_until(bob, m.TAG["round"])
        rd_back = m.parse_round(back[-1][2]) if back and back[-1][1] == m.TAG["round"] else None
        check("a client's JPRD is answered with the server's own", rd_back == rd_b, rd_back)
        check("the server's round is untouched", (server.round.number, server.round.phase,
              server.round.players[2]["score"]) == (1, m.ROUND_PHASE_LIVE, 0))
        alice.settimeout(0.6)
        leaked = []
        try:
            while True:
                f = recv_env(alice)
                if f is None:
                    break
                leaked.append(f)
        except socket.timeout:
            pass
        check("...and never forwarded to anyone else",
              all(m.parse_round(f[2]) != m.parse_round(forged) for f in leaked if f[1] == m.TAG["round"]),
              [f[1] for f in leaked])
        alice.settimeout(5.0)

        # -- a leaver's row goes
        m.send_frame(bob, m.TAG["leave"], b"")
        after = frames_until(alice, m.TAG["round"])
        rd_l = m.parse_round(after[-1][2]) if after and after[-1][1] == m.TAG["round"] else None
        check("a leaver's row goes, the round goes on", rd_l == (0, m.ROUND_PHASE_LIVE, 1,
              [(1, m.ROUND_PLAYER_PLAYING, -1, 0)]), rd_l)

        # -- proto 24: the ADMIN bit rides the round row
        with server.lock:
            for info in server.clients.values():
                if info["id"] == 1:
                    info["admin"] = True
        server.broadcast_round_if_changed()
        adm = frames_until(alice, m.TAG["round"])
        raw = adm[-1][2] if adm and adm[-1][1] == m.TAG["round"] else b""
        row = m.ROUND_ENTRY.unpack_from(raw, m.ROUND.size) if len(raw) >= m.ROUND.size + m.ROUND_ENTRY.size else None
        check("an admin's round row carries ROUND_ENTRY_F_ADMIN", row is not None and row[0] == 1 and
              row[3] == m.ROUND_ENTRY_F_ADMIN, row)
        # -- proto 24: arenas 0..3, bits 8-9 of the session spare
        server.set_arena(3)
        check("arena 3 rides the session spare's bits 8-9",
              (server.session_spare() >> m.SESSION_ARENA_SHIFT) & m.SESSION_ARENA_MASK == 3, hex(server.session_spare()))
        try:
            server.set_arena(4)
            bad = False
        except ValueError:
            bad = True
        check("arena 4 is refused", bad)
        server.set_arena(0)

        # -- proto-21, -22 and -23 peers are refused
        for proto in (21, 22, 23):
            old, _to = connect(server, "Old%d" % proto, proto=proto)
            f = recv_env(old)
            check("a proto-%d HELLO is refused with REJECT VERSION" % proto,
                  f is not None and f[1] == m.TAG["reject"] and f[2][0] == 2, f and (f[1], f[2][:1]))
            old.close()
        m.send_frame(alice, m.TAG["leave"], b"")
        alice.close()
        bob.close()

        # -- no gamemode key: mode 0
        cfg = dict(d.DEFAULTS)
        cfg.pop("gamemode", None)
        cfg.update({"master": "", "beacon": False, "plugins_dir": tmp})
        bare = d.Dedicated(cfg, d.Log("error"))
        rd0 = m.parse_round(bare.round_bytes())
        check("a config without 'gamemode' referees mode 0", rd0 == (0, m.ROUND_PHASE_LIVE, 1, []), rd0)
        bare.stop.set()
    finally:
        server.stop.set()

    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
