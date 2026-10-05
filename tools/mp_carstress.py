"""Car-path stress test: every seat cycles through every car, flat out.

    python tools/mp_carstress.py                       # 3 seats, 60 s, a change every 700 ms
    python tools/mp_carstress.py --seconds 30           # quick
    python tools/mp_carstress.py --players 2 --interval-ms 250

One host plus N-1 clients on THIS machine, each walking the whole list of cars the session
can offer in its OWN random order, as fast as asked. Three seats at 700 ms is a car change
somewhere in the session every ~230 ms - far past anything a player does - which is where
slot reuse, the palette rows, the texture pages and the peer folds either hold or start to
leak, go invisible, or refuse.

It uses mp_localpair.py for the session and MP_TEST_CARCYCLE for the driving, so the changes
go through the same call the pause menu's Apply row makes. The list is the session's own
(MpCarListForCity), not a guess at model numbers.

What it reports, per seat: how many cars it actually drove, how many DISTINCT ones (the
coverage claim - "every car" is only true if this reaches the whole list), how many passes it
finished, and every sign of trouble:

  MISSING ....... a car whose mesh is not built: the invisible car, named
  no spare ...... the session ran out of resident slots
  not loaded .... a peer's car this machine could not hold
  keeping slot .. a car drawn from another car's slot
  refused ....... a change the session said no to

Exit code is 0 only when every seat cycled its whole list with none of the above.
"""

import argparse
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAIR = os.path.join(HERE, "mp_localpair.py")

SEATS = ["host", "client", "client1", "client2", "client3", "client4", "client5", "client6"]

TROUBLE = (
    ("MISSING", re.compile(r"MISSING")),
    ("no spare", re.compile(r"no spare resident slot")),
    ("not loaded", re.compile(r"is not loaded here")),
    ("keeping slot", re.compile(r"keeping slot \d+")),
    ("refused", re.compile(r"could not be built|refus|cannot load the car", re.I)),
)

CHANGE = re.compile(r"\[mp\] change car: we asked for (\w+) model (\d+)")
STATUS = re.compile(r"car status: driving model (\d+) \(resident (-?\d+), source (\w+)\)")
PASS = re.compile(r"car cycle PASS (\d+) done")
STARTED = re.compile(r"car cycle: (\d+) car\(s\), a change every (\d+) ms")
VERDICT = re.compile(r"\[pair\] verdict: (.*)")


def read(path):
    if not os.path.exists(path):
        return ""

    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def summarise(text):
    """Everything one seat's log says about the cycling."""
    cars = set()
    missing = []

    for m in STATUS.finditer(text):
        # (source city, model) is the car's identity; the source is the CITY it came from.
        cars.add((m.group(3), int(m.group(1))))

        if "MISSING" in text[m.start():text.find("\n", m.start())]:
            missing.append(m.group(0))

    passes = [int(x) for x in PASS.findall(text)]
    started = STARTED.search(text)
    trouble = []

    for label, rx in TROUBLE:
        hits = len(rx.findall(text))

        if hits:
            trouble.append((label, hits))

    return {
        "changes": len(CHANGE.findall(text)),
        "distinct": len(cars),
        "passes": len(passes),
        "offered": int(started.group(1)) if started else None,
        "trouble": trouble,
        "missing": missing,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--players", type=int, default=3, help="one host plus this many - 1 (default 3)")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--interval-ms", type=int, default=700,
                    help="how often each seat changes car (default 700, as asked for)")
    ap.add_argument("--game-dir", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(HERE)))),
        "src_rebuild", "bin", "Release_dev"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--bot", default="off", help="leave it off: a parked car is a still picture")
    a = ap.parse_args()

    if not os.path.exists(PAIR):
        print("cannot find %s" % PAIR)
        return 2

    seats = SEATS[:a.players]
    cmd = [sys.executable, PAIR,
           "--game-dir", a.game_dir,
           "--players", str(a.players),
           "--seconds", str(a.seconds),
           "--bot", a.bot]

    for s in seats:
        cmd += ["--seat-env", "%s=MP_TEST_CARCYCLE=%d" % (s, a.interval_ms)]

    if a.keep:
        cmd.append("--keep")

    print("stress: %d seat(s), a change every %d ms each (~%d ms somewhere in the session)"
          % (a.players, a.interval_ms, max(1, a.interval_ms // a.players)))
    print("        " + " ".join(cmd))
    print()

    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    verdict = VERDICT.search(out)

    # The rig writes a/b/c...; the seat names are in the same order.
    run_root = os.path.join(a.game_dir, ".mp-pair")
    names = ["a", "b", "c", "d", "e", "f", "g", "h"]

    print("%-8s %8s %9s %7s %9s  %s" % ("seat", "changes", "distinct", "passes", "offered", "trouble"))
    print("-" * 74)

    totals = collections.Counter()
    bad = 0
    coverage = []

    for s, d in zip(seats, names):
        info = summarise(read(os.path.join(run_root, d, "JERICHO.log")))
        trouble = ", ".join("%s x%d" % t for t in info["trouble"]) or "-"

        print("%-8s %8d %9d %7d %9s  %s" % (
            s, info["changes"], info["distinct"], info["passes"],
            info["offered"] if info["offered"] is not None else "?", trouble))

        totals["changes"] += info["changes"]
        totals["distinct"] = max(totals["distinct"], info["distinct"])

        if info["offered"] is not None:
            coverage.append((s, info["distinct"], info["offered"]))

        if info["trouble"] or info["missing"]:
            bad += 1

    print()
    print("%d car change(s) in %ds = %.1f/s somewhere in the session"
          % (totals["changes"], a.seconds, totals["changes"] / max(1, a.seconds)))

    if coverage:
        for s, got, want in coverage:
            if got < want:
                print("!! %s drove %d of the %d car(s) on offer - the list did not get covered"
                      % (s, got, want))
                bad += 1
            else:
                print("   %s drove every one of its %d car(s)" % (s, want))

    if verdict:
        print("pair verdict: %s" % verdict.group(1))

    if bad:
        print("\nRESULT: trouble above - read the seat logs in %s" % run_root)
        return 1

    print("\nRESULT: every seat cycled its whole list, no trouble in any seat's log")
    return 0


if __name__ == "__main__":
    sys.exit(main())
