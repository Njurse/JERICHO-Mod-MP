#!/usr/bin/env python3
"""The mp smoke gate: one command that answers "does a pair still play a match?"

    python JERICHO/MODS/mp/tools/mp_smoke.py

Why this exists, and why it is not just mp_localpair.py
-------------------------------------------------------
`mp_localpair.py` PRINTS a verdict; nothing acts on it. So a run that STALLED --
a frozen game that logs nothing -- could be read as a pass by anyone skimming the
output, and `mp_crashrate.py` cannot see it at all (it scores `clean` from crash
dumps alone). This script turns the verdict into an exit code with the whole bar
in one place:

  * the pair verdict is PASS,
  * lost=0 (no client dropped),
  * dumps=0 (no access violation), and
  * NOT STALLED (a frozen game is not a pass).

It deliberately does NOT re-implement the harness: it runs `mp_localpair.py` and
reads the one line it prints, so the two cannot drift apart.

The game assets are 1.6 GB and are not in the repo, and two game instances need a
real window -- so this cannot run on a hosted CI runner. When the exe or the data
is missing it says SKIP and exits 2 (a distinct code, so a CI job can tell "not
runnable here" from "failed"), unless --require-assets is given, which turns a
skip into a failure. That is the honest shape: the gate runs wherever the assets
exist, and never pretends to have run where they do not.

Exit codes:  0 = the smoke passed      2 = SKIP (no assets)
             1 = the smoke failed      3 = --require-assets and no assets
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAIR = os.path.join(HERE, "mp_localpair.py")

# The exe and the data are the two things a run needs. The exe is built (so a CI
# runner could have it); DRIVER2 is the 1.6 GB of game assets that never is.
DEFAULT_GAME_DIR = os.path.join("src_rebuild", "bin", "Release_dev")
REQUIRED = ("JERICHO_dev.exe", os.path.join("DRIVER2", "LEVELS"))

VERDICT = re.compile(r"\[pair\] verdict: (.*)")
STALLED = re.compile(r"stopped=stall|STALLED")
LOST = re.compile(r"lost=(\d+)")
DUMPS = re.compile(r"dumps=(\d+)")


def assets_present(game_dir):
    return all(os.path.exists(os.path.join(game_dir, p)) for p in REQUIRED)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", default=DEFAULT_GAME_DIR)
    ap.add_argument("--seconds", type=int, default=40,
                    help="how long the pair plays (default 40; the freeze this gate exists "
                         "for was seen at 60 s, so a green 40 s run is not proof -- raise it "
                         "with --seconds 60 when chasing that)")
    ap.add_argument("--settle", type=int, default=5)
    ap.add_argument("--level", default="rio")
    ap.add_argument("--require-assets", action="store_true",
                    help="exit 3 instead of skipping when the game assets are absent")
    a = ap.parse_args()

    if not assets_present(a.game_dir):
        missing = [p for p in REQUIRED
                   if not os.path.exists(os.path.join(a.game_dir, p))]
        print(f"[smoke] SKIP: no game assets under {a.game_dir} "
              f"(missing: {', '.join(missing)})")
        print("[smoke] a hosted CI runner has neither the 1.6 GB of data nor a window; "
              "run this where the game is installed")
        return 3 if a.require_assets else 2

    cmd = [sys.executable, PAIR, "--game-dir", a.game_dir, "--level", a.level,
           "--seconds", str(a.seconds), "--settle", str(a.settle)]
    print(f"[smoke] running: {' '.join(cmd)}")
    r = subprocess.run(cmd, capture_output=True, text=True)

    out = (r.stdout or "") + (r.stderr or "")
    m = VERDICT.search(out)
    line = m.group(1).strip() if m else ""
    print(f"[smoke] pair verdict: {line or '<none -- the harness did not reach a verdict>'}")

    failures = []

    if m is None:
        failures.append("the harness produced no verdict line")
    else:
        if "-> PASS" not in line:
            failures.append("the pair verdict was not PASS")
        n = LOST.search(line)
        if n and int(n.group(1)) != 0:
            failures.append(f"lost={n.group(1)} (a client dropped)")
        d = DUMPS.search(line)
        if d and int(d.group(1)) != 0:
            failures.append(f"dumps={d.group(1)} (an access violation)")
        if STALLED.search(line):
            failures.append("STALLED (a frozen game logs nothing, and is not a pass)")

    if failures:
        print("[smoke] FAIL:")
        for f in failures:
            print(f"    - {f}")
        if out.strip():
            print("[smoke] --- last 15 lines of the harness output ---")
            for ln in out.rstrip().splitlines()[-15:]:
                print("    " + ln)
        return 1

    print("[smoke] PASS: pair PASS, lost=0, dumps=0, not stalled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
