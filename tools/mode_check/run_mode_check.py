#!/usr/bin/env python3
"""Build and run the game-mode checks: test_mp_mode.cpp (the registry),
test_mp_round.cpp (the round state) and test_mp_mark.cpp (the Fox N Hounds / Car
Bomb Tag referee), each against mp_mode.c + mp_round.c + mp_mark.c.

    python tools/mode_check/run_mode_check.py [--cxx "clang++"]

The compiler is --cxx, else $CXX, else the first of c++ / clang++ / g++ on PATH,
else `python -m ziglang c++` when the ziglang package is installed. Exit 0 = every
mode-0 answer is what the session did before modes existed.

Also checks, without a compiler, that the mode names in mp_mode.c match the Python
servers' mirror (tools/mp_test.py GAMEMODE_NAMES), and, with one, that the C round
snapshot is byte for byte what mp_test.build_round makes, that each mode's clock
and music match mp_test.GAMEMODE_TIMER / GAMEMODE_MUSIC, and that the C referee
and the dedicated server's (dedi/markref.py) produce the same JPMK bytes after
every step of the same scripts.
"""
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MP = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(MP, "tools"))


def c_mode_names():
    """{id: name} from the kModes table in mp_mode.c (id macro resolved via mp_proto.h)."""
    proto = open(os.path.join(MP, "mp_proto.h"), encoding="utf-8").read()
    consts = {}
    m = re.search(r"enum\s*\{([^}]*MP_GAMEMODE_TAKEADRIDE[^}]*)\}", proto)
    if m:
        n = 0
        for part in m.group(1).split(","):
            part = re.sub(r"/\*.*?\*/", "", part, flags=re.S).strip()
            if not part:
                continue
            if "=" in part:
                k, v = (s.strip() for s in part.split("=", 1))
                n = int(v, 0)
            else:
                k = part
            consts[k] = n
            n += 1
    src = open(os.path.join(MP, "mp_mode.c"), encoding="utf-8").read()
    rows = re.findall(r"\{\s*(MP_GAMEMODE_\w+|\d+)\s*,\s*/\*\s*id\s*\*/\s*\"([^\"]*)\"", src)
    return {consts.get(i, None) if not i.isdigit() else int(i): name for i, name in rows}


def check_names():
    import mp_test
    c = c_mode_names()
    py = dict(mp_test.GAMEMODE_NAMES)
    ok = bool(c) and None not in c and c == py
    print("names: mp_mode.c %r vs mp_test.GAMEMODE_NAMES %r -> %s" % (c, py, "OK" if ok else "MISMATCH"), flush=True)
    ok = ok and mp_test.mode_name(0) == "TAKE A RIDE"
    return ok


def check_snapshot(output):
    """The C round snapshot (test_mp_round's SNAPSHOT line) must be the bytes the
    Python servers build for the same state: Take a Ride round 1, live, players 0
    and 1 playing, no team, score 0."""
    import mp_test
    want = mp_test.build_round(0, mp_test.ROUND_PHASE_LIVE, 1,
                               [(0, mp_test.ROUND_PLAYER_PLAYING, -1, 0),
                                (1, mp_test.ROUND_PLAYER_PLAYING, -1, 0)]).hex()
    got = re.search(r"^SNAPSHOT ([0-9a-f]+)$", output, re.M)
    ok = got is not None and got.group(1) == want
    print("round snapshot: C %s vs mp_test.build_round %s -> %s"
          % (got.group(1) if got else None, want, "OK" if ok else "MISMATCH"), flush=True)
    return ok


def check_mode_rows(output):
    """test_mp_mode's MODEROW lines (id timer music countdown lateJoin respawn) vs the Python mirror."""
    import mp_test
    rows = {int(i): (int(t), int(mu), int(cd), int(lj), int(rs)) for i, t, mu, cd, lj, rs in
            re.findall(r"^MODEROW (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)$", output, re.M)}
    py = {i: (mp_test.GAMEMODE_TIMER.get(i), mp_test.GAMEMODE_MUSIC.get(i), mp_test.GAMEMODE_COUNTDOWN.get(i),
              mp_test.GAMEMODE_LATEJOIN.get(i), mp_test.GAMEMODE_RESPAWN.get(i))
          for i in mp_test.GAMEMODE_NAMES}
    ok = bool(rows) and rows == py
    print("mode clock/music/countdown/latejoin/respawn: C %r vs mp_test %r -> %s" % (rows, py, "OK" if ok else "MISMATCH"), flush=True)
    return ok


def mark_scripts():
    """Referee scripts both twins run. Fixed ones for the rules, then seeded random
    ones so an ordering slip in either twin shows up as a byte difference."""
    import random
    fox, bomb, cnr = "1", "2", "3"
    everyone = " ".join(str(i) for i in range(16))
    scripts = [
        # The round start: two present but a car not picked -> no start; every car
        # in -> COUNTDOWN, whose time touches no clock and allows no tag; a joiner
        # without a car aborts it; GO; intermission -> the same gate again.
        ["reset %s 2" % fox, "sync 0 3 6", "ready 3", "tick 5000 0", "tick 5000 0",
         "ready 3 6", "tick 33 0", "contact 3 6", "tick 1000 0", "sync 0 3 6 8",
         "tick 33 0", "ready 3 6 8", "tick 33 0", "tick 3999 0", "contact 3 6",
         "tick 1 1", "tick 1000 0", "tick 1000 0", "contact 3 6", "tick 118000 0",
         "tick 5000 0", "tick 5000 0", "tick 4000 0", "tick 1 2", "sync 0 3",
         "tick 33 0", "tick 10000 0", "sync 0 3 9", "tick 33 0", "ready 3 9", "tick 33 0",
         "sync 0 9", "tick 33 0"],
        ["reset %s 1" % bomb, "ready 1 4", "sync 0 1", "tick 500 0", "sync 0 1 4",
         "tick 0 0", "tick 2000 0", "tick 2000 0", "contact 1 4", "tick 2000 0",
         "contact 1 4", "tick 60000 0", "tick 10000 0", "ready 1", "tick 33 0",
         "ready 1 4", "tick 33 0", "tick 4000 1"],
        # Join during WAITING and during COUNTDOWN (t515): a joiner without a
        # car is counted in the wait and aborts a countdown already running;
        # once they are ready the first-load count starts (or restarts).
        ["reset %s 2" % fox, "sync 0 3", "ready 3", "tick 1000 0", "sync 0 3 6",
         "tick 33 0", "ready 3 6", "tick 33 0", "sync 0 3 6 8", "tick 33 0",
         "ready 3 6 8", "tick 33 0", "tick 5000 1"],
        ["reset %s 2" % bomb, "ready 1 4", "sync 0 1 4", "tick 0 0", "tick 2000 0",
         "sync 0 1 4 9", "tick 33 0", "ready 1 4 9", "tick 33 0", "tick 2500 0",
         "tick 2500 0"],
        # Pre-game wait (t515/t516): a map load sits in WAITING until the mode's
        # wait is satisfied, then one first-load 5-4-3-2-1; later rounds on the
        # same map use the shorter count; everyone leaving re-arms the first-load.
        ["reset %s 2" % bomb, "ready %s" % everyone, "tick 9000 0", "sync 0 1", "tick 33 0",
         "tick 1000 0", "contact 1 4", "sync 0 1 4", "tick 2000 0", "sync 0", "tick 3000 0",
         "sync 0 1 4", "tick 4999 0", "tick 1 0", "tick 4000 0", "tick 120000 0",
         "tick 10000 0", "tick 4000 1", "sync 0", "tick 33 0", "tick 10000 0", "sync 0 4 1",
         "tick 33 0", "tick 2500 0", "tick 2500 0", "tick 4000 0"],
        ["reset %s 3" % fox, "sync 0 2 5", "ready 2", "tick 0 0", "tick 5000 0", "tick 33 0",
         "ready 2 5", "tick 33 0", "tick 4000 1", "sync 0 5", "tick 33 0", "tick 10000 0",
         "sync 0", "sync 0 2 5", "tick 33 0", "tick 5000 0", "tick 4000 0"],
        # Fox N Hounds: one player waits, the second starts it, a tag in the cooldown
        # is refused, a tag after it moves the fox, no bounce, first to 0:00 wins,
        # intermission, the next round.
        ["reset %s 3" % fox, "ready %s" % everyone, "sync 0 2", "tick 5000 0",
         "sync 0 2 5", "tick 33 1", "tick 4000 1",
         "tick 1000 0", "contact 2 5", "tick 1000 0", "contact 2 5", "contact 5 2",
         "tick 1999 0", "contact 5 2", "tick 1 0", "contact 5 2", "sync 0 2 5 7",
         "tick 2500 0", "contact 2 7", "tick 170000 0", "tick 9999 0", "tick 1 2",
         "sync 1 2 5", "sync 0 2", "tick 33 0", "tick 10000 0"],
        # Car Bomb Tag: shared clock, pass, no bounce, late joiner, the holder at 0:00.
        ["reset %s 1" % bomb, "ready %s" % everyone, "sync 0 1 4", "tick 0 0",
         "tick 4000 0", "tick 2000 0", "contact 1 4",
         "contact 1 4", "tick 1000 0", "sync 0 1 4 9", "tick 57000 0", "tick 10000 3"],
        # Cops n Robbers: the getaway is named at the COUNTDOWN (random first, kept
        # across an abort), no tags, hits score with a per-cop cooldown and fill the
        # heat meter, drive points carry their remainder, 0:00 = the getaway wins,
        # then the next id in turn; a bust; an escape; the getaway leaving.
        ["reset %s 1" % cnr, "ready %s" % everyone, "sync 0 2 5 7", "tick 0 77", "sync 0 2 5 7 9",
         "tick 33 0", "ready 2 5 7", "tick 33 0", "tick 33 0", "ready %s" % everyone, "tick 33 0",
         "tick 4999 5", "hit 2 5 9", "tick 1 6", "contact 2 5", "contact 5 7", "drive 1000 0",
         "drive 1000 300", "drive 333 150", "drive 333 150", "drive 334 150", "hit 2 5 9",
         "hit 5 7 30", "hit 7 5 20", "hit 7 5 20", "hit 2 9 0", "tick 999 0", "hit 7 5 20",
         "tick 1 0", "hit 5 7 20", "escape", "tick 60000 0", "drive 100 0", "tick 9999 0",
         "tick 1 0", "tick 3000 3", "tick 33 0"],
        ["reset %s 1" % cnr, "ready %s" % everyone, "sync 0 1 4", "tick 0 0", "tick 5000 0",
         "hit 1 4 20", "tick 1000 0", "hit 1 4 20", "tick 1000 0", "hit 4 1 20", "tick 1000 0",
         "hit 1 4 20", "tick 1000 0", "hit 1 4 20", "tick 1000 0", "hit 1 4 20", "tick 1000 0",
         "hit 1 4 20", "tick 1000 0", "hit 1 4 20", "tick 1000 0", "hit 1 4 20", "tick 1000 0",
         "hit 1 4 20", "hit 1 4 20", "tick 10000 0", "tick 3000 0", "escape", "tick 1000 0",
         "sync 0 1 4 6", "tick 33 0", "tick 10000 0", "tick 3000 0", "sync 0 1 6", "tick 33 0",
         "sync 0 1 4 6", "tick 10000 0", "tick 3000 0"],
        # 0.23.4 Cops n Robbers on real damage: contacts before the first report
        # still score (an older getaway build), the first report switches the
        # meter to damage/maxDamage, a contact names the cop credited within
        # CREDIT_MS, a repair keeps the meter, a full wreck is a bust.
        ["reset %s 1" % cnr, "ready %s" % everyone, "sync 0 1 4 6", "tick 0 0", "tick 5000 0",
         "hit 4 1 10", "damage 4 0 30000", "damage 1 0 30000", "hit 1 4 10", "tick 500 0",
         "damage 1 3000 30000", "tick 2500 0", "damage 1 4500 30000", "hit 6 1 3",
         "damage 1 4510 30000", "damage 1 2000 0", "damage 1 1000 30000", "tick 1000 0",
         "damage 1 1100 30000", "hit 1 6 20", "damage 1 29000 30000", "hit 1 4 20",
         "damage 1 30000 30000", "tick 10000 0", "damage 1 100 30000"],
        ["reset %s 1" % cnr, "ready %s" % everyone, "sync 0 2 5", "tick 0 0", "tick 5000 0",
         "damage 2 12345 30000", "hit 5 2 4", "damage 2 99999 30000", "tick 1 0"],
        # 0.23.4 Car Bomb Tag leavers: the holder leaving with 2+ left -> a random
        # new holder with a 12 s grace (no pass, the clock raised to 12 s); with
        # fewer than 2 left -> the round is void, nobody marked (no bomb goes off);
        # a non-holder leaving the holder alone -> void too.
        ["reset %s 1" % bomb, "ready %s" % everyone, "sync 0 1 4 6", "tick 0 0", "tick 5000 0",
         "tick 55000 0", "sync 7 4 6", "tick 3000 0", "contact 4 6", "contact 6 4",
         "tick 9000 0", "contact 4 6", "contact 6 4", "tick 1 0", "contact 4 6", "contact 6 4",
         "tick 12000 0", "tick 10000 0", "tick 4000 0", "sync 3 4", "tick 33 0", "tick 10000 0"],
        ["reset %s 1" % bomb, "ready %s" % everyone, "sync 0 1 4", "tick 0 0", "tick 5000 0",
         "tick 1000 0", "sync 0 1", "tick 33 0", "sync 0 1 4", "tick 10000 0", "tick 4000 0",
         "sync 0 4", "tick 33 0"],
        ["reset %s 2" % fox, "ready %s" % everyone, "sync 0 2 5", "tick 0 0", "tick 5000 0",
         "sync 0 5", "tick 33 0"],
        # Proto 24: a join after GO is benched (WAITING) and never tagged / hit;
        # the marked player leaving a waiter alone is void; the next countdown
        # puts the waiter in; out benches (the getaway out = bust, the holder out
        # = a new holder with the grace); every end carries its reason.
        ["reset %s 2" % fox, "ready %s" % everyone, "sync 0 2 5", "tick 0 0", "tick 5000 1",
         "sync 0 2 5 7", "tick 3000 0", "contact 5 7", "contact 2 7", "tick 2000 0",
         "contact 2 5", "sync 0 2 7", "tick 33 0", "tick 10000 0", "tick 33 0", "tick 4000 0"],
        ["reset %s 1" % bomb, "ready %s" % everyone, "sync 0 1 4 6", "tick 0 0", "tick 5000 0",
         "out 1 1", "out 1 0", "contact 4 6", "tick 13000 0", "sync 0 1 4 6 9", "out 9 0",
         "out 6 0", "tick 33 0", "tick 10000 0", "tick 4000 0", "out 4 0"],
        ["reset %s 1" % cnr, "ready %s" % everyone, "sync 0 1 4 6", "out 4 0", "tick 0 0",
         "tick 5000 0", "out 4 0", "hit 1 4 20", "hit 1 6 20", "damage 1 3000 30000",
         "sync 0 1 4 6 8", "hit 8 1 20", "tick 33 0", "out 1 0", "tick 10000 0", "tick 4000 0",
         "escape", "tick 10000 0", "tick 4000 0", "sync 0 4 6 8", "tick 33 0"],
        # Take a Ride: inert; and the minutes clamp.
        ["reset 0 3", "sync 0 0 1 2", "tick 1000 0", "contact 0 1"],
        ["reset %s 0" % fox, "reset %s 9" % bomb, "sync 0 3 4", "tick 1 0"],
    ]
    rng = random.Random(0x7A6)
    for n in range(40):
        mode = rng.choice([fox, bomb, cnr])
        steps = ["reset %s %d" % (mode, rng.randint(0, 6))]
        ids = set()
        ready = set(rng.sample(range(6), rng.randint(0, 6)))
        steps.append("ready %s" % " ".join(str(i) for i in sorted(ready)))
        for _ in range(120):
            r = rng.random()
            if r < 0.12:
                ids ^= {rng.randrange(0, 6)}
                steps.append("sync %d %s" % (rng.randrange(1000), " ".join(str(i) for i in sorted(ids))))
            elif r < 0.20:
                ready ^= {rng.randrange(0, 6)}
                steps.append("ready %s" % " ".join(str(i) for i in sorted(ready)))
            elif r < 0.45:
                steps.append("contact %d %d" % (rng.randrange(0, 6), rng.randrange(0, 6)))
            elif mode == cnr and r < 0.55:
                steps.append("hit %d %d %d" % (rng.randrange(0, 6), rng.randrange(0, 6), rng.randrange(-2, 30)))
            elif mode == cnr and r < 0.62:
                steps.append("drive %d %d" % (rng.choice([0, 33, 34, 500, 1000]), rng.randrange(-10, 400)))
            elif mode == cnr and r < 0.63:
                steps.append("escape")
            elif r < 0.66 and r >= 0.63:
                steps.append("out %d %d" % (rng.randrange(0, 6), rng.randrange(1000)))
            elif mode == cnr and r < 0.70:
                steps.append("damage %d %d %d" % (rng.randrange(0, 6), rng.choice([0, 1, 500, 3333, 15000, 29999, 30000, 40000]),
                                                 rng.choice([0, 1000, 30000])))
            else:
                steps.append("tick %d %d" % (rng.choice([0, 33, 34, 500, 1000, 2000, 3999, 4000, 5000, 15000, 61000]), rng.randrange(1000)))
        scripts.append(steps)
    return scripts


def python_mark_run(steps):
    sys.path.insert(0, os.path.join(MP, "tools", "servers", "dediserver"))
    from dedi.markref import MarkReferee
    ref = MarkReferee()
    out = []
    for n, line in enumerate(steps):
        t = line.split()
        ev = 0
        if t[0] == "reset":
            ref.reset(int(t[1]), int(t[2]))
        elif t[0] == "sync":
            ev = ref.sync([int(i) for i in t[2:]], int(t[1]))
        elif t[0] == "ready":
            ref.set_ready([int(i) for i in t[1:]])
        elif t[0] == "tick":
            ev = ref.tick(int(t[1]), int(t[2]))
        elif t[0] == "contact":
            ev = 1 if ref.contact(int(t[1]), int(t[2])) else 0
        elif t[0] == "hit":
            pts, hev = ref.hit(int(t[1]), int(t[2]), int(t[3]))
            ev = pts * 256 + hev
        elif t[0] == "drive":
            ev = ref.drive(int(t[1]), int(t[2]))
        elif t[0] == "escape":
            ev = ref.escape()
        elif t[0] == "damage":
            pts, cop, dev = ref.damage(int(t[1]), int(t[2]), int(t[3]))
            ev = pts * 65536 + (cop + 1) * 256 + dev
        elif t[0] == "out":
            ev = ref.out(int(t[1]), int(t[2]))
        out.append("STEP %d %d %s" % (n, ev, ref.to_bytes().hex()))
    return out


def check_mark_constants():
    """The referee's timings: mp_mark.h vs dedi/markref.py."""
    sys.path.insert(0, os.path.join(MP, "tools", "servers", "dediserver"))
    from dedi import markref
    h = open(os.path.join(MP, "mp_mark.h"), encoding="utf-8").read()
    pairs = {"MP_MARK_TAG_COOLDOWN_MS": markref.TAG_COOLDOWN_MS,
             "MP_MARK_INTERMISSION_MS": markref.INTERMISSION_MS,
             "MP_MARK_COUNTDOWN_MS": markref.COUNTDOWN_MS,
             "MP_MARK_LOBBY_MS": markref.LOBBY_MS,
             "MP_MARK_EV_LOBBY": markref.EV_LOBBY,
             "MP_MARK_EV_BEGIN": markref.EV_BEGIN, "MP_MARK_EV_END": markref.EV_END,
             "MP_MARK_EV_MOVED": markref.EV_MOVED, "MP_MARK_EV_COUNTDOWN": markref.EV_COUNTDOWN,
             "MP_MARK_CNR_HIT_COOLDOWN_MS": markref.CNR_HIT_COOLDOWN_MS,
             "MP_MARK_CNR_HIT_MAX": markref.CNR_HIT_MAX, "MP_MARK_CNR_HIT_POINTS": markref.CNR_HIT_POINTS,
             "MP_MARK_CNR_BUST_POINTS": markref.CNR_BUST_POINTS,
             "MP_MARK_CNR_DRIVE_BASE": markref.CNR_DRIVE_BASE, "MP_MARK_CNR_DRIVE_BONUS": markref.CNR_DRIVE_BONUS,
             "MP_MARK_CNR_DRIVE_TOPSPEED": markref.CNR_DRIVE_TOPSPEED,
             "MP_MARK_CNR_ESCAPE_DIST": markref.CNR_ESCAPE_DIST,
             "MP_MARK_CNR_ESCAPE_HOLD_MS": markref.CNR_ESCAPE_HOLD_MS,
             "MP_MARK_CNR_CREDIT_MS": markref.CNR_CREDIT_MS,
             "MP_MARK_BOMB_GRACE_MS": markref.BOMB_GRACE_MS}
    ok = True
    for name, py in pairs.items():
        mm = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)\b" % name, h)
        c = int(mm.group(1), 0) if mm else None
        if c != py:
            print("mark constants: %s C %r vs markref %r -> MISMATCH" % (name, c, py), flush=True)
            ok = False
    if ok:
        print("mark constants: %d mp_mark.h value(s) == dedi/markref.py -> OK" % len(pairs), flush=True)
    return ok


def check_mark_lockstep(exe):
    scripts = mark_scripts()
    steps = 0
    for k, script in enumerate(scripts):
        run = subprocess.run([exe, "--script"], input="\n".join(script) + "\n",
                             stdout=subprocess.PIPE, universal_newlines=True)
        c = run.stdout.strip().splitlines()
        py = python_mark_run(script)
        if c != py:
            for a, b, line in zip(c, py, script):
                if a != b:
                    print("mark lockstep: script %d diverged at %r\n  C  %s\n  py %s" % (k, line, a, b), flush=True)
                    break
            else:
                print("mark lockstep: script %d: %d C lines vs %d Python" % (k, len(c), len(py)), flush=True)
            return False
        steps += len(script)
    print("mark lockstep: C mp_mark.c == dedi/markref.py over %d script(s), %d step(s) -> OK"
          % (len(scripts), steps), flush=True)
    return True


def find_cxx(arg):
    if arg:
        return shlex.split(arg)
    if os.environ.get("CXX"):
        return shlex.split(os.environ["CXX"])
    for c in ("c++", "clang++", "g++"):
        if shutil.which(c):
            return [c]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx")
    ap.add_argument("--names-only", action="store_true")
    a = ap.parse_args()

    ok = check_names()
    ok = check_mark_constants() and ok
    if a.names_only:
        return 0 if ok else 1

    cxx = find_cxx(a.cxx)
    if cxx is None:
        print("no C++ compiler found (set CXX or --cxx); the C check did not run", flush=True)
        return 2

    rc = 0
    with tempfile.TemporaryDirectory() as tmp:
        for test in ("test_mp_mode", "test_mp_round", "test_mp_mark"):
            exe = os.path.join(tmp, test + (".exe" if os.name == "nt" else ""))
            cmd = cxx + ["-std=gnu++14", "-I", MP, "-x", "c++",
                         os.path.join(HERE, test + ".cpp"),
                         os.path.join(MP, "mp_mode.c"),
                         os.path.join(MP, "mp_round.c"),
                         os.path.join(MP, "mp_mark.c"), "-o", exe]
            print("build:", " ".join(cmd), flush=True)
            if subprocess.call(cmd) != 0:
                print("build FAILED", flush=True)
                return 1
            run = subprocess.run([exe], stdout=subprocess.PIPE, universal_newlines=True)
            sys.stdout.write(run.stdout)
            sys.stdout.flush()
            rc = rc or run.returncode
            if test == "test_mp_round":
                ok = check_snapshot(run.stdout) and ok
            if test == "test_mp_mode":
                ok = check_mode_rows(run.stdout) and ok
            if test == "test_mp_mark":
                ok = check_mark_lockstep(exe) and ok
    return 0 if (ok and rc == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
