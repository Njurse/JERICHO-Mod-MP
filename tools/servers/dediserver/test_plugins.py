"""The dedicated server's plugin system and its built-in commands/admin plugins.

  * loading: a good user plugin loads, a broken one (raises in on_load, or fails
    to import) is logged and disabled while the server keeps going, and an
    ENABLED_BY_DEFAULT = False plugin stays off unless named;
  * routing: '!'/'/' lines are consumed and answered privately, ordinary chat is
    relayed, admin commands are refused to players and allowed to admins;
  * identity: the JPID block in a HELLO, and admins recognised by it;
  * !login: right/wrong password and the rate limit.

No sockets: sends are captured by replacing Dedicated.send_to / broadcast.
"""

import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)

import dediserver as d          # noqa: E402
import mp_test as m             # noqa: E402

GOOD = '''
loaded = []
def on_load(server):
    loaded.append(server)
    server.register_command("ping", lambda caller, args: "pong " + args)
def on_chat(player, text):
    return text == "swallow me"
'''
BROKEN_LOAD = "def on_load(server):\n    raise RuntimeError('boom')\n"
BROKEN_IMPORT = "def broken(:\n"
OFF = "ENABLED_BY_DEFAULT = False\ndef on_load(server):\n    raise SystemExit('must not load')\n"

ADMIN_ID = "00112233445566778899aabbccddeeff"


class FakeConn:
    def __init__(self, name):
        self.name = name

    def shutdown(self, how):
        self.closed = True


def chat_text(frame):
    return frame[4:].split(b"\0")[0].decode("latin1")


def build(tmp, **extra):
    cfg = dict(d.DEFAULTS)
    cfg.update({"name": "Plugin Test", "master": "", "beacon": False,
                "plugins_dir": tmp, "admins": [ADMIN_ID], "admin_password": "s3cret"})
    cfg.update(extra)
    server = d.Dedicated(cfg, d.Log("error"))
    server.sent = []                                   # (conn or None, text)
    server.send_to = lambda conn, tag, payload: server.sent.append((conn, chat_text(payload)))
    server.broadcast = lambda payload, tag, skip=None: server.sent.append((None, chat_text(payload)))
    return server


def join(server, slot, name, player_id=None):
    conn = FakeConn(name)
    server.clients[conn] = {"id": slot, "name": name, "addr": "10.0.0.%d:5000" % slot,
                            "player_id": player_id}
    server.plugins.call("on_player_join", d.Player(conn, server.clients[conn]))
    return conn


def say(server, conn, text):
    info = server.clients[conn]
    server.sent.clear()
    server._chat(conn, m.CHAT.pack(info["id"], 0, 0, 0, text.encode()), info["id"], info["name"])
    return list(server.sent)


def main():
    fails = 0

    def check(name, ok, detail=""):
        nonlocal fails
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail else ""))
        fails += 0 if ok else 1

    tmp = tempfile.mkdtemp()
    try:
        for n, src in (("good", GOOD), ("broken_load", BROKEN_LOAD),
                       ("broken_import", BROKEN_IMPORT), ("off", OFF)):
            with open(os.path.join(tmp, n + ".py"), "w") as f:
                f.write(src)

        server = build(tmp)
        loaded = [n for n, _ in server.plugins.plugins]
        check("built-ins load", loaded[:2] == ["admin", "commands"], loaded)
        check("a good user plugin loads", "good" in loaded, loaded)
        check("a plugin raising in on_load is disabled",
              "broken_load" in server.plugins.disabled and "broken_load" not in loaded)
        check("a plugin that cannot import is disabled", "broken_import" in server.plugins.disabled)
        check("ENABLED_BY_DEFAULT = False stays off", "off" not in loaded and "off" not in server.plugins.disabled)
        check("console lists plugins", "broken_load" in server.cmd_plugins(""))

        # -- identity
        pid_hex = "0123456789abcdef0123456789abcdef"
        hello = m.build_hello("Ann", mods=[("mp", "0.12.0")], player_id=bytes.fromhex(pid_hex))
        check("JPID parses after the manifest", m.parse_player_id(hello) == pid_hex)
        check("no JPID -> None", m.parse_player_id(m.build_hello("Old", mods=[("mp", "1")])) is None)
        check("JPID is not read as a mod row", len(d.parse_manifest(hello)) == 1)

        # -- routing
        bob = join(server, 1, "Bob")
        boss = join(server, 2, "Boss", ADMIN_ID)
        check("listed id is admin on join", server.clients[boss].get("admin") is True)
        check("unlisted is not admin", not server.clients[bob].get("admin"))

        out = say(server, bob, "hello all")
        check("plain chat is relayed", out == [(None, "hello all")], out)
        out = say(server, bob, "!ping x")
        check("a command is consumed and answered privately", out == [(bob, "pong x")], out)
        out = say(server, bob, "/ping y")
        check("'/' works too", out == [(bob, "pong y")], out)
        out = say(server, bob, "swallow me")
        check("a plugin's on_chat can consume a line", out == [], out)
        out = say(server, bob, "!help")
        text = " ".join(t for _, t in out)
        check("!help lists player commands", "!whoami" in text and "!ping" in text, text)
        check("!help hides admin commands from players", "!kick" not in text, text)
        out = say(server, boss, "!help")
        check("!help shows admins the admin commands", "!kick" in " ".join(t for _, t in out))

        out = say(server, bob, "!cops off")
        check("admin command denied to a player", "for admins" in out[0][1] and server.cfg["cops"] is True, out)
        out = say(server, boss, "!cops off")
        check("admin command allowed to an admin", server.cfg["cops"] is False and server.cfg["rules"] & 1 == 0, out)
        check("a rules change asks for a re-advertise", server.readvertise_evt.is_set())
        out = say(server, boss, "!difficulty 2")
        check("!difficulty packs into rules", (server.cfg["rules"] >> 2) & 3 == 2, out)
        out = say(server, bob, "!whoami")
        check("!whoami without an id says so", "none" in out[0][1], out)
        out = say(server, boss, "!whoami")
        check("!whoami shows the id", ADMIN_ID in out[0][1], out)
        out = say(server, bob, "!nosuch")
        check("unknown command answered privately", out and out[0][0] is bob and "unknown" in out[0][1], out)
        out = say(server, bob, "!who")
        check("!who hides addresses from players", "10.0.0." not in out[0][1], out)

        # -- console shares the registry
        names = [row[0] for row in server.console_commands()]
        check("console gets plugin commands", "cops" in names and "motd" in names, names)
        check("console keeps its own kick/say", names.count("kick") == 1 and names.count("say") == 1)

        # -- login + rate limit
        admin = dict(server.plugins.plugins)["admin"]
        clock = [1000.0]
        admin.now = lambda: clock[0]
        out = say(server, bob, "!login wrong")
        check("wrong password refused", "wrong" in out[0][1] and not server.clients[bob].get("admin"), out)
        say(server, bob, "!login wrong")
        out = say(server, bob, "!login wrong")
        check("third failure locks !login", "locked" in out[0][1], out)
        out = say(server, bob, "!login s3cret")
        check("locked even with the right password", "too many" in out[0][1] and not server.clients[bob].get("admin"), out)
        clock[0] += admin.LOGIN_LOCKOUT_S + 1
        out = say(server, bob, "!login s3cret")
        check("right password after the lockout", server.clients[bob].get("admin") is True, out)
        out = say(server, bob, "!kick Boss")
        check("a logged-in admin can kick", out[-1][1].startswith("kicked player 2"), out)

        # -- explicit list
        server2 = build(tmp, plugins=["commands", "off"])
        check("an explicit list loads exactly those (and disabled-by-default ones)",
              [n for n, _ in server2.plugins.plugins] == ["commands"]
              and "off" in server2.plugins.disabled, server2.plugins.disabled)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
