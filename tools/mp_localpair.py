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
is what keeps the two REDRIVER2.log files and the two JERICHO/CONFIG/mp.ini from
fighting each other.

    python mp_localpair.py                  # host + join, report, clean up
    python mp_localpair.py --seconds 90     # watch for longer
    python mp_localpair.py --keep           # leave the run dirs for poking at

Both instances are launched directly (not via a .bat), so the PIDs here are real
and the cleanup kills exactly what it started -- nothing else.
"""
import argparse
import atexit
import os
import random
import shutil
import subprocess
import sys
import time

DEFAULT_GAME_DIR = os.path.join("src_rebuild", "bin", "Release_dev")
INSTANCE_DIR_NAMES = ("a", "b")
# The game writes its log as JERICHO.log -- the LOADER owns the file. REDRIVER2.log
# is kept as a fallback for a build that writes that name instead. Reporting the
# wrong one made every run print "no REDRIVER2.log" even when the game logged fine.
GAME_LOGS = ("JERICHO.log", "REDRIVER2.log")


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
             "Get-Process REDRIVER2_dev -ErrorAction SilentlyContinue | "
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


def verdict(dirs):
    """Pass/fail for the run: the pair must connect AND must not drop.

    This is what turns the harness from a log dump into a regression test.
    "The client dropped instantly" was invisible in a wall of logs, so the two
    markers that actually mean it -- a client that never got accepted, and either
    side reporting a peer that spoke 0 bytes (the signature of the receive-guard
    bug) -- are asserted here. A benign end-of-run close ('dropped (closed)')
    is not counted.
    """
    host = read_log(dirs["a"])
    client = read_log(dirs["b"])

    host_join = "joined (" in host
    client_ok = "accepted as player" in client
    client_lost = client.count("Lost the server")
    zero_byte = (host + client).count("0 byte(s) received")

    ok = host_join and client_ok and client_lost == 0 and zero_byte == 0
    log(f"verdict: host_join={host_join} client_accepted={client_ok} "
        f"client_lost={client_lost} zero_byte_peers={zero_byte} -> "
        f"{'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", default=DEFAULT_GAME_DIR)
    ap.add_argument("--exe", default="REDRIVER2_dev.exe")
    ap.add_argument("--port", type=int, default=1400)
    ap.add_argument("--seconds", type=int, default=60, help="how long to let them run")
    ap.add_argument("--settle", type=int, default=7,
                    help="seconds to wait before the client joins")
    ap.add_argument("--keep", action="store_true", help="leave the run dirs behind")
    ap.add_argument("--clean", action="store_true",
                    help="remove the run dirs and exit (never follows a junction)")
    ap.add_argument("--map", action="store_true", help="hold the in-game map open (MP_MAP=1)")
    ap.add_argument("--no-debug", action="store_true",
                    help="do NOT set MP_DEBUG=1. This is what the packaged launchers "
                         "(PLAY_HOST.bat / PLAY_JOIN.bat) do, so it is the only way to test "
                         "what a player actually runs -- a bug that only appears without "
                         "MP_DEBUG is invisible otherwise.")
    ap.add_argument("--bot", default="off", choices=["off", "random", "chase", "fight", "pursuit"],
                    help="drive the player cars with the mp test bot: random / chase (host flees, "
                         "joiner chases) / fight (both charge) / pursuit (BOTH hunt each other, so "
                         "they reliably meet and collide). OFF by default -- this drives a real "
                         "player's car.")
    ap.add_argument("--level", default="rio",
                    help="city for the host to host (default rio)")
    ap.add_argument("--mp-arena", default="1",
                    help="multiplayer map/arena for both sides (default 1)")
    ap.add_argument("--host-car", default="slot1",
                    help="the host's car: a model number, slotN, 'random', or 'default' "
                         "(= pass NO -mpcar, so the level chooses -- what a player who just "
                         "presses Host does, and the case where the join used to get the "
                         "wrong car, default slot1)")
    ap.add_argument("--client-car", default="slot3",
                    help="the joining player's car: a model number, slotN, 'random', or 'default' "
                         "(= pass NO -mpcar; default slot3)")
    args = ap.parse_args()

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
    dirs = {n: os.path.join(pair_root, n) for n in INSTANCE_DIR_NAMES}

    if args.clean:
        for name in INSTANCE_DIR_NAMES:
            safe_remove(dirs[name])
        try:
            os.rmdir(pair_root)
        except OSError:
            pass
        log(f"removed {pair_root}")
        return 0

    for name in INSTANCE_DIR_NAMES:
        build_run_dir(game_dir, dirs[name], f"Local{name.upper()}", args.port)
    log(f"run dirs ready under {pair_root}")

    # NOTE: both instances want UDP/1318 for discovery, so the second one reports
    # "discovery unavailable" and simply does not browse. The join below uses the
    # address directly, which is the path that must work anyway.
    env = {} if args.no_debug else {"MP_DEBUG": "1"}
    if args.map:
        env["MP_MAP"] = "1"

    # The host has to start the match itself: the attract demo that used to do it
    # by accident is suppressed now (it was launching a level nobody asked for and
    # blocking through the load). MP_AUTOSTART=host is the module's own lever --
    # it launches once a player is in. The client must NOT get it, or it would try
    # to host as well.
    host_env = dict(env)
    host_env["MP_AUTOSTART"] = "host"
    client_env = dict(env)
    client_env.pop("MP_AUTOSTART", None)

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
    a = launch(dirs["a"], args.exe, host_argv, host_env)
    log(f"host  pid {a.pid}  (port {args.port}, {args.level} arena {args.mp_arena}, "
        f"host car {args.host_car}, client car {args.client_car})")

    log(f"waiting {args.settle}s for the host to load...")
    time.sleep(args.settle)

    if args.client_car == "default":
        client_argv = client_args + ["-join", f"127.0.0.1:{args.port}"]
    else:
        client_argv = client_args + ["-mpcar", args.client_car, "-join", f"127.0.0.1:{args.port}"]
    b = launch(dirs["b"], args.exe, client_argv, client_env)

    for label, argv in (("host", host_argv), ("client", client_argv)):
        log(f"{label} args: {' '.join(argv)}")
    log(f"client pid {b.pid}")

    remaining = max(5, args.seconds - args.settle)
    log(f"running for {remaining}s...")
    time.sleep(remaining)

    patterns = ("[mp]", "[error]", "[jericho]")
    report(dirs["a"], "HOST", patterns)
    report(dirs["b"], "CLIENT", patterns)

    # Pass/fail BEFORE the run dirs (and their logs) are removed.
    passes = verdict(dirs)

    for p, label in ((b, "client"), (a, "host")):
        if p.poll() is None:
            p.terminate()
        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p.kill()
        log(f"stopped {label} (pid {p.pid}, exit {p.returncode})")

    if not args.keep:
        for name in INSTANCE_DIR_NAMES:
            safe_remove(dirs[name])
        try:
            os.rmdir(pair_root)
        except OSError:
            pass
        log("run dirs removed (--keep to keep them)")

    return passes


if __name__ == "__main__":
    sys.exit(main())
