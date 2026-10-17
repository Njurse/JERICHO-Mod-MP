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
        "rules": 0, "beacon": True, "master": "",       # no master: 'master' says so
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
