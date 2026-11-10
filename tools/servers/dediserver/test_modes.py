"""The game-mode scaffold on the dedicated server.

  * mode 0 is what the server has always sent: "gamemode" defaults to 0 and is
    named TAKE A RIDE, and a config file WITHOUT the key still loads with it;
  * no new DEFAULTS key came with modes (an upgrade must not rewrite a config);
  * the modes plugin is off by default, loads when named, and !mode is read-only.

No sockets (same harness as test_plugins.py).
"""
import json
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
from test_plugins import build, join   # noqa: E402



def main():
    fails = 0

    def check(name, ok, detail=""):
        nonlocal fails
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail else ""))
        fails += 0 if ok else 1

    check("DEFAULTS gamemode is 0", d.DEFAULTS.get("gamemode") == 0, d.DEFAULTS.get("gamemode"))
    check("mode 0 is TAKE A RIDE", m.mode_name(0) == "TAKE A RIDE")
    check("the modes are 0 TAKE A RIDE, 1 FOX N HOUNDS, 2 CAR BOMB TAG, 3 COPS N ROBBERS",
          m.GAMEMODE_NAMES == {0: "TAKE A RIDE", 1: "FOX N HOUNDS", 2: "CAR BOMB TAG",
                               3: "COPS N ROBBERS"}, m.GAMEMODE_NAMES)
    check("an unknown id is named '-'", m.mode_name(7) == "-")
    check("no mode keys in DEFAULTS", not any(k.startswith("mode") or k.startswith("team") for k in d.DEFAULTS),
          [k for k in d.DEFAULTS if k.startswith(("mode", "team"))])

    tmp = tempfile.mkdtemp()
    try:
        # a config file with NO gamemode key: loads, gets 0, and the file is not given one
        # beyond what upgrade_config already did before modes (it adds every DEFAULT)
        path = os.path.join(tmp, "dedi.json")
        with open(path, "w") as f:
            json.dump({"name": "No Mode Key", "port": 1318}, f)
        cfg, _p = d.build_config(["--config", path])
        check("config without 'gamemode' loads as mode 0", int(cfg["gamemode"]) == 0, cfg.get("gamemode"))
        with open(path) as f:
            got = json.load(f)
        check("the user's keys are kept", got.get("name") == "No Mode Key" and got.get("port") == 1318, got)

        # explicit gamemode from the CLI still wins (same id end to end)
        cfg2, _p = d.build_config(["--config", path, "--gamemode", "0"])
        check("--gamemode 0", int(cfg2["gamemode"]) == 0)

        # plugin: off by default
        server = build(tmp)
        loaded = [n for n, _ in server.plugins.plugins]
        check("modes plugin is off by default", "modes" not in loaded and "modes" not in server.plugins.disabled, loaded)
        check("no !mode without the plugin", "mode" not in server.plugins.commands)
        check("status names the gamemode", "gamemode:  0" in server.cmd_status(""))

        # plugin: on when named
        server = build(tmp, plugins=["commands", "modes"])
        loaded = [n for n, _ in server.plugins.plugins]
        check("modes plugin loads when named", loaded == ["commands", "modes"], (loaded, server.plugins.disabled))
        bob = join(server, 1, "Bob")
        before = dict(server.cfg)
        reply = server.plugins.run_command(d.Player(bob, server.clients[bob]), "!mode")
        check("!mode names TAKE A RIDE", reply is not None and "0 (TAKE A RIDE)" in reply, reply)
        check("!mode changes nothing", dict(server.cfg) == before)

        server = build(tmp, plugins=["modes"], gamemode=9)
        bob = join(server, 1, "Bob")
        reply = server.plugins.run_command(d.Player(bob, server.clients[bob]), "!mode")
        check("an unknown id is reported, not refused", reply is not None and "9 (-" in reply
              and int(server.cfg["gamemode"]) == 9, reply)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
