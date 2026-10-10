#!/usr/bin/env python3
"""The delivery gate: every headless mp rig, with the bar each one has to clear.

One command and one exit code, because a gate that has to be read is a gate that gets
skipped -- and two of these rigs exist precisely because a wrong result was once
believed for a whole session.

  smoke      mp_smoke.py       a pair plays a match: verdict PASS, lost=0, dumps=0,
                               not stalled, and BOTH seats' sims actually RAN
                               (simFrames). A joiner frozen at frame 1 with the link
                               perfectly up is a FAILURE here, which it used to be
                               reported as a pass.
  carstress  mp_carstress.py   3 seats cycle every car the roster offers: no dumps, no
                               freeze, and the list is COVERED, per seat. A shortfall is
                               the spare resident slots running out -- the car-swap
                               feature failing -- and it fails the rig.
  tries      mp_tries.py       a client joins and picks a guest car, three times: every
                               try passes its own checks and leaves a trace on a seat,
                               so a refused pick cannot read as a pass.

Exit codes mirror mp_smoke.py, so a CI job (or a human) can tell the states apart:

  0  every rig passed
  1  a rig failed
  2  SKIPPED - no game assets here, so nothing could run
  3  --require-assets was given and there are no assets

The rigs need ~1.6 GB of game assets and a display, which is why this cannot run in an
ordinary CI container. It SKIPS (2) where it cannot run, and a release job passes
--require-assets, turning "no assets" into a failure (3) instead of a quiet pass. That
is the honest shape: the gate runs where the assets are, and says so where they are not.

    python JERICHO/MODS/mp/tools/run_gate.py                 # the whole gate
    python .../run_gate.py --only smoke --runs 6 --seconds 60  # just the freeze bar
    python .../run_gate.py --require-assets                    # for a release job
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mp_smoke as smoke   # noqa: E402  (path first)


def run_rig(name, argv, timeout_secs):
    """Run one rig and show the tail of what it said. Returns (name, code)."""
    cmd = [sys.executable, os.path.join(HERE, argv[0])] + argv[1:]

    print(f"\n=== {name} ===\n$ {' '.join(cmd)}", flush=True)

    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_secs)
        out = (r.stdout or "") + (r.stderr or "")
        code = r.returncode
    except subprocess.TimeoutExpired as e:
        out = ((e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes)
               else (e.stdout or ""))
        out += f"\n{name} exceeded its {timeout_secs}s budget"
        code = 1

    lines = [l for l in out.splitlines() if l.strip()]

    # The tail is what a reader needs; the whole log lives in the pair dirs.
    for l in lines[-12:]:
        print("  " + l)

    return name, code


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", default=smoke.DEFAULT_GAME_DIR)
    ap.add_argument("--runs", type=int, default=2,
                    help="smoke pairs back to back (default 2; the freeze was 2-in-3, so "
                         "one pass proves little -- use 6 for a release bar)")
    ap.add_argument("--seconds", type=int, default=45,
                    help="seconds per smoke pair (default 45; the freeze was seen at 60)")
    ap.add_argument("--stress-seconds", type=int, default=60,
                    help="seconds for the car-swap stress (default 60)")
    ap.add_argument("--tries-seconds", type=int, default=35,
                    help="seconds per try in the guest-pick rig (default 35)")
    ap.add_argument("--only", action="append", default=[], choices=["smoke", "carstress", "tries"],
                    help="run only the named rig(s); repeatable")
    ap.add_argument("--require-assets", action="store_true",
                    help="exit 3 instead of skipping when the game assets are absent")
    a = ap.parse_args()

    if not smoke.assets_present(a.game_dir):
        print(f"[gate] SKIP: no game assets under {a.game_dir}")
        print("[gate] the rigs need the assets AND a display, so this host cannot run them.")
        return 3 if a.require_assets else 2

    rigs = [
        # one entry per rig: name, argv, worst-case seconds
        ("smoke",
         ["mp_smoke.py", "--game-dir", a.game_dir, "--runs", str(a.runs),
          "--seconds", str(a.seconds)],
         a.runs * (a.seconds + 40) + 30),

        ("carstress",
         ["mp_carstress.py", "--game-dir", a.game_dir, "--players", "3",
          "--bot", "off", "--seconds", str(a.stress_seconds)],
         a.stress_seconds + 120),

        # three tries, each paying a level load
        ("tries",
         ["mp_tries.py", "--game-dir", a.game_dir, "--seconds", str(a.tries_seconds)],
         5 * (a.tries_seconds + 45)),
    ]

    if a.only:
        rigs = [r for r in rigs if r[0] in a.only]

    results = [run_rig(*r) for r in rigs]

    print("\n=== gate ===")

    failed, skipped = [], []

    for name, code in results:
        if code == 0:
            state = "PASS"
        elif code in (2, 3):
            state = "SKIP (no assets)"
            skipped.append(name)
        else:
            state = "FAIL"
            failed.append(name)
        print(f"  {name:<10} {state}")

    if failed:
        print(f"\n[gate] FAIL: {', '.join(failed)} -- a rig did not clear its bar")
        return 1

    if len(skipped) == len(results):
        print("\n[gate] SKIP: no rig could run here")
        return 2

    print(f"\n[gate] PASS: {len(results) - len(skipped)} rig(s) cleared their bar"
          + (f"; skipped {', '.join(skipped)}" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
