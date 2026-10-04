#!/usr/bin/env python3
"""Run "the tries" as ONE command: a host on its own city, and a client that joins
picking a car from another city -- repeated per try, with a result table.

This is the automation of the shape the user kept testing by hand:

    host used chicago car slot 1
    try 1: client used rio car 1
    try 2: client used vegas car 1
    try 3: client used havana car 12

Each try is a full pair-run of its own (mp_localpair.py), so the client starts from
nothing, exactly as a fresh join does -- which is what makes a try a try. The
per-try evidence that matters is extracted from BOTH seats' logs and printed with
the verdict, because "it looked right on the host but the client was still the old
car" is the failure this exists to catch, and that is only visible if the two seats
are read separately:

    identity   what each seat says each player drives  (carhacks/net + mp lines)
    pages      whether the imported pages are pool-pinned, and what the palette
               walk refused (cross-city lines)

Usage:
    python JERICHO/MODS/mp/tools/mp_tries.py                 # the three tries above
    python .../mp_tries.py --try rio:1 --try havana:12 --keep --seconds 70
    python .../mp_tries.py --require "draws exactly that"    # a check per try

Exit status is 0 only if every try's verdict passed.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOCALPAIR = os.path.join(HERE, "mp_localpair.py")

# The repo root, so mp_localpair.py is invoked from where it expects to be: its default
# game dir is CWD-relative, and running it from this directory made it look for
# tools/src_rebuild/bin/Release_dev -- a try that fails to launch at all, and a FAIL that
# says nothing about the game. The absolute path is passed explicitly as well, so the run
# cannot depend on the CWD either way.
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
DEFAULT_GAME_DIR = os.path.join(REPO_ROOT, "src_rebuild", "bin", "Release_dev")

# The engine's own order (system.c: LevelNames). -level takes the directory name.
CITY_INDEX = {"chicago": 0, "havana": 1, "vegas": 2, "rio": 3}
CITY_DIR = {"chicago": "chicago", "havana": "havana", "vegas": "lasvegas", "rio": "rio"}

DEFAULT_TRIES = ["rio:1", "vegas:1", "havana:12"]

# What each seat says about the cars. Read from both seats, printed per try: these are
# the lines that told us "correct on the host, still the old car on the client".
EVIDENCE = {
    "identity": r"\[carhacks/net\] (?:player \d+ drives .*|peer \d+ drives .*)|\[mp\] (?:late joiner: .*|rebuilt player .*|player \d+ changed car: .*)",
    "pages": r"cross-city: (?:slot \d+ holds .*|set \d+ .*index .*|.*left unplaced.*|.*evicting the world.*)",
}
EVIDENCE_LIMIT = 6


def identity_check(host_text, client_text):
    """The two identity assertions from the plan, on the seat that matters.

    Both come from measured failures, not theory:

    1. THE PICKER'S OWN SEAT. A client that picks a guest car must not be sitting in the
       session city's car of that number. Measured (try 2, vegas:1):

         [carhacks/net] told the session: VEGAS model 2
         [carhacks] import: the pick (VEGAS model 2) -> resident slot 7
         [mp] player 0 changed car: slot 0 -> 1 (the session city model 2)

       the import is right, the advert is right, and the picker is in CHICAGO's model 2 -
       "the client was still rio car 1", one city along.

    2. CROSS-SEAT AGREEMENT. What the picker says it drives and what the host says it
       drives must be the same car. When they disagree, each machine is internally
       consistent and the match still shows the wrong vehicle to somebody.
    """
    problems = []

    for m in re.finditer(r"\[mp\] player \d+ changed car: slot \d+ -> \d+ "
                         r"\(the session city model (\d+)\)", client_text):
        problems.append(f"the picker's own car is the SESSION city's model {m.group(1)}")

    told = set(re.findall(r"\[carhacks/net\] told the session: (\w+) model (\d+)", client_text))
    seen = set(re.findall(r"\[carhacks/net\] peer \d+ drives (\w+) model (\d+)", host_text))

    if told and seen and not (told & seen):
        problems.append(f"the picker says it drives {sorted(told)}; the host says {sorted(seen)}")

    # A player who chose a car must never be described by the level's own numbers.
    for m in re.finditer(r"\[carhacks/net\] (?:player|peer) \d+ drives level model (\d+)", host_text):
        problems.append(f"a seat reports the picker as the level's own model {m.group(1)}")

    return problems


def parse_try(spec):
    """`city:model` -> (city, model, city_index)."""
    if ":" not in spec:
        raise SystemExit(f"[tries] --try wants CITY:MODEL (e.g. havana:12), got '{spec}'")

    city, model = spec.split(":", 1)
    city = city.strip().lower()

    if city not in CITY_INDEX:
        raise SystemExit(f"[tries] unknown city '{city}' - one of {', '.join(CITY_INDEX)}")

    return city, int(model), CITY_INDEX[city]


def grep(text, pattern, limit=EVIDENCE_LIMIT):
    out = []

    for m in re.finditer(pattern, text):
        out.append(m.group(0).strip())

        if len(out) >= limit:
            break

    return out


def run_try(index, spec, args):
    city, model, city_index = parse_try(spec)
    port = args.port + index - 1
    game_dir = args.game_dir or DEFAULT_GAME_DIR
    run_dir = os.path.join(args.out, f"try{index}")

    env = dict(os.environ)
    # A joining client that reaches the module's car screen and picks for itself: the
    # whole point is that the PICK drives the car, not a launcher argument.
    env["MP_TEST_FRONTEND_JOIN"] = "1"
    env["CHK_FORCE_MENU"] = "1"

    cmd = [sys.executable, LOCALPAIR,
           "--players", "2",
           "--seconds", str(args.seconds),
           "--port", str(port),
           "--level", CITY_DIR[args.host_city],
           "--game-dir", game_dir,
           "--keep",
           "--host-car", args.host_car,
           "--seat-env", f"host=CHK_FORCE_CAR={args.host_slot}",
           "--seat-env", f"client=CHK_FORCE_ROSTER_CITY={city_index}",
           "--seat-env", f"client=CHK_FORCE_CAR={model}"]

    for want in args.require:
        cmd += ["--require", want]

    cmd += args.extra

    print(f"\n=== try {index}: client picks {city.upper()} model {model} "
          f"(host {args.host_city.upper()} slot {args.host_slot}) ===", flush=True)

    proc = subprocess.run(cmd, cwd=REPO_ROOT, env=env,
                          capture_output=True, text=True, errors="replace")
    output = proc.stdout + proc.stderr

    verdict_line = ""
    for line in output.splitlines():
        if "verdict:" in line:
            verdict_line = line.strip()

    # A try that failed to LAUNCH is not a try that failed the requirement, and the
    # difference matters: it says the harness is broken, not the game. Say which.
    if not verdict_line:
        tail = [l for l in output.splitlines() if l.strip()][-6:]
        verdict_line = "the harness did not get as far as a verdict:"
        for line in tail:
            verdict_line += f"\n             {line}"

    # Keep this try's evidence: the pair dirs are reused per run, so copy them out
    # before the next try overwrites them.
    pair_dir = os.path.abspath(os.path.join(game_dir, ".mp-pair"))

    identity, pages = [], []
    host_text = client_text = ""

    for seat in ("a", "b"):
        log_path = os.path.join(pair_dir, seat, "JERICHO.log")

        try:
            with open(log_path, errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue

        if seat == "a":
            host_text = text
        else:
            client_text = text

        if args.keep:
            os.makedirs(run_dir, exist_ok=True)

            try:
                shutil.copy(log_path, os.path.join(
                    run_dir, f"{'host' if seat == 'a' else 'client'}.JERICHO.log"))
            except OSError:
                pass

        identity += [f"{'host' if seat == 'a' else 'client'}: {l}"
                     for l in grep(text, EVIDENCE["identity"])]
        pages += [f"{'host' if seat == 'a' else 'client'}: {l}"
                  for l in grep(text, EVIDENCE["pages"])]

    passed = proc.returncode == 0
    problems = identity_check(host_text, client_text)

    # An identity problem fails the try even when the harness said PASS: "correct on the
    # host but the client was still the old car" is exactly the failure a verdict cannot see.
    if problems:
        passed = False

    return {
        "try": index,
        "pick": f"{city}:{model}",
        "pass": passed,
        "verdict": verdict_line,
        "problems": problems,
        "identity": identity,
        "pages": pages,
        "dir": run_dir if args.keep else "",
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--try", action="append", default=[], dest="tries", metavar="CITY:MODEL",
                    help=f"what the joining client picks (repeatable; default "
                         f"{', '.join(DEFAULT_TRIES)})")
    ap.add_argument("--host-city", default="chicago",
                    help="the city the host hosts in (default chicago - the level's own "
                         "city, so every client pick is a guest)")
    ap.add_argument("--host-car", default="slot1",
                    help="the host's own car in the module's roster menu (default slot1)")
    ap.add_argument("--host-slot", type=int, default=1,
                    help="the host's car SLOT the harness drives (default 1 = the level's "
                         "second car, the shape the user's tries used)")
    ap.add_argument("--seconds", type=int, default=55,
                    help="match seconds per try (default 55; each try also pays the level "
                         "load, so budget a few minutes for all three)")
    ap.add_argument("--port", type=int, default=1450,
                    help="first port; each try takes the next one (default 1450)")
    ap.add_argument("--require", action="append", default=[],
                    help="passed to every try (see mp_localpair.py --require)")
    ap.add_argument("--keep", action="store_true",
                    help="copy each try's two logs into one place, so evidence survives the "
                         "next try's run")
    ap.add_argument("--out", default=None,
                    help="where --keep puts the per-try logs (default .mp-tries next to the "
                         "harness)")
    ap.add_argument("--game-dir", default=None, help="passed through to mp_localpair.py")
    ap.add_argument("extra", nargs="*",
                    help="anything else is passed straight to mp_localpair.py")

    args = ap.parse_args()

    if args.out is None:
        args.out = os.path.join(HERE, ".mp-tries")

    tries = args.tries or DEFAULT_TRIES
    results = [run_try(i, spec, args) for i, spec in enumerate(tries, 1)]

    print("\n=============================== tries ===============================")
    print(f"{'try':>3}  {'client pick':<14} {'result':<8} verdict")
    print("-" * 78)

    for r in results:
        tag = "PASS" if r["pass"] else "FAIL"
        print(f"{r['try']:>3}  {r['pick']:<14} {tag:<8} {r['verdict']}")

    for r in results:
        print(f"\n--- try {r['try']} ({r['pick']}) evidence ---")

        if r["dir"]:
            print(f"    logs: {r['dir']}")

        for problem in r["problems"]:
            print(f"    [identity PROBLEM] {problem}")

        for label, key in (("identity", "identity"), ("pages", "pages")):
            for line in r[key] or ["<nothing matched>"]:
                print(f"    [{label}] {line}")

    print()

    return 0 if all(r["pass"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
