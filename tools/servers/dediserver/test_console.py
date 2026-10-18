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
    ped = m.PEDSTATE.pack(1) + m.PEDSTATE_ENTRY.pack(4, 1, 0, 100, -20, 300, 1024, 40)
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
    for pid, (srv, cli) in pairs.items():
        server.clients.pop(srv, None)
        srv.close()
        cli.close()

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
    check("two rows parse", len(d.parse_manifest(
        hello + m.MODI.pack(b"mp", b"1.0", 1, 0) + m.MODI.pack(b"carhacks", b"2", 1, 0))) == 2)
    check("a hello with no manifest parses as none", d.parse_manifest(hello) == [])

    # -- a co-hosted dedi must not list itself under an address nobody can dial
    check("loopback is caught", d.is_private_host("127.0.0.1") and d.is_private_host("localhost"))
    check("RFC1918 is caught",
          all(d.is_private_host(h) for h in ("10.1.2.3", "172.16.5.4", "192.168.1.9")))
    check("a public address is not",
          not d.is_private_host("143.198.78.241") and not d.is_private_host("172.32.5.4"))
    check("a name is the operator's business, not a private address",
          not d.is_private_host("jerichomp.animetiddi.es") and not d.is_private_host(""))

    # -- stop sets the event the run loop watches
    server.cmd_stop("")
    check("stop sets the stop event", server.stop.is_set())

    print("\ndedi console: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
