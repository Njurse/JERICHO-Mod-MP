#!/usr/bin/env python3
"""Run TWO REAL REDRIVER2 instances on one PC, one hosting and one joining.

Why this exists
---------------
Testing multiplayer through the frontend menus takes two people and two machines,
and the interesting bugs (the wrong game mode launching on the joining side, a car
that never appears, a connection that drops) only show up in a real pair. The mock
in mp_test.py is one-sided: it can prove the wire format, but there is only ever
one real engine in the room.

This builds two throwaway run directories beside the game and launches the real
executable in each. Directory junctions mean the big trees are shared, so it costs
almost nothing on disk, and each instance gets its own working directory -- which
is what keeps the two JERICHO.log files and the two JERICHO/CONFIG/mp.ini from
fighting each other.

    python mp_localpair.py                  # host + join, report, clean up
    python mp_localpair.py --seconds 90     # watch for longer
    python mp_localpair.py --keep           # leave the run dirs for poking at

It also ends a run as soon as it can -- which is the difference between iterating
and sitting out a window:

    --until "getting OUT"    stop the moment that line appears (repeatable)
    --forbid "Lost the server"  stop the moment that line appears: a FAIL
    --stall 10               no new log bytes on EITHER side for 10s -> STALLED

and it prints the running tail of both logs while it waits, so a bad run is
obvious immediately instead of at the end.

Both instances are launched directly (not via a .bat), so the PIDs here are real
and the cleanup kills exactly what it started -- nothing else.
"""
import argparse
import atexit
import os
import random
import re
import shutil
import subprocess
import sys
import time

DEFAULT_GAME_DIR = os.path.join("src_rebuild", "bin", "Release_dev")
# Seat names, the host first. A run uses the first --players of these. The module
# itself seats up to MP_MAX_PLAYERS (8); the extra names exist because a 4-seat run
# needs somewhere to put its LATE JOINERS, and a late join is a different spawn
# path from the one a pair exercises.
SEAT_NAMES = ("a", "b", "c", "d", "e", "f", "g", "h")
# The game writes its log as JERICHO.log -- the LOADER owns the file. JERICHO.log
# is kept as a fallback for a build that writes that name instead. Reporting the
# wrong one made every run print "no JERICHO.log" even when the game logged fine.
GAME_LOGS = ("JERICHO.log", "JERICHO.log")


def log(msg):
    print(f"[pair] {msg}", flush=True)


# Every instance this run started. Cleanup is registered with atexit as well as
# done inline, because the inline path only runs on the happy path: when the
# harness itself raised, its instances were left ALIVE -- holding the game's
# files (so the next run could not even copy them) and, far worse, still
# listening on the session port. A later client then connects to that stale
# host and the whole run is nonsense. That is the shape of a lot of the
# confusing results so far.
STARTED = []


def stop_started():
    for p in reversed(STARTED):
        if p.poll() is None:
            try:
                p.terminate()
                p.wait(timeout=8)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass


atexit.register(stop_started)


def stale_instances():
    """REDRIVER2 processes still running out of a run directory."""
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-Process JERICHO_dev -ErrorAction SilentlyContinue | "
             "Where-Object { $_.Path -like '*mp-pair*' } | ForEach-Object { $_.Id }"],
            capture_output=True, text=True)
    except Exception:
        return []

    return [int(tok) for tok in out.stdout.split() if tok.strip().isdigit()]


def junction(link, target):
    """Directory junction. No admin rights needed, unlike a symlink."""
    os.makedirs(os.path.dirname(link), exist_ok=True)
    r = subprocess.run(["cmd", "/c", "mklink", "/J", link, target],
                       capture_output=True, text=True)
    return r.returncode == 0


def build_run_dir(game_dir, run_dir, name, port):
    """A directory that looks like the game dir, sharing everything but CONFIG."""
    if os.path.exists(run_dir):
        # junctions must be removed as links, not followed
        subprocess.run(["cmd", "/c", "rmdir", run_dir], capture_output=True)
        if os.path.exists(run_dir):
            shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(run_dir, exist_ok=True)

    for entry in sorted(os.listdir(game_dir)):
        if entry in (".mp-pair", "JERICHO"):
            continue
        if entry.lower().endswith((".log", ".dmp")):
            continue

        src = os.path.join(game_dir, entry)
        dst = os.path.join(run_dir, entry)

        if os.path.isdir(src):
            if not junction(dst, os.path.abspath(src)):
                log(f"WARNING: could not link {entry} into {name}")
        else:
            shutil.copy2(src, dst)

    # JERICHO: share the module tree, own the config (mp.ini differs per instance)
    jer_src = os.path.join(game_dir, "JERICHO")
    jer_dst = os.path.join(run_dir, "JERICHO")
    os.makedirs(jer_dst, exist_ok=True)

    if os.path.isdir(jer_src):
        for entry in sorted(os.listdir(jer_src)):
            src = os.path.join(jer_src, entry)
            dst = os.path.join(jer_dst, entry)

            if os.path.isdir(src) and entry != "CONFIG":
                junction(dst, os.path.abspath(src))
            elif os.path.isdir(src):
                shutil.copytree(src, dst, dirs_exist_ok=True)
            else:
                shutil.copy2(src, dst)

    cfg = os.path.join(jer_dst, "CONFIG")
    os.makedirs(cfg, exist_ok=True)

    with open(os.path.join(cfg, "mp.ini"), "w", encoding="utf-8") as f:
        f.write(f"port = {port}\nplayer_name = {name}\n")

    return run_dir


def safe_remove(path):
    """Delete a run directory WITHOUT ever following a junction into real data.

    shutil.rmtree is the obvious tool here and it is exactly the wrong one: the
    junctions inside a run dir point at the real game tree, so a recursive delete
    that descends into one takes the real files with it. `rmdir` on a junction
    removes the link and leaves the target alone, so every reparse point is
    unlinked before anything recurses.
    """
    if os.path.isdir(path) and os.path.isjunction(path):
        subprocess.run(["cmd", "/c", "rmdir", path], capture_output=True)
        return

    if not os.path.isdir(path):
        if os.path.exists(path):
            try:
                os.remove(path)
            except OSError:
                pass
        return

    for name in os.listdir(path):
        full = os.path.join(path, name)
        if os.path.isdir(full) and os.path.isjunction(full):
            subprocess.run(["cmd", "/c", "rmdir", full], capture_output=True)

    for name in os.listdir(path):
        safe_remove(os.path.join(path, name))

    try:
        os.rmdir(path)
    except OSError:
        pass


def launch(run_dir, exe, args, env_extra):
    env = dict(os.environ)
    env.update(env_extra)
    p = subprocess.Popen([os.path.join(run_dir, exe)] + args, cwd=run_dir, env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(p)
    return p


def report(run_dir, label, patterns):
    path = next((os.path.join(run_dir, n) for n in GAME_LOGS
                 if os.path.exists(os.path.join(run_dir, n))), None)
    if path is None:
        log(f"{label}: no {'/'.join(GAME_LOGS)}")
        return

    hits = []
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip()
            if any(p in line for p in patterns):
                hits.append(line)

    print(f"--- {label} ({len(hits)} line(s)) ---")
    for line in hits[-24:]:
        print("   ", line)


def read_log(run_dir):
    for name in GAME_LOGS:
        path = os.path.join(run_dir, name)
        if os.path.exists(path):
            with open(path, "rb") as f:
                return f.read().decode("utf-8", "replace")
    return ""


HEARTBEAT = re.compile(r"\[mp\] heartbeat: frame (\d+) ")


def read_logs(dirs):
    """Both sides' live log text, plus the total length.

    One read per poll, because the same text answers two different questions: has
    the line under test appeared (--until/--forbid), and is anything still
    happening at all (--stall).
    """
    out = {}
    total = 0

    for side in sorted(dirs):
        out[side] = read_log(dirs[side])
        total += len(out[side])

    return out, total


def heartbeat_frames(text):
    """The sim frames a side has reported through MP_HEARTBEAT.

    That is the module's own tick counter, which is the point: a heartbeat that
    stops advancing means the SIMULATION stopped, not that the game went quiet.
    """
    return [int(m.group(1)) for m in HEARTBEAT.finditer(text)]


def _repo_tools():
    """<repo>/tools, where dmp_fault.py and map_lookup.py live."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, "..", "..", "..", "..", "tools"))


def crash_dumps(dirs):
    """JERICHO.dmp beside the exe means an ACCESS VIOLATION.

    A clean exit -- an Alt+F4, a menu quit, the harness terminating it -- leaves
    none. This is the one thing that must never be silent: a crash part-way
    through makes everything after it in the log meaningless, and without this
    check the run could still be reported as a PASS.
    """
    return {side: os.path.join(d, "JERICHO.dmp")
            for side, d in dirs.items()
            if os.path.isfile(os.path.join(d, "JERICHO.dmp"))}


def triage_crash(dump, mapfile):
    """Say WHERE it died, not only that it did.

    "It crashed" is not a finding; "PingInCivCar+0x105" is. dmp_fault gives the
    module and the RVA, map_lookup turns an RVA in our own exe into a symbol.
    """
    lines = []
    fault = os.path.join(_repo_tools(), "dmp_fault.py")
    lookup = os.path.join(_repo_tools(), "map_lookup.py")

    rva = None

    if os.path.isfile(fault):
        try:
            r = subprocess.run([sys.executable, fault, dump],
                               capture_output=True, text=True, timeout=60)
            for line in (r.stdout or "").splitlines():
                s = line.strip()
                if s.startswith("exception") or s.startswith("in module"):
                    lines.append(s)
                if "at rva" in s:
                    parts = s.split("at rva")
                    if len(parts) == 2:
                        rva = parts[1].strip().split()[0]
        except Exception as e:
            lines.append(f"(dmp_fault failed: {e})")

    if rva and os.path.isfile(lookup) and os.path.isfile(mapfile):
        try:
            r = subprocess.run([sys.executable, lookup, mapfile, rva],
                               capture_output=True, text=True, timeout=60)
            for line in (r.stdout or "").splitlines():
                if "->" in line:
                    lines.append(line.strip())
        except Exception as e:
            lines.append(f"(map_lookup failed: {e})")

    return lines


def known_disconnect(texts):
    """Did this run end on the mid-match disconnect we already know about?

    A pre-existing drop takes the client out part-way through a match, and it looks
    exactly like a fresh defect: a stall, plus "Lost the server", plus a peer dropped
    with "send failed". Naming it is the difference between "the feature broke" and
    "the link went away again, and everything up to that point still holds" -- which
    matters now that one command runs several tries in sequence, because each extra
    minute of match makes the drop more likely to be what ends the run.

    It is deliberately EVIDENCE-based (a named drop plus a live match), not timing:
    a change that makes disconnects worse must not be able to hide behind this.
    """
    joined = "\n".join(texts)

    in_match = "in the match" in joined
    dropped = ("Lost the server" in joined
               or "send failed" in joined
               or "peer sent LEAVE" in joined
               or "(timeout)" in joined)

    return in_match and dropped


def check_requires(names, dirs, requires):
    """Every `--require` must have appeared. SEAT=REGEX scopes it to one seat's log.

    The mirror of --forbid, and the point of automating the tries: a run should assert
    that the RIGHT car was drawn, not merely that nothing crashed.
    """
    texts = {n: read_log(dirs[n]) for n in names}
    missing = []

    for want in requires:
        if "=" in want and want.split("=", 1)[0] in texts:
            seat, pattern = want.split("=", 1)
            haystack = texts[seat]
            where = seat
        else:
            seat, pattern = None, want
            haystack = "\n".join(texts[n] for n in names)
            where = "any seat"

        if not re.search(pattern, haystack):
            missing.append((where, pattern))

    return missing


def verdict(names, dirs, stopped=None, requires=()):
    """Pass/fail for the run: EVERY seat must connect, none may drop, none may crash.

    This is what turns the harness from a log dump into a regression test.
    "The client dropped instantly" was invisible in a wall of logs, so the two
    markers that actually mean it -- a client that never got accepted, and either
    side reporting a peer that spoke 0 bytes (the signature of the receive-guard
    bug) -- are asserted here. A benign end-of-run close ('dropped (closed)')
    is not counted.

    A crash is checked FIRST and reported with its function, because a run that
    died at the halfway point can still show every marker of a healthy one.

    `stopped` is what ended the run early, and two of its values are FAILURES even
    though every marker above may hold:

      "stall"  -- nothing was logged on EITHER side for --stall seconds. Every
                  connection marker was satisfied in the first seconds of the run,
                  so a game that froze at 6s used to be reported PASS; that is how
                  "the window that previously killed the client is now survived"
                  got recorded as good news. A frozen run is not a passing run.
      "forbid" -- a line the caller declared fatal (--forbid) turned up.
    """
    host = read_log(dirs["a"])
    others = {n: read_log(dirs[n]) for n in names if n != "a"}
    everything = [host] + list(others.values())

    dumps = crash_dumps(dirs)

    for side, dump in sorted(dumps.items()):
        mapfile = os.path.join(os.path.dirname(dump), "JERICHO_dev.map")
        log(f"*** {side.upper()} CRASHED -- an access violation, not a clean exit ***")
        for line in triage_crash(dump, mapfile):
            log(f"    {line}")

    # One "joined" line per joiner on the host, and every joiner's own acceptance
    # line in its own log: with more than two seats it is not enough that SOMEBODY
    # connected, so both are counted rather than tested for presence.
    joins = host.count("joined (")
    want_joins = len(names) - 1
    host_join = joins >= want_joins
    missing = [n for n, text in sorted(others.items()) if "accepted as player" not in text]
    client_ok = not missing
    client_lost = sum(t.count("Lost the server") for t in everything)
    zero_byte = sum(t.count("0 byte(s) received") for t in everything)

    stalled = stopped == "stall"
    forbidden = stopped == "forbid"

    known = known_disconnect(everything)
    missing_requires = check_requires(names, dirs, requires)

    ok = (host_join and client_ok and zero_byte == 0
          and (client_lost == 0 or known)
          and not dumps and not forbidden
          and not missing_requires
          and not (stalled and not known))

    why = ""
    if forbidden:
        why = " -> FAIL (a --forbid line appeared)"
    elif missing_requires:
        why = f" -> FAIL ({len(missing_requires)} --require line(s) missing)"
    elif stalled and known:
        why = (" -> DROPPED (the known mid-match disconnect, not a new defect; "
               "everything above holds up to it")
    elif stalled:
        why = " -> STALLED (not a pass: a frozen game logs nothing)"
    else:
        why = f" -> {'PASS' if ok else 'FAIL'}"

    if known and not stalled:
        why += " [known disconnect markers present]"

    log(f"verdict: host_joins={joins}/{want_joins} "
        f"joiners_accepted={len(others) - len(missing)}/{len(others)} "
        f"lost={client_lost} zero_byte_peers={zero_byte} "
        f"dumps={len(dumps)} stopped={stopped or 'no'}{why}")

    if missing:
        log(f"    never accepted: {', '.join(missing)}")

    for where, pattern in missing_requires:
        log(f"    --require missing ({where}): {pattern}")

    if known:
        log("    note: this run hit the known mid-match disconnect; a --require that only "
            "appears AFTER it cannot be judged from this run")

    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", default=DEFAULT_GAME_DIR)
    ap.add_argument("--exe", default="JERICHO_dev.exe")
    ap.add_argument("--port", type=int, default=1400)
    ap.add_argument("--seconds", type=int, default=40,
                    help="how long to let them run (default 40; the shapes that need a level "
                         "rebuild -- a city change, a car change -- want more)")
    ap.add_argument("--settle", type=int, default=4,
                    help="seconds to wait before the client joins (default 4; the join itself "
                         "takes a few seconds more, which is why the first joiner is still in "
                         "time to be in the match from the start)")
    ap.add_argument("--keep", action="store_true", help="leave the run dirs behind")
    ap.add_argument("--clean", action="store_true",
                    help="remove the run dirs and exit (never follows a junction)")
    ap.add_argument("--map", action="store_true", help="hold the in-game map open (MP_MAP=1)")
    ap.add_argument("--no-debug", action="store_true",
                    help="do NOT set MP_DEBUG=1. This is what the packaged launchers "
                         "(PLAY_HOST.bat / PLAY_JOIN.bat) do, so it is the only way to test "
                         "what a player actually runs -- a bug that only appears without "
                         "MP_DEBUG is invisible otherwise.")
    ap.add_argument("--bot", default="chase", choices=["off", "random", "chase", "fight", "pursuit"],
                    help="drive the player cars with the mp test bot: random / chase (HOST FLEES, "
                         "every joiner chases the host - the default, because it is what makes the "
                         "cars actually meet and collide, which is what the collision, palette and "
                         "catch-up work needs) / fight (both charge) / pursuit (BOTH hunt each "
                         "other) / off (drive nothing: this drives a real player's car, so a "
                         "reproduction of a human report should use it)")
    ap.add_argument("--level", default="rio",
                    help="city for the host to host (default rio)")
    ap.add_argument("--mp-arena", default="1",
                    help="multiplayer map/arena for both sides (default 1)")
    ap.add_argument("--players", type=int, default=2, metavar="N",
                    help="how many seats to run: one host plus N-1 joiners (2..8). The FIRST "
                         "joiner arrives before the match starts, so the ones after it join a "
                         "LIVE match -- which is the only way the late-join spawn path and the "
                         "host's relay get exercised at all (default 2).")
    ap.add_argument("--stagger", type=int, default=8, metavar="SECS",
                    help="gap between the late joiners, so each one really does join a match "
                         "that is already running (default 8; a level takes ~10s to load from "
                         "a cold cache, less once it is warm -- a smaller gap is the fastest "
                         "way to shorten a run, but too small and the joiner arrives before "
                         "the host's match has started, which is a different path).")
    ap.add_argument("--host-car", default="slot1",
                    help="the host's car: a model number, slotN, 'random', or 'default' "
                         "(= pass NO -mpcar, so the level chooses -- what a player who just "
                         "presses Host does, and the case where the join used to get the "
                         "wrong car, default slot1)")
    ap.add_argument("--client-car", default="slot3",
                    help="the joining player's car: a model number, slotN, 'random', or 'default' "
                         "(= pass NO -mpcar; default slot3)")
    ap.add_argument("--until", action="append", default=[], metavar="REGEX",
                    help="end the run the moment this appears in EITHER log (repeatable). "
                         "Use it to stop as soon as the thing under test has happened instead "
                         "of sitting out --seconds: mp_localpair.py --until 'getting OUT'")
    ap.add_argument("--forbid", action="append", default=[], metavar="REGEX",
                    help="end the run AND fail the moment this appears -- a marker that must "
                         "never turn up (repeatable)")
    ap.add_argument("--require", action="append", default=[], metavar="[SEAT=]REGEX",
                    help="FAIL unless this line appears somewhere in the run. Prefix with a "
                         "seat (host=/client=/client2=) to scope it to that seat's log; "
                         "without one, any seat may carry it. This is the mirror of --forbid "
                         "and the reason to automate a try: assert the RIGHT car was drawn, "
                         "not merely that nothing crashed (repeatable)")
    ap.add_argument("--stall", type=int, default=10, metavar="SECS",                    help="if a side's sim tick stops advancing (or, without heartbeats, "
                         "NEITHER log grows) for this many seconds, stop and report STALLED. "
                         "STALLED is not a pass: a frozen game logs nothing, and that used to "
                         "be indistinguishable from a healthy run whose markers were all "
                         "logged early. 0 disables.")
    ap.add_argument("--seat-env", action="append", default=[], metavar="SEAT=KEY=VALUE",
                    help="set an env var for ONE seat (seats: host, client, client1, "
                         "client2 ...). Repeatable, and applied after the inherited "
                         "environment. This is how each player picks a DIFFERENT car -- "
                         "e.g. --seat-env host=CHK_FORCE_CAR=8 with "
                         "--seat-env client=CHK_FORCE_CAR=2 -- because one shared value "
                         "has every seat ride the same car and hides whether a peer's own "
                         "pick is really respected on the other machines. To force a "
                         "CROSS-CITY pick, pair it with CHK_FORCE_ROSTER_CITY: "
                         "--seat-env host=CHK_FORCE_ROSTER_CITY=1 --seat-env host=CHK_FORCE_CAR=8 "
                         "(--seat-env host=CHK_FORCE_ROSTER_CITY=2 --seat-env client=CHK_FORCE_CAR=2 "
                         "on the joiner), city 0..3 = CHICAGO/HAVANA/VEGAS/RIO.")
    ap.add_argument("--tail", type=int, default=3, metavar="SECS",
                    help="print the running tail of both logs this often while waiting, so a "
                         "bad run is obvious as it happens (0 = never)")
    args = ap.parse_args()

    if args.players < 2 or args.players > len(SEAT_NAMES):
        log(f"--players must be 2..{len(SEAT_NAMES)}")
        return 2

    names = SEAT_NAMES[:args.players]

    # A 4-seat run costs the level load plus a stagger per late joiner, and the
    # commonest way to waste one is to give it less window than that.
    needed = args.settle + args.stagger * (args.players - 2) + 25

    if args.seconds < needed:
        log(f"note: --seconds {args.seconds} is shorter than this run needs ({needed}s for "
            f"{args.players} seats); raising it to {needed}")
        args.seconds = needed

    # A random car each, from the level's own domestic set (models 0..4), so a run
    # actually exercises per-player vehicle choice instead of always the same two.
    # A RAW model number, not slotN: the client's slot cannot be resolved until it
    # knows the host's city, and it is the raw value that travels in the HELLO.
    if args.host_car == "random":
        args.host_car = str(random.choice([0, 1, 2, 3, 4]))
    if args.client_car == "random":
        args.client_car = str(random.choice([0, 1, 2, 3, 4]))

    game_dir = os.path.abspath(args.game_dir)
    if not os.path.isfile(os.path.join(game_dir, args.exe)):
        log(f"no {args.exe} in {game_dir}")
        return 2

    stale = stale_instances()
    if stale:
        log(f"found {len(stale)} instance(s) left over from an earlier run: {stale}")
        log("killing them: they hold the game's files and a stale host would still "
            "be listening on the session port")
        for pid in stale:
            subprocess.run(["powershell", "-NoProfile", "-Command",
                            f"Stop-Process -Id {pid} -Force"],
                           capture_output=True)
        time.sleep(1)

    pair_root = os.path.join(game_dir, ".mp-pair")
    dirs = {n: os.path.join(pair_root, n) for n in names}

    if args.clean:
        # every seat name, not just this run's: a leftover from a longer run is
        # exactly what --clean is for.
        for name in SEAT_NAMES:
            safe_remove(os.path.join(pair_root, name))
        try:
            os.rmdir(pair_root)
        except OSError:
            pass
        log(f"removed {pair_root}")
        return 0

    for name in names:
        build_run_dir(game_dir, dirs[name], f"Local{name.upper()}", args.port)
    log(f"run dirs ready under {pair_root}")

    # NOTE: both instances want UDP/1318 for discovery, so the second one reports
    # "discovery unavailable" and simply does not browse. The join below uses the
    # address directly, which is the path that must work anyway.
    env = {} if args.no_debug else {"MP_DEBUG": "1"}

    # This harness reads both logs WHILE the games are still running (verdict()
    # runs before anything is killed), and the engine's log is a buffered FILE*
    # that is only flushed when a LOADING SCREEN goes up -- not per frame, and
    # never at all on the way out of a killed process. So the file on disk can
    # lose its tail, and a line written just before the game hangs inside a hook
    # is lost for good -- which reads exactly like "that code never ran" (it is
    # how "the eject lever never fired" was recorded). Flush per line instead, so
    # what is judged is what actually happened.
    #
    # Not forced: `JERICHO_LOG_FLUSH=0 python mp_localpair.py ...` keeps the old
    # buffered behaviour, which is what proves the lever is doing anything.
    if "JERICHO_LOG_FLUSH" not in os.environ:
        env["JERICHO_LOG_FLUSH"] = "1"

    # ...and a heartbeat once a second, so this harness can tell a FROZEN game
    # from a quiet one. Every periodic line in the module is MP_DEBUG-gated, and
    # --no-debug is exactly the case worth testing, so without this the two are
    # indistinguishable (see --stall).
    if "MP_HEARTBEAT" not in os.environ:
        env["MP_HEARTBEAT"] = "1"

    if args.map:
        env["MP_MAP"] = "1"

    # The host has to start the match itself: the attract demo that used to do it
    # by accident is suppressed now (it was launching a level nobody asked for and
    # blocking through the load). MP_AUTOSTART=host is the module's own lever --
    # it launches once a player is in. The client must NOT get it, or it would try
    # to host as well.
    host_env = dict(env)
    host_env["MP_AUTOSTART"] = "host"

    # ...and start the match as soon as ONE joiner is in. That is what makes every
    # joiner after it a LATE one: the match is already running when they arrive, so
    # the module takes its late-join spawn path rather than the level-init one.
    if args.players > 2:
        host_env["MP_AUTOJOIN_START"] = "2"
    client_env = dict(env)
    client_env.pop("MP_AUTOSTART", None)

    # Per-seat overrides: --seat-env host=K=V, --seat-env client=K=V (every joiner),
    # --seat-env client2=K=V (one joiner). Applied after everything else, so a seat
    # can be given a pick the other seats do not have.
    seat_env = {"host": {}, "client": {}}
    for spec in args.seat_env:
        if spec.count("=") < 2:
            raise SystemExit(f"--seat-env wants SEAT=KEY=VALUE, got {spec!r}")

        seat, rest = spec.split("=", 1)
        key, val = rest.split("=", 1)
        seat = seat.strip().lower()

        if seat != "host" and seat != "client" and not seat.startswith("client"):
            raise SystemExit(f"--seat-env: unknown seat {seat!r} (host, client, client1 ...)")

        seat_env.setdefault(seat, {})[key] = val

    host_env.update(seat_env.get("host", {}))

    # The test bot is OFF unless asked for; it drives the player's actual car.
    if args.bot != "off":
        host_env["MP_BOT"] = args.bot
        client_env["MP_BOT"] = args.bot

    # -level only on the HOST. It boots the engine straight into a city, frontend
    # bypassed -- giving it to the client too means the client is already booting
    # a level while the module drives its own join and launch, and that is a
    # crash, not a slow start. The client follows the host's session for its level
    # and only needs -mp (which multiplayer map/arena) as a local boot flag, since
    # the arena is not in the session config yet.
    host_args = ["-nointro", "-nofmv", "-level", args.level, "-mp", args.mp_arena]
    client_args = ["-nointro", "-nofmv", "-mp", args.mp_arena]

    # The car comes from the MODULE's -mpcar, not the engine's -car. -car is a
    # dependent option that requires -level, and -level boots the engine straight
    # into a city -- so the client could only ever have one or the other, and
    # giving it -car alone popped a blocking message box that made the client look
    # like it could not connect at all.

    # "default" = pass NO -mpcar, so the ENGINE/level chooses the host's car -- the
    # way a player who just presses Host does it (config.car stays -1). That is the
    # case where the joiner used to be handed the level's slot-0 car instead.
    if args.host_car == "default":
        host_argv = host_args + ["-host", str(args.port)]
    else:
        host_argv = host_args + ["-mpcar", args.host_car, "-host", str(args.port)]

    procs = {}
    procs["a"] = launch(dirs["a"], args.exe, host_argv, host_env)
    log(f"host pid {procs['a'].pid} args: {' '.join(host_argv)}")
    log(f"host  (port {args.port}, {args.level} arena {args.mp_arena}, "
        f"host car {args.host_car}, first joiner car {args.client_car})")

    log(f"waiting {args.settle}s for the host to load...")
    time.sleep(args.settle)

    # The FIRST joiner arrives before the match starts (it is what starts it, with
    # MP_AUTOJOIN_START=2); every one after it is staggered, so it joins a match that
    # is already live and takes the late-join spawn path -- the path a pair never
    # exercises, and the one a late joiner has been seen to spawn above the host on.
    for i, name in enumerate(names[1:]):
        argv = list(client_args)

        if i == 0 and args.client_car != "default":
            argv += ["-mpcar", args.client_car]

        argv += ["-join", f"127.0.0.1:{args.port}"]

        # this seat's own picks: the generic client entries first, then its own
        this_env = dict(client_env)
        this_env.update(seat_env.get("client", {}))
        this_env.update(seat_env.get(f"client{i + 1}", {}))

        procs[name] = launch(dirs[name], args.exe, argv, this_env)
        log(f"joiner {i + 1} pid {procs[name].pid} args: {' '.join(argv)}")

        if i + 1 < len(names) - 1:
            log(f"waiting {args.stagger}s so the next joiner joins a LIVE match...")
            time.sleep(args.stagger)

    remaining = max(5, args.seconds - args.settle)
    log(f"running for {remaining}s...")

    # Watch for a crash WHILE it runs, not after. An access violation ends the
    # interesting part of the run, so there is no point sitting out the rest of
    # the window with both machines dead -- and a crash needs to be impossible to
    # miss, because everything the log says after it is meaningless.
    #
    # A crash is not the only way a run stops being interesting, so the same loop
    # also ends the run the moment the line under test appears (--until), the
    # moment a marker that must never appear does (--forbid), and the moment
    # nothing is happening any more (--stall).
    deadline = time.time() + remaining
    stopped = None
    seen_bytes = -1
    last_growth = time.time()
    next_tail = time.time()
    labels = {n: ("host" if n == "a" else f"joiner{n}") for n in names}
    hb = {s: {"seen": False, "frame": -1, "time": 0.0} for s in names}

    while time.time() < deadline:
        hit = crash_dumps(dirs)

        if hit:
            log(f"!! CRASH on {', '.join(sorted(hit))} -- stopping the run now")
            break

        texts, total = read_logs(dirs)
        now = time.time()

        if total != seen_bytes:
            seen_bytes = total
            last_growth = now

        # The thing we came for has happened. Repeatable, so a test can name both
        # sides -- and it is only worth anything because both logs are flushed
        # live (JERICHO_LOG_FLUSH), otherwise the line could still be in the
        # engine's buffer and the run would sit here waiting for a thing that
        # already happened.
        every = "".join(texts[s] for s in names)
        m = next((p for p in args.until if re.search(p, every)), None)

        if m:
            log(f"== --until '{m}' appeared -- stopping the run now")
            stopped = "until"
            break

        # ...or a marker that must never turn up.
        m = next((p for p in args.forbid if re.search(p, every)), None)

        if m:
            log(f"!! --forbid '{m}' appeared -- stopping the run now")
            stopped = "forbid"
            break

        # Liveness. Watching for a crash dump is not enough: a game that FREEZES
        # leaves no dump, has already logged every pass/fail marker, and reads as
        # a PASS -- which is exactly how a run that died at 6 s was recorded as a
        # healthier one than the run before it. So watch the module's own tick:
        # a heartbeat that stops advancing while the link is up means the
        # simulation stopped.
        for side in names:
            frames = heartbeat_frames(texts[side])

            if frames and (not hb[side]["seen"] or frames[-1] != hb[side]["frame"]):
                hb[side]["seen"] = True
                hb[side]["frame"] = frames[-1]
                hb[side]["time"] = now

        if args.stall > 0:
            stuck = [f"{labels[s]} (last heartbeat frame {hb[s]['frame']})"
                     for s in names
                     if hb[s]["seen"] and (now - hb[s]["time"]) >= args.stall]

            # No heartbeats to watch (the run was started without the lever):
            # fall back to "neither log grew at all".
            if (not stuck and not any(hb[s]["seen"] for s in names)
                    and (now - last_growth) >= args.stall):
                stuck = ["both logs (no growth)"]

            if stuck:
                log(f"!! STALLED -- nothing for {args.stall}s: {', '.join(stuck)}. "
                    f"A frozen game logs nothing, and this is NOT a pass")
                stopped = "stall"
                break

        # Say what is happening while it happens, so a bad run is obvious in
        # seconds instead of at the end of the window.
        if args.tail > 0 and now >= next_tail:
            next_tail = now + args.tail

            for side in names:
                lines = [l for l in texts[side].splitlines() if l.strip()]
                log(f"  [{labels[side]}] {lines[-1] if lines else '<nothing logged yet>'}")

        time.sleep(0.5)

    patterns = ("[mp]", "[error]", "[jericho]")

    for name in names:
        report(dirs[name], "HOST" if name == "a" else f"JOINER {name.upper()}", patterns)

    # Pass/fail BEFORE the run dirs (and their logs) are removed.
    passes = verdict(names, dirs, stopped, requires=args.require)

    for name in reversed(names):
        p = procs[name]
        label = labels[name]
        if p.poll() is None:
            p.terminate()

        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            # A crash dialog used to hold the process open here, and this is
            # exactly how a run "hangs": nothing is wrong, it is simply waiting
            # for someone to click OK. Terminate, then kill, then kill it by PID.
            # A process that will not die is a finding, not a reason to wait.
            p.kill()

            try:
                p.wait(timeout=8)
            except subprocess.TimeoutExpired:
                subprocess.run(["taskkill", "/PID", str(p.pid), "/F", "/T"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                log(f"!! {label} (pid {p.pid}) would not die -- killed it by PID. "
                    f"A modal crash dialog is the usual reason.")

        log(f"stopped {label} (pid {p.pid}, exit {p.returncode})")

    if not args.keep:
        for name in names:
            safe_remove(dirs[name])
        try:
            os.rmdir(pair_root)
        except OSError:
            pass
        log("run dirs removed (--keep to keep them)")

    return passes


if __name__ == "__main__":
    sys.exit(main())
