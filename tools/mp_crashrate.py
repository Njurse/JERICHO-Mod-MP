#!/usr/bin/env python3
"""Repeat the multiplayer pair N times and report the crash RATE, plus the one
number that the breadcrumbs in PingInCivCar made interesting: how many car slots
were free.

Why a rate, and why a correlation:

  * A single run tells you nothing here. The crash was 4/4 at one point and then
    a run passed; "it passed" is the exact evidence that got a wrong fix
    committed once already.
  * Every match so far has run with most of car_data occupied (7 of 20 free on
    the host). PingInCivCar was written for the base game, so if the crash
    tracks free slots running low, that is the thread to pull -- and if it does
    not, the hypothesis dies cheaply instead of costing another session.

    python mp_crashrate.py --runs 5

Each run is an ordinary mp_localpair run, so a crash is caught and triaged by
the harness (dump -> faulting module -> symbol) rather than hanging on a dialog.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mp_localpair as lp   # noqa: E402  (path first)

FREE = re.compile(r"PINGIN: enter dist=\d+ cookieStart=\d+ freeSlots=(\d+)")
SLOT = re.compile(r"PINGIN: slot=(-?\d+)")


def one_run(i, args):
    for side in ("a", "b"):
        d = os.path.join(lp.DEFAULT_GAME_DIR, ".mp-pair", side)
        for f in ("JERICHO.dmp", "JERICHO.log"):
            try:
                os.remove(os.path.join(d, f))
            except OSError:
                pass

    env = dict(os.environ)
    if args.onfoot:
        env["MP_TEST_ONFOOT"] = str(args.onfoot)

    cmd = [sys.executable, os.path.join(HERE, "mp_localpair.py"),
           "--seconds", str(args.seconds), "--settle", str(args.settle), "--keep"]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=args.seconds + 120)

    verdict = ""
    for line in (r.stdout or "").splitlines():
        if "verdict:" in line:
            verdict = line.strip()

    # what the engine's own breadcrumbs saw, across both machines
    frees, slots, pings = [], [], 0
    for side in ("a", "b"):
        p = os.path.join(lp.DEFAULT_GAME_DIR, ".mp-pair", side, "JERICHO.log")
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    m = FREE.search(line)
                    if m:
                        frees.append(int(m.group(1)))
                        pings += 1
                    m = SLOT.search(line)
                    if m:
                        slots.append(int(m.group(1)))
        except OSError:
            pass

    dumps = 0
    for side in ("a", "b"):
        if os.path.isfile(os.path.join(lp.DEFAULT_GAME_DIR, ".mp-pair", side, "JERICHO.dmp")):
            dumps += 1

    crashed = dumps > 0 or "dumps=1" in verdict or "dumps=2" in verdict

    print(f"  run {i}: {'CRASHED' if crashed else 'clean  '}  dumps={dumps} "
          f"pings={pings}  freeSlots min={min(frees) if frees else '-'} "
          f"max={max(frees) if frees else '-'}  slots={sorted(set(slots))}")
    return crashed, min(frees) if frees else None, pings


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=4)
    ap.add_argument("--seconds", type=int, default=34)
    ap.add_argument("--settle", type=int, default=5)
    ap.add_argument("--onfoot", type=int, default=0,
                    help="also auto-eject this many seconds in (MP_TEST_ONFOOT)")
    a = ap.parse_args()

    print(f"repeating the pair {a.runs} time(s): seconds={a.seconds} settle={a.settle} "
          f"onfoot={a.onfoot or 'off'}")
    print()

    rows = []
    for i in range(1, a.runs + 1):
        rows.append(one_run(i, a))

    crashed = sum(1 for c, _, _ in rows if c)
    free_at_crash = [f for c, f, _ in rows if c and f is not None]
    free_clean = [f for c, f, _ in rows if not c and f is not None]
    nopings = sum(1 for _, _, p in rows if p == 0)

    print()
    print(f"=== {crashed}/{len(rows)} crashed ===")
    if free_at_crash and free_clean:
        print(f"    freeSlots on crashed runs: {sorted(free_at_crash)}")
        print(f"    freeSlots on clean   runs: {sorted(free_clean)}")
    if nopings:
        print(f"    NOTE {nopings} run(s) produced NO PingInCivCar breadcrumbs at all -- "
              f"those are not evidence about this crash, whatever their verdict")
    # Exit non-zero on any crash, so "the crash fix holds" can be a command that
    # fails rather than a table somebody has to read. A crash-free run of N is the
    # claim; this is what makes it checkable in a chain.
    return 1 if crashed else 0


if __name__ == "__main__":
    sys.exit(main())
