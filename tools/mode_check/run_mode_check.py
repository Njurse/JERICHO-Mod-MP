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
    """test_mp_mode's MODEROW lines (id timer music) vs the Python mirror."""
    import mp_test
    rows = {int(i): (int(t), int(mu)) for i, t, mu in re.findall(r"^MODEROW (\d+) (\d+) (\d+)$", output, re.M)}
    py = {i: (mp_test.GAMEMODE_TIMER.get(i), mp_test.GAMEMODE_MUSIC.get(i)) for i in mp_test.GAMEMODE_NAMES}
    ok = bool(rows) and rows == py
    print("mode clock/music: C %r vs mp_test %r -> %s" % (rows, py, "OK" if ok else "MISMATCH"), flush=True)
    return ok


def mark_scripts():
    """Referee scripts both twins run. Fixed ones for the rules, then seeded random
    ones so an ordering slip in either twin shows up as a byte difference."""
    import random
    fox, bomb = "1", "2"
    scripts = [
        # Fox N Hounds: one player waits, the second starts it, a tag in the cooldown
        # is refused, a tag after it moves the fox, no bounce, first to 0:00 wins,
        # intermission, the next round.
        ["reset %s 3" % fox, "sync 0 2", "tick 5000 0", "sync 0 2 5", "tick 33 1",
         "tick 1000 0", "contact 2 5", "tick 1000 0", "contact 2 5", "contact 5 2",
         "tick 1999 0", "contact 5 2", "tick 1 0", "contact 5 2", "sync 0 2 5 7",
         "tick 2500 0", "contact 2 7", "tick 170000 0", "tick 9999 0", "tick 1 2",
         "sync 1 2 5", "sync 0 2", "tick 33 0", "tick 10000 0"],
        # Car Bomb Tag: shared clock, pass, no bounce, late joiner, the holder at 0:00.
        ["reset %s 1" % bomb, "sync 0 1 4", "tick 0 0", "tick 2000 0", "contact 1 4",
         "contact 1 4", "tick 1000 0", "sync 0 1 4 9", "tick 57000 0", "tick 10000 3"],
        # Take a Ride: inert; and the minutes clamp.
        ["reset 0 3", "sync 0 0 1 2", "tick 1000 0", "contact 0 1"],
        ["reset %s 0" % fox, "reset %s 9" % bomb, "sync 0 3 4", "tick 1 0"],
    ]
    rng = random.Random(0x7A6)
    for n in range(40):
        mode = rng.choice([fox, bomb])
        steps = ["reset %s %d" % (mode, rng.randint(0, 6))]
        ids = set()
        for _ in range(120):
            r = rng.random()
            if r < 0.12:
                ids ^= {rng.randrange(0, 6)}
                steps.append("sync %d %s" % (rng.randrange(1000), " ".join(str(i) for i in sorted(ids))))
            elif r < 0.45:
                steps.append("contact %d %d" % (rng.randrange(0, 6), rng.randrange(0, 6)))
            else:
                steps.append("tick %d %d" % (rng.choice([0, 33, 34, 500, 2000, 15000, 61000]), rng.randrange(1000)))
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
        elif t[0] == "tick":
            ev = ref.tick(int(t[1]), int(t[2]))
        elif t[0] == "contact":
            ev = 1 if ref.contact(int(t[1]), int(t[2])) else 0
        out.append("STEP %d %d %s" % (n, ev, ref.to_bytes().hex()))
    return out


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
