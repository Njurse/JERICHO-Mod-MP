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
    server.register_command("help", lambda caller, args: "hijacked")
def on_chat(player, text):
    return text == "swallow me"
'''
BROKEN_LOAD = "def on_load(server):\n    raise RuntimeError('boom')\n"
BROKEN_IMPORT = "def broken(:\n"
OFF = "ENABLED_BY_DEFAULT = False\ndef on_load(server):\n    raise SystemExit('must not load')\n"

ADMIN_ID = "00112233-4455-4677-8899-AABBCCDDEEFF"       # listed in upper case, dashed
ADMIN_NORM = ADMIN_ID.lower()


class FakeConn:
    def __init__(self, name):
        self.name = name

    def shutdown(self, how):
        self.closed = True


def chat_text(frame):
    return frame[4:].split(b"\0")[0].decode("latin1")


def build(tmp, **extra):
    import json
    cfg_path = os.path.join(tmp, "dedi.json")
    with open(cfg_path, "w") as f:
        json.dump({"name": "Plugin Test", "custom_key": 42}, f)
    cfg = dict(d.DEFAULTS)
    cfg["_config_path"] = cfg_path
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
        check("built-ins load", loaded[:3] == ["admin", "announce", "commands"], loaded)
        check("a good user plugin loads", "good" in loaded, loaded)
        check("a plugin raising in on_load is disabled",
              "broken_load" in server.plugins.disabled and "broken_load" not in loaded)
        check("a plugin that cannot import is disabled", "broken_import" in server.plugins.disabled)
        check("ENABLED_BY_DEFAULT = False stays off", "off" not in loaded and "off" not in server.plugins.disabled)
        check("console lists plugins", "broken_load" in server.cmd_plugins(""))

        # -- identity
        pid_hex = "0123456789ab4def8123456789abcdef"
        hello = m.build_hello("Ann", mods=[("mp", "0.12.0")], player_id=bytes.fromhex(pid_hex))
        check("JPID parses to a dashed UUID", m.parse_player_id(hello) == "01234567-89ab-4def-8123-456789abcdef",
              m.parse_player_id(hello))
        check("ids: plain/dashed/upper/braces all normalize",
              len({m.normalize_player_id(x) for x in (pid_hex, pid_hex.upper(), "{01234567-89AB-4def-8123-456789abcdef}",
                                                      "01234567-89ab-4def-8123-456789abcdef")}) == 1)
        check("ids: junk is refused", m.normalize_player_id("not-an-id") is None and m.normalize_player_id(pid_hex[:-2]) is None)
        check("a clashing command from another plugin is refused",
              server.plugins.commands["help"].plugin == "commands")
        check("no JPID -> None", m.parse_player_id(m.build_hello("Old", mods=[("mp", "1")])) is None)
        check("JPID is not read as a mod row", len(d.parse_manifest(hello)) == 1)

        # -- routing
        bob = join(server, 1, "Bob")
        boss = join(server, 2, "Boss", ADMIN_NORM)
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
        check("!whoami shows the id", ADMIN_NORM in out[0][1], out)
        out = say(server, bob, "!help whoami")
        check("!help <command> gives its details", out[0][1].startswith("!whoami - "), out)
        out = say(server, bob, "!help kick")
        check("!help on an admin command is hidden from players", "no command" in out[0][1], out)
        out = say(server, boss, "!who")
        allt = " | ".join(t for _, t in out)
        check("!who shows admins ids and addresses", ADMIN_NORM in allt and "10.0.0." in allt, allt)

        # -- time / weather, and what goes on the wire
        out = say(server, boss, "!time night")
        check("!time sets time_of_day", server.cfg["time_of_day"] == "night" and "next level load" in out[0][1], out)
        out = say(server, boss, "!weather rain")
        check("!weather sets weather", server.cfg["weather"] == "rain", out)
        out = say(server, boss, "!weather snow")
        check("!weather refuses an unknown name", "usage" in out[0][1] and server.cfg["weather"] == "rain", out)
        w = m.WELCOME.unpack(server._welcome(3, 1))
        check("welcome carries time/weather/rules", w[10] == 3 and w[11] == 1 and w[14] == server.cfg["rules"], w)

        # -- !admin add/remove/list, persisted
        import json
        out = say(server, boss, "!admin add Bob")
        check("!admin add needs Bob's id (he has none)", "not a player id" in out[0][1], out)
        new_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"
        carl = join(server, 3, "Carl", new_id)
        out = say(server, boss, "!admin add carl")
        saved = json.load(open(server.config_path))
        check("!admin add saves to the file", new_id in saved["admins"] and saved["custom_key"] == 42, saved)
        check("!admin add grants it now", server.clients[carl].get("admin") is True)
        out = say(server, boss, "!admin list")
        check("!admin list shows who is online", "Carl, online" in " | ".join(t for _, t in out), out)
        out = say(server, boss, "!admin remove AAAAAAAABBBB4CCC8DDDEEEEEEEEEEEE")
        saved = json.load(open(server.config_path))
        check("!admin remove by plain hex saves", new_id not in saved["admins"] and not server.clients[carl].get("admin"), saved)
        say(server, boss, "!motd Hi there")
        say(server, boss, "!motd save")
        saved = json.load(open(server.config_path))
        check("!motd save persists", saved.get("motd") == "Hi there" and saved["custom_key"] == 42, saved)
        check("no temp file left behind", not os.path.exists(server.config_path + ".tmp"))
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
