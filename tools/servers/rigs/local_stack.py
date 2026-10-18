#!/usr/bin/env python3
"""local_stack.py -- run the whole mp server stack on ONE machine and report.

Starts the master discovery server, the dedicated server, and N real
JERICHO_dev.exe clients that join the dedi, then reads every log and prints what
each layer actually did. This is the "mock internet on one PC" rig: it needs no
second machine, no port-forward and no internet.

    python JERICHO/MODS/mp/tools/servers/rigs/local_stack.py                  # 1 client
    python .../local_stack.py --clients 2 --seconds 60
    python .../local_stack.py --clients 2 --beacon --keep

Why a SEPARATE script rather than a mode of mp_localpair.py: the pair harness
starts a HOST seat and joins to it, and the dedicated server is not a game
instance -- it is a headless Python process. The topology is different, so the
topology lives here. Everything that is NOT topology is imported from
mp_localpair as a library -- the junctioned run dirs, the real-PID launch, the
per-seat log reading and the PID-scoped kill -- rather than copied, so those
keep working the one way they already do.

Exit codes: 0 = every seat connected and its sim ran, 1 = a finding, 2 = no game
build to run (skip, like the other rigs).
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))            # .../mp/tools/servers/rigs
SERVERS = os.path.dirname(HERE)                               # .../mp/tools/servers
TOOLS = os.path.dirname(SERVERS)                              # .../mp/tools
REPO = os.path.abspath(os.path.join(TOOLS, "..", "..", "..", ".."))
GAME_DIR_DEFAULT = os.path.join(REPO, "src_rebuild", "bin", "Release_dev")

MASTER_PY = os.path.join(SERVERS, "masterserver", "masterserver.py")
DEDI_PY = os.path.join(SERVERS, "dediserver", "dediserver.py")

sys.path.insert(0, TOOLS)
sys.path.insert(0, SERVERS)
import mp_localpair as lp      # noqa: E402  the run-dir / launch / log helpers
import proto_check             # noqa: E402  the wire-mirror guard

# Seats live inside the pair harness's scratch root: build_run_dir() skips an
# entry named ".mp-pair", so a run dir nested under it is never junctioned into
# itself. bin/ is gitignored, so none of this shows up in git status either.
SCRATCH = ".mp-pair"
SIM_FLOOR = lp.SIM_FLOOR


def log(msg):
    print(f"[stack] {msg}", flush=True)


# ------------------------------------------------------------------ processes
def spawn(argv, out_path):
    fh = open(out_path, "w", encoding="utf-8", buffering=1)
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    p = subprocess.Popen(argv, stdout=fh, stderr=subprocess.STDOUT, cwd=REPO,
                         creationflags=flags)
    return p, out_path


def stop_server(p, name):
    """Graceful where the platform allows it: a clean stop is what unregisters the
    dedi from the master, and that is evidence we want, not just tidiness."""
    if p is None or p.poll() is not None:
        return
    sig = signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM
    try:
        p.send_signal(sig)
    except (OSError, ValueError):
        p.terminate()
    try:
        p.wait(timeout=8)
    except subprocess.TimeoutExpired:
        log(f"{name} did not stop on the signal; killing")
        p.kill()


def wait_tcp(port, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def wait_log(path, needle, timeout=15.0):
    """Wait for a line to appear in a server's own log.

    Used instead of a TCP probe for the dedi: a bare connect is a real connection
    to it, and it answers one with 'bad handshake (tag None), dropping' -- true,
    but OUR noise in the log we are about to read as evidence.
    """
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                if needle in f.read():
                    return True
        time.sleep(0.2)
    return False


def master_listing(port):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/v1/servers" % port, timeout=3) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError):
        return None


def tail(text, patterns, limit=6):
    hits = [ln for ln in text.splitlines() if any(p in ln for p in patterns)]
    return hits[-limit:]


def run_advertise(args, run_root, master_url):
    """Does a HOST list ITSELF on the master? The half no screenshot can show.

    run_browser proves the other direction -- a client reading what the master
    holds. This one starts the master, HOSTS one real game with master_url pointed
    at it, and reads the master's own listing back. Then it brings a joiner in and
    reads again, because the player count is the heartbeat's job and a listing that
    never updates is worse than no listing at all.
    """
    if not master_url:
        log("FAIL: --advertise needs the master (do not pass --no-master)")
        return 1

    name = "Rig Host"
    run_dir = os.path.join(run_root, "host")
    lp.safe_remove(run_dir)
    lp.build_run_dir(args.game_dir, run_dir, "HostSeat", args.dedi_port)

    # the REAL config path for the master address (there is no lever for it)
    with open(os.path.join(run_dir, "JERICHO", "CONFIG", "mp.ini"), "a", encoding="utf-8") as f:
        f.write(f"master_url = {master_url}\n")
        f.write(f"host_name = {name}\n")

    env = {} if args.no_debug else {"MP_DEBUG": "1"}
    env["JERICHO_LOG_FLUSH"] = "1"
    env["MP_AUTOSTART"] = "host"          # a hosted game is what registers
    env["MP_HEARTBEAT"] = "1"

    host_log = os.path.join(run_dir, "JERICHO.log")
    host = lp.launch(run_dir, args.exe,
                     ["-nointro", "-nofmv", "-mp", "1", "-host", str(args.dedi_port)], env)
    log(f"hosting seat pid {host.pid} on {args.dedi_port}, master {master_url}")

    # the registration is one POST after the level is up; give the load its time
    row = None
    deadline = time.time() + args.advertise_wait
    while time.time() < deadline:
        listing = master_listing(args.master_port)
        for s in (listing or {}).get("servers", []):
            if s.get("name") == name:
                row = s
                break
        if row:
            break
        time.sleep(2.0)

    if not row:
        log("FAIL: the host never appeared in the master's listing")
        log(f"  seat log: {(lp.read_log(run_dir) or '')[-400:]}")
        return 1

    log("listed: name=%r host=%s port=%s players=%s/%s city=%s rules=%s"
        % (row["name"], row["host"], row["port"], row["players"], row["maxPlayers"],
           row["city"], row.get("rules")))

    # the two fields the rig itself decides: the port it bound and the city it asked
    # for. The rules byte is whatever the host's session settings are, so it is
    # reported rather than asserted.
    ok = (row["port"] == args.dedi_port and row["city"] == args.city)

    if not args.advertise_join:
        log("PASS: the host listed itself (pass --advertise-join to also watch the"
            " player count follow a joiner)")
        return 0 if ok else 1

    # ...and the count follows the session: one joiner, then read it again
    join_dir = os.path.join(run_root, "join")
    lp.safe_remove(join_dir)
    lp.build_run_dir(args.game_dir, join_dir, "JoinSeat", args.dedi_port)
    jenv = {} if args.no_debug else {"MP_DEBUG": "1"}
    jenv["JERICHO_LOG_FLUSH"] = "1"
    lp.launch(join_dir, args.exe,
              ["-nointro", "-nofmv", "-mp", "1", "-join", f"127.0.0.1:{args.dedi_port}"], jenv)

    grew = False
    deadline = time.time() + args.advertise_wait
    while time.time() < deadline:
        listing = master_listing(args.master_port)
        for s in (listing or {}).get("servers", []):
            if s.get("name") == name and s.get("players", 0) > row.get("players", 0):
                log("the listing followed the session: players now %s" % s["players"])
                grew = True
                break
        if grew:
            break
        time.sleep(2.0)

    if not grew:
        log("FAIL: the listing never showed the joiner (the heartbeat is not updating)")

    if ok and grew:
        log("PASS: a host lists itself, and the listing tracks the players")
        return 0

    log("FAIL: listed=%s grew=%s" % (ok, grew))
    return 1


def run_browser(args, run_root, master_url):
    """Open the Online browser (mp.online) in a real client and photograph it.

    This is a FRONTEND screen, so nothing joins and the stack is only there to
    give the browser a list to draw. Two things it needs are the real ones, not
    test substitutes: the seat's mp.ini is pointed at the local master (that is
    how a player configures it, and the fetch is what the screen is FOR), and
    MP_TEST_OPENMENU opens the screen -- module menus have no headless driver,
    so an automated run cannot reach one otherwise.

    A second host is registered too, so the table shows two rows with DIFFERENT
    modes and cities rather than one row twice.
    """
    second = None
    if not master_url:
        # --no-master + --browser is not a mistake: it asks for the one screen the
        # normal run cannot reach, the EMPTY browser -- what a player sees before a
        # master_url is set anywhere. No second host, because there would be nothing
        # for it to register with.
        log("no master: photographing the EMPTY browser (nothing to list, by design)")
    else:
        second_port = args.dedi_port + 1
        second_log = os.path.join(run_root, "dedi2.log")
        second, _ = spawn(
            [sys.executable, DEDI_PY, "--port", str(second_port),
             "--name", "Second Game", "--city", str((args.city + 2) % 4),
             "--cops",                          # cops on (still Take a Ride)
             "--no-beacon",
             "--master", master_url, "--master-interval", str(args.master_interval),
             "--log-level", "info"], second_log)
        if not wait_log(second_log, "listening on TCP/%d" % second_port):
            log("FAIL: the second dedi never reported itself listening")
            stop_server(second, "dedi2")
            return 1

        time.sleep(2.0)                        # let both registrations land
        listing = master_listing(args.master_port)
        log(f"master lists {listing['count'] if listing else '?'} server(s)")

    run_dir = os.path.join(run_root, "browser")
    lp.safe_remove(run_dir)
    lp.build_run_dir(args.game_dir, run_dir, "BrowserSeat", args.dedi_port)

    # the REAL config path for the master address (there is no lever for it)
    with open(os.path.join(run_dir, "JERICHO", "CONFIG", "mp.ini"), "a", encoding="utf-8") as f:
        f.write(f"master_url = {master_url}\n")

    env = {} if args.no_debug else {"MP_DEBUG": "1"}
    env["JERICHO_LOG_FLUSH"] = "1"
    env["MP_TEST_OPENMENU"] = args.browser_menu

    proc = lp.launch(run_dir, args.exe, ["-nointro", "-nofmv"], env)
    log(f"browser seat pid {proc.pid}: frontend, MP_TEST_OPENMENU={args.browser_menu}")

    # Wait for the SCREEN, not for the clock. How long the game takes to reach the
    # frontend and open the menu varies by ten seconds run to run, so a fixed sleep
    # is why the grab used to come back with "no window of pid ... matched" -- the
    # window did not exist yet. The lever logs when the open takes; that line is the
    # signal, and a short settle afterwards is for the fetch and the first paint.
    seat_log = os.path.join(run_dir, "JERICHO.log")
    if wait_log(seat_log, "(MP_TEST_OPENMENU)", timeout=90.0):
        time.sleep(min(args.seconds, 12))
    else:
        log("FAIL: the screen never opened (no MP_TEST_OPENMENU line in the seat log)")

    png = args.browser_shot or os.path.join(args.shots or run_root, "online.png")
    os.makedirs(os.path.dirname(os.path.abspath(png)), exist_ok=True)

    # ...and retry the grab: the OS window can lag the drawn screen by a moment.
    rc = 1
    for attempt in range(6):
        rc = subprocess.call([sys.executable, os.path.join(TOOLS, "mpshots.py"), "grab",
                              "--pid", str(proc.pid), "--out", png])
        if rc == 0:
            break
        time.sleep(2.0)
    log(f"screenshot: {png}" if rc == 0 else f"FAIL: mpshots.py grab exited {rc}")

    text = lp.read_log(run_dir) or ""
    for pat in ("frontend:", "discovery server", "online table drawn", "online ping", "opening menu"):
        for line in tail(text, (pat,), 3):
            log("  seat: " + line.strip())

    stop_server(second, "dedi2")
    return 0


# ----------------------------------------------------------------------- main
def parse_args(argv):
    ap = argparse.ArgumentParser(description="run the mp master + dedi + real clients on one PC")
    ap.add_argument("--clients", type=int, default=1, help="how many real game clients join")
    ap.add_argument("--seconds", type=int, default=45, help="how long to let them play")
    ap.add_argument("--settle", type=int, default=4, help="seconds to wait after the dedi is up")
    ap.add_argument("--stagger", type=int, default=8, help="seconds between client launches")
    ap.add_argument("--city", type=int, default=0, help="GameLevel the dedi tells clients to load")
    ap.add_argument("--dedi-port", type=int, default=1400)
    ap.add_argument("--external-dedi", type=int, default=None, metavar="PORT",
                    help="run clients against an ALREADY-RUNNING dedi on PORT: start no "
                         "server at all (use this to cycle clients against one dedi)")
    ap.add_argument("--dedi-config", default=None, metavar="FILE",
                    help="pass a dediserver.json to the dedi (--config)")
    ap.add_argument("--master-port", type=int, default=8100)
    ap.add_argument("--master-ttl", type=int, default=30)
    ap.add_argument("--master-interval", type=int, default=5)
    ap.add_argument("--no-master", action="store_true", help="do not start the master at all")
    ap.add_argument("--beacon", action="store_true",
                    help="let the dedi beacon on UDP (off by default: keeps a run deterministic)")
    ap.add_argument("--join-manual", action="store_true",
                    help="join via the MANUAL ADDRESS field (MP_TEST_MANUALADDR) instead of -join")
    ap.add_argument("--manual-addr", default=None, metavar="TEXT",
                    help="exactly what to type into the manual address field (default 127.0.0.1:<dedi port>); a %%s in it is replaced with the dedi port")
    ap.add_argument("--game-dir", default=GAME_DIR_DEFAULT)
    ap.add_argument("--exe", default="JERICHO_dev.exe")
    ap.add_argument("--no-debug", action="store_true", help="run WITHOUT MP_DEBUG, like a player does")
    ap.add_argument("--keep", action="store_true", help="leave the run dirs and logs behind")
    ap.add_argument("--shots", default=None, metavar="DIR", help="photograph the seats with mpshots.py")
    ap.add_argument("--advertise-wait", type=float, default=45.0, metavar="S",
                    help="how long to wait for the listing to appear (default 45)")
    ap.add_argument("--advertise-join", action="store_true",
                    help="also bring a joiner in and watch the player count follow "
                         "(adds a heartbeat interval, ~30s)")
    ap.add_argument("--advertise", action="store_true",
                    help="host ONE game and prove it lists ITSELF on the master, then that the listing follows a joiner")
    ap.add_argument("--browser", action="store_true",
                    help="open the Online browser (mp.online) in a frontend client pointed "
                         "at the local master and photograph it, instead of joining")
    ap.add_argument("--browser-shot", default=None, metavar="PNG",
                    help="where to write that screenshot (default <shots>/online.png)")
    ap.add_argument("--browser-menu", default="mp.online", metavar="ID",
                    help="which menu MP_TEST_OPENMENU opens (default mp.online; another id "
                         "is the control for a pixel diff)")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    proto_check.assert_ok()   # a drifted wire mirror explains a lot of nonsense; fail first

    exe = os.path.join(args.game_dir, args.exe)
    if not os.path.isfile(exe):
        log(f"SKIP: no {args.exe} in {args.game_dir}")
        return 2
    if not os.path.isdir(os.path.join(args.game_dir, "DRIVER2")):
        log(f"SKIP: no DRIVER2 game data in {args.game_dir}")
        return 2

    run_root = os.path.join(args.game_dir, SCRATCH, "stack")
    master_proc = dedi_proc = None
    # the client loop's bookkeeping, hoisted so the finally below is safe on every
    # path out of the try (the --browser path returns before the loop runs)
    procs = {}
    shots_proc = None
    master_log = os.path.join(run_root, "master.log")
    dedi_log = os.path.join(run_root, "dedi.log")
    os.makedirs(run_root, exist_ok=True)

    names = [f"Seat{n + 1}" for n in range(args.clients)]
    seats = {f"c{n + 1}": os.path.join(run_root, f"c{n + 1}") for n in range(args.clients)}

    try:
        if args.external_dedi:
            # Stability work wants MANY client sessions against ONE dedi: slot reuse,
            # cycling, a restart under a live client. So the rig can also be only the
            # client half, pointed at a server somebody else is keeping up.
            args.dedi_port = args.external_dedi
            master_url = ""
            log(f"using an external dedi on TCP/{args.dedi_port}; starting no server")
            log(f"waiting {args.settle}s before the first client...")
            time.sleep(args.settle)
        else:
            # -- master --------------------------------------------------------
            master_url = ""
            if not args.no_master:
                master_proc, _ = spawn(
                    [sys.executable, MASTER_PY, "--bind", "127.0.0.1",
                     "--port", str(args.master_port), "--ttl", str(args.master_ttl),
                     "--sweep", "1", "--log-level", "info"], master_log)
                if not wait_tcp(args.master_port):
                    log("FAIL: the master never accepted a connection")
                    return 1
                master_url = "http://127.0.0.1:%d" % args.master_port
                log(f"master up on {master_url} (ttl {args.master_ttl}s)")
            else:
                log("master: skipped (--no-master)")

        # -- dedicated server ---------------------------------------------
        if not args.external_dedi:
            dedi_argv = [sys.executable, DEDI_PY, "--port", str(args.dedi_port),
                         "--name", "LocalStack", "--city", str(args.city),
                         "--idle-drop", "30", "--log-level", "debug"]
            if args.dedi_config:
                dedi_argv += ["--config", args.dedi_config]
            dedi_argv += ["--beacon"] if args.beacon else ["--no-beacon"]
            if master_url:
                dedi_argv += ["--master", master_url, "--master-interval", str(args.master_interval)]
            dedi_proc, _ = spawn(dedi_argv, dedi_log)
            if not wait_log(dedi_log, "listening on TCP/%d" % args.dedi_port):
                log("FAIL: the dedi never reported itself listening")
                return 1
            log(f"dedi up on TCP/{args.dedi_port} (city {args.city}, "
                f"{'beaconing' if args.beacon else 'no beacon'})")

            if master_url:
                time.sleep(1.5)   # let the dedi's registration land
                listing = master_listing(args.master_port)
                log(f"master lists {listing['count'] if listing else '?'} server(s)"
                    + ("  <- the dedi registered" if listing and listing["count"] else ""))

            log(f"waiting {args.settle}s before the first client...")
            time.sleep(args.settle)

        # -- the Online browser (frontend only) ---------------------------
        # Nothing joins here: the stack exists to give the screen a list, and the
        # screenshot is the verification (the pixels are judged by eye).
        if args.browser:
            return run_browser(args, run_root, master_url)

        # -- a HOST listing itself ---------------------------------------
        # The mirror of the browser run: the master is the witness, and the host
        # is a real game rather than a tool.
        if args.advertise:
            return run_advertise(args, run_root, master_url)

        # -- clients ------------------------------------------------------
        env = {} if args.no_debug else {"MP_DEBUG": "1"}
        env["JERICHO_LOG_FLUSH"] = "1"     # judge what actually happened, not a buffer
        env["MP_HEARTBEAT"] = "1"          # so a frozen seat is distinguishable from a quiet one
        env.pop("MP_AUTOSTART", None)      # a client must never host as well

        seat_args = []
        for i, key in enumerate(sorted(seats)):
            run_dir = seats[key]
            lp.safe_remove(run_dir)
            # the mp.ini port WINS over a -join port, so it must be the dedi's --
            # except in the manual case, where it is deliberately the WRONG port:
            # that is what proves the ":port" in the typed address is understood
            # (a client that fell back to the configured port would go nowhere).
            lp.build_run_dir(args.game_dir, run_dir, names[i],
                             args.dedi_port + 7 if args.join_manual else args.dedi_port)
            argv = ["-nointro", "-nofmv"]
            if args.join_manual:
                # Exercise the MANUAL ADDRESS field the way a player types it: the
                # module's own lever fills the field and presses Connect through the
                # same ActManualConnect the row uses. No -join, so the game comes up
                # in the frontend and this is the path that gets there.
                addr = args.manual_addr if args.manual_addr else "127.0.0.1:%d" % args.dedi_port
                env["MP_TEST_MANUALADDR"] = addr.replace("%s", str(args.dedi_port))
            else:
                argv += ["-join", f"127.0.0.1:{args.dedi_port}"]
            procs[key] = lp.launch(run_dir, args.exe, argv, env)
            log(f"client {key} pid {procs[key].pid} -> 127.0.0.1:{args.dedi_port}")
            seat_args += ["--seat", f"{key}={os.path.join(run_dir, 'JERICHO.log')}:{procs[key].pid}"]
            if i + 1 < len(seats):
                time.sleep(args.stagger)      # a later seat joins a LIVE match

        if args.shots:
            os.makedirs(args.shots, exist_ok=True)
            shots = os.path.join(TOOLS, "mpshots.py")
            if os.path.isfile(shots):
                shots_proc, _ = spawn(
                    [sys.executable, shots, "--shots", os.path.abspath(args.shots),
                     "--seconds", str(args.seconds), "--quiet"] + seat_args,
                    os.path.join(run_root, "shots.log"))

        log(f"running for {args.seconds}s...")
        time.sleep(args.seconds)

        # A snapshot WHILE clients are connected: the registration alone is proven by
        # the start-up check, but the live player count is what the listing is for.
        if not args.no_master and not args.external_dedi:
            mid = master_listing(args.master_port)
            if mid and mid["count"]:
                log("master mid-run: " + json.dumps(mid["servers"][0]))
            else:
                log("master mid-run: no servers listed")
    finally:
        if shots_proc is not None:
            shots_proc.terminate()
        # clients first (they are the traffic), then the servers, gracefully
        lp.stop_started()
        stop_server(dedi_proc, "dedi")
        stop_server(master_proc, "master")

    # -- report ------------------------------------------------------------
    print()
    # With an external dedi this file is a stale leftover of an earlier rig run, not
    # this one's log -- read nothing rather than report somebody else's session.
    dedi_text = ""
    if not args.external_dedi and os.path.exists(dedi_log):
        with open(dedi_log, encoding="utf-8", errors="replace") as f:
            dedi_text = f.read()
    master_text = open(master_log, encoding="utf-8", errors="replace").read() if os.path.exists(master_log) else ""

    log("master log:")
    for ln in tail(master_text, ("[INFO]", "[WARNING]", "[ERROR]"), 5):
        print("   ", ln)

    log("dedi log:" if not args.external_dedi else "dedi log: (external dedi — read its own log)")
    for ln in tail(dedi_text, ("joined", "left", "refused", "timed out", "[WARNING]", "[ERROR]"), 8):
        print("   ", ln)

    if master_proc is not None:
        log(f"master: {'stopped' if master_proc.poll() is not None else 'still up'}")
    print()

    failed = []
    for i, key in enumerate(sorted(seats)):
        run_dir = seats[key]
        text = lp.read_log(run_dir)
        accepted = "accepted as player" in text
        started = "match started" in text
        frames = lp.heartbeat_frames(text)
        last = frames[-1] if frames else -1
        dump = os.path.isfile(os.path.join(run_dir, "JERICHO.dmp"))
        lost = text.count("Lost the server")
        zerobyte = text.count("0 byte(s) received")

        log(f"seat {key} ({names[i]}):")
        print(f"    accepted={accepted}  match_started={started}  "
              f"last_sim_frame={last}  crash_dump={dump}  lost={lost}  zero_byte={zerobyte}")
        for ln in tail(text, ("[mp] player", "[mp] roster", "as player", "dropped"), 5):
            print("      ", ln)

        if not accepted or not started or last < SIM_FLOOR or dump:
            failed.append(key)

    # -- what the layers said about each other -----------------------------
    print()
    if args.external_dedi:
        log(f"{len(seats)} client(s) ran against the external dedi (its log has the joins)")
    else:
        joins = dedi_text.count("joined")
        leaves = dedi_text.count(" left ")
        log(f"dedi saw {joins} join(s) and {leaves} leave(s) for {len(seats)} client(s)")
        if not args.no_master:
            unreg = "unregistered from master" in dedi_text
            log(f"dedi unregistered from the master on shutdown: {unreg}")

    if not args.keep:
        for run_dir in seats.values():
            lp.safe_remove(run_dir)

    print()
    if failed:
        log(f"FAIL: {', '.join(failed)} did not connect and run")
        return 1
    log("PASS: every seat connected and its sim ran")
    return 0


if __name__ == "__main__":
    sys.exit(main())
