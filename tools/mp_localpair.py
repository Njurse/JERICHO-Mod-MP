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
import os
import shutil
import subprocess
import sys
import time

DEFAULT_GAME_DIR = os.path.join("src_rebuild", "bin", "Release_dev")
INSTANCE_DIR_NAMES = ("a", "b")
GAME_LOG = "REDRIVER2.log"


def log(msg):
    print(f"[pair] {msg}", flush=True)


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
    return p


def report(run_dir, label, patterns):
    path = os.path.join(run_dir, GAME_LOG)
    if not os.path.exists(path):
        log(f"{label}: no {GAME_LOG}")
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", default=DEFAULT_GAME_DIR)
    ap.add_argument("--exe", default="REDRIVER2_dev.exe")
    ap.add_argument("--port", type=int, default=1400)
    ap.add_argument("--seconds", type=int, default=60, help="how long to let them run")
    ap.add_argument("--settle", type=int, default=12,
                    help="seconds to wait before the client joins")
    ap.add_argument("--keep", action="store_true", help="leave the run dirs behind")
    ap.add_argument("--clean", action="store_true",
                    help="remove the run dirs and exit (never follows a junction)")
    ap.add_argument("--map", action="store_true", help="hold the in-game map open (MP_MAP=1)")
    args = ap.parse_args()

    game_dir = os.path.abspath(args.game_dir)
    if not os.path.isfile(os.path.join(game_dir, args.exe)):
        log(f"no {args.exe} in {game_dir}")
        return 2

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
    env = {"MP_DEBUG": "1"}
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

    a = launch(dirs["a"], args.exe, ["-nointro", "-nofmv", "-host", str(args.port)], host_env)
    log(f"host  pid {a.pid}  (port {args.port})")

    log(f"waiting {args.settle}s for the host to load...")
    time.sleep(args.settle)

    b = launch(dirs["b"], args.exe,
               ["-nointro", "-nofmv", "-join", f"127.0.0.1:{args.port}"], client_env)
    log(f"client pid {b.pid}")

    remaining = max(5, args.seconds - args.settle)
    log(f"running for {remaining}s...")
    time.sleep(remaining)

    patterns = ("[mp]", "[error]", "[jericho]")
    report(dirs["a"], "HOST", patterns)
    report(dirs["b"], "CLIENT", patterns)

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

    return 0


if __name__ == "__main__":
    sys.exit(main())
