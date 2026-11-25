"""The game-mode scaffold on the dedicated server.

  * mode 0 is what the server has always sent: "gamemode" defaults to 0 and is
    named TAKE A RIDE, and a config file WITHOUT the key still loads with it;
  * no new DEFAULTS key came with modes (an upgrade must not rewrite a config);
  * the modes plugin loads by default; !mode shows the mode to anyone, and an
    admin (or the console) switches it by id or name: round state and referee
    restart at round 0 of the new mode and every player is told to rejoin;
  * build_config takes a mode by name and refuses an unknown one (falls back to 0).

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
    check("mode 0 is TAKE A RIDE", m.mode_name(0) == "Take a Ride")
    check("the modes are 0 TAKE A RIDE, 1 FOX N HOUNDS, 2 CAR BOMB TAG, 3 COPS N ROBBERS",
          m.GAMEMODE_NAMES == {0: "Take a Ride", 1: "Fox n Hounds", 2: "Car Bomb Tag",
                               3: "Cops n Robbers"}, m.GAMEMODE_NAMES)
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

        cfg3, _p = d.build_config(["--config", path, "--gamemode", "copsnrobbers"])
        check("--gamemode by name", int(cfg3["gamemode"]) == 3, cfg3.get("gamemode"))
        cfg4, _p = d.build_config(["--config", path, "--gamemode", "9"])
        check("an unknown --gamemode falls back to 0", int(cfg4["gamemode"]) == 0, cfg4.get("gamemode"))
        for name, gid in (("Fox n Hounds", 1), ("carbombtag", 2), ("cnr", 3), ("2", 2), ("ride", 0)):
            check("parse_gamemode(%r)" % name, d.parse_gamemode(name) == gid)
        try:
            d.parse_gamemode("racing"); ok = False
        except ValueError:
            ok = True
        check("parse_gamemode refuses an unknown name", ok)

        server = build(tmp)
        loaded = [n for n, _ in server.plugins.plugins]
        check("modes plugin loads by default", "modes" in loaded, loaded)
        check("status names the gamemode", "gamemode:  0" in server.cmd_status(""))

        bob = join(server, 1, "Bob")
        before = dict(server.cfg)
        bp = d.Player(bob, server.clients[bob])
        reply = server.plugins.run_command(bp, "!mode")
        check("!mode names TAKE A RIDE", reply is not None and "0 (Take a Ride)" in reply, reply)
        reply = server.plugins.run_command(bp, "!mode foxnhounds")
        check("a non-admin cannot switch", int(server.cfg["gamemode"]) == 0 and dict(server.cfg) == before, reply)

        kicked = []
        server.kick_player = lambda pid, reason=None, **kw: kicked.append((pid, kw.get("rejoin")))
        server.clients[bob]["admin"] = True
        for arg, gid, timed in (("foxnhounds 4", 1, True), ("carbombtag", 2, True),
                                ("3 2", 3, True), ("takearide", 0, False)):
            kicked.clear()
            reply = server.plugins.run_command(d.Player(bob, server.clients[bob]), "!mode " + arg)
            check("!mode %s -> %d" % (arg, gid), int(server.cfg["gamemode"]) == gid, reply)
            check("  round state and referee follow (%d)" % gid,
                  server.round.gamemode == gid and server.mark.gamemode == gid
                  and server.mark.active == timed)
            check("  a timer mode waits at round 0, Take a Ride is live at once",
                  (server.round.number == 0) if timed else (server.round.number >= 1),
                  server.round.number)
            check("  everyone is told to rejoin", kicked == [(1, True)], kicked)
        check("!mode foxnhounds 4 set the time", True)
        server.plugins.run_command(d.Player(bob, server.clients[bob]), "!mode fox 4")
        check("the referee uses the new time", server.mark.minutes == 4, server.mark.minutes)
        reply = server.plugins.run_command(d.Player(bob, server.clients[bob]), "!modetime 2")
        check("!modetime 2", server.mark.minutes == 2 and int(server.cfg["mode_time"]) == 2
              and int(server.cfg["gamemode"]) == 1, reply)
        reply = server.plugins.run_command(d.Player(bob, server.clients[bob]), "!mode racing")
        check("!mode refuses an unknown mode", "unknown" in (reply or "") and int(server.cfg["gamemode"]) == 1, reply)

        server = build(tmp, plugins=["modes"], gamemode=9)
        bob = join(server, 1, "Bob")
        reply = server.plugins.run_command(d.Player(bob, server.clients[bob]), "!mode")
        check("an unknown id from an embedder is reported, not refused", reply is not None and "9 (-" in reply
              and int(server.cfg["gamemode"]) == 9, reply)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # the browser flags in the HIGH byte of the advertised rules (mp_proto.h MP_ADV_*)
    tmp2 = tempfile.mkdtemp()
    try:
        srv = build(tmp2)
        r = srv.advert_rules()
        check("an advertised dedicated server carries ADV_DEDICATED", bool(r & m.ADV_DEDICATED), hex(r))
        check("no password -> no lock flag", not (r & m.ADV_PASSWORD), hex(r))
        check("the session's own rules (low byte) are untouched",
              (r & 0xFF) == (int(srv.cfg["rules"]) & 0xFF), hex(r))
        srv.cfg["password"] = "hunter2"
        check("a password sets the lock flag", bool(srv.advert_rules() & m.ADV_PASSWORD))
        check("flags never reach the 8-bit rules the session enforces", int(srv.cfg["rules"]) < 0x100,
              srv.cfg["rules"])
    finally:
        shutil.rmtree(tmp2, ignore_errors=True)

    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
