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
    python .../mp_tries.py --scenario T2 --keep              # a car-switch release scenario
    python .../mp_tries.py --scenario all --keep             # T2, T3, T5 and T6

SCENARIOS (--scenario) are the car-switch release tests (#14 / #12; carhacks/MP_ADAPTER.md,
"Releasing a slot"). Each is a pre-set try - player count, picks, test levers and the lines
that must / must not appear - so the run is one word instead of a quoted command line:

    T2  switch until the spares would run out: the client changes car 7 times, more than the
        6 spare resident slots; every old car's slot must be RELEASED, the spares never exhaust
    T3  a shared car outlives its leaver: two joiners drive the same VEGAS car (roster slot 1,
        i.e. VEGAS model 2 - see CAR_SLOT_TO_MODEL below), one leaves; the slot must be KEPT
        ("still named by player N"), never released
    T5  the release outlives the old car: the client switches while its old car may still be
        drawn on the host; the host and the client must both RELEASE. The DEFER is reported,
        not required - it only exists if the host has not re-modelled the car yet
    T6  two same-city imports (roster slots 1 and 3), the slot-3 one switches away: only that
        slot goes, the shared pages stay, and crosscheck.py's invariants still hold on the host

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
    # one line per release decision (carhacks/net.c, chkNetReleaseSlotIfUnused) and the pool
    # summary CarSlotResReport prints after each - the numbers a leak shows up in
    "release": r"\[carhacks\] release: slot \d+ .*|cross-city: pool - .*",
}
EVIDENCE_LIMIT = 6

# The release line every scenario reads (carhacks/net.c). VERDICT is released/kept/deferred.
REL = r"\[carhacks\] release: slot \d+ \({car}\) {verdict}"
ANY_CAR = r"\w+ model \d+"

# The FRONTEND slot -> model table (Game/Frontend/FEmain.c: carNumLookup[4][10]; the
# same ten entries for every city). A roster slot - and therefore the harness lever
# CHK_FORCE_CAR - is a SLOT, not a model number: the module logs
#
#   [carhacks] car select: RIDE VEGAS slot 1 -> model 2
#
# so a scenario that names its car "VEGAS model 1" for slot 1 can never match the run it
# is describing, however right the release logic underneath it is. Every car name below
# goes through this, so that mistake is not available.
CAR_SLOT_TO_MODEL = (1, 2, 3, 4, 0, 8, 9, 10, 11, 12)


def car(city, slot):
    """The rig's name for '<slot> of <city>'s roster', spelled as the module logs it."""
    return f"{city.upper()} model {CAR_SLOT_TO_MODEL[slot]}"


# The test levers whose numbers are SECONDS, and which of their fields those are. --scale
# shortens a run by multiplying them; a lever field that is NOT seconds (a city, a model)
# must never be scaled, or the scenario quietly starts asking for a different car.
SCALED_LEVERS = {
    "MP_TEST_PAUSECAR": "first-of-each",	# 30,3,1;45,1,2 - field 0 of each ';' entry
    "MP_TEST_LEAVE": "all",
    "MP_TEST_ONFOOT": "all",
    "MP_TEST_RESTART": "all",
    "MP_TEST_CARCHANGE": "all",			# <changeSecs>[,<exitSecs>] - both are times
}


def scale_lever_value(key, value, scale):
    """MP_TEST_PAUSECAR=30,3,1;45,1,2 with scale 0.5 -> 15,3,1;22,1,2 (city/model untouched)."""
    mode = SCALED_LEVERS.get(key)

    if mode is None or scale == 1.0:
        return value

    def secs(tok):
        try:
            return str(max(1, int(round(int(tok) * scale))))
        except ValueError:
            return tok

    if mode == "all":
        return ",".join(secs(t) for t in value.split(","))

    out = []

    for entry in value.split(";"):
        fields = entry.split(",")
        fields[0] = secs(fields[0])
        out.append(",".join(fields))

    return ";".join(out)


def scale_seat_env(spec, scale):
    """SEAT=KEY=VALUE -> the same, with the lever's own seconds shortened by `scale`."""
    parts = spec.split("=", 2)

    if len(parts) != 3:
        return spec

    seat, key, value = parts
    return f"{seat}={key}={scale_lever_value(key, value, scale)}"

# The car-switch release scenarios (see the module docstring). `pick` is the frontend pick
# every joiner makes (a seat-env can override it per joiner); `seat_env` and `require`/`forbid`
# use mp_localpair.py's seat names (host, client = every joiner, client1, client2 ...).
SCENARIOS = {
    "T2": {
        "what": "switch until the spares would run out (7 switches, 6 spares): every old slot released",
        "pick": "vegas:1", "players": 2, "seconds": 120,
        "seat_env": ["client=MP_TEST_PAUSECAR=30,3,1;40,1,2;50,2,3;60,3,2;70,1,3;80,2,2;90,3,3"],
        "require": ["client=" + REL.format(car=ANY_CAR, verdict="released"),
                    "host=" + REL.format(car=ANY_CAR, verdict="released"),
                    r"client=\[mp\] test: PAUSECAR change 7 of 7"],
        "forbid": [r"no spare resident slot"],
    },
    "T3": {
        "what": "two joiners share the same VEGAS car (roster slot 1), one leaves: kept, not released",
        "pick": "vegas:1", "players": 3, "seconds": 100,
        "seat_env": ["client1=MP_TEST_LEAVE=60"],
        "require": ["host=" + REL.format(car=car("vegas", 1), verdict="kept") + r" - still named by player \d+",
                    r"host=\[carhacks/net\] player \d+ left the session"],
        "forbid": [REL.format(car=car("vegas", 1), verdict="released")],
    },
    "T5": {
        "what": "switch while the old car is still drawn on the host: released, and deferred first where the timing allows",
        "pick": "vegas:1", "players": 2, "seconds": 80,
        "seat_env": ["client=MP_TEST_PAUSECAR=35,3,1"],
        "require": ["host=" + REL.format(car=car("vegas", 1), verdict="released"),
                    "client=" + REL.format(car=car("vegas", 1), verdict="released")],
        "forbid": [],
        # DEFERRED IS TIMING, NOT A REQUIREMENT. A defer only exists while a car is still on
        # the slot; if the host has already re-modelled the peer's car when the PICK lands,
        # the release goes straight through and there is nothing to defer. Failing on it would
        # fail the run for being early, so it is REPORTED instead - the same call the PR body
        # makes ("if T5 fails only on that line, send me the host log").
        "soft": [("host", REL.format(car=car("vegas", 1), verdict="deferred") + r" - car \d+ still on it",
                  "the defer path was not exercised (the host had already re-modelled the car "
                  "when the PICK arrived) - say so rather than call it a regression")],
    },
    "T6": {
        "what": "two VEGAS cars imported (roster slots 1 and 3), the slot-3 one switches away: only its slot goes",
        "pick": "vegas:1", "players": 3, "seconds": 100,
        "seat_env": ["client2=CHK_FORCE_CAR=3", "client2=MP_TEST_PAUSECAR=30,3,1"],
        "require": ["host=" + REL.format(car=car("vegas", 3), verdict="released")],
        "forbid": [REL.format(car=car("vegas", 1), verdict="released")],
        "crosscheck": True,
    },
}

CROSSCHECK = os.path.join(HERE, "..", "..", "carhacks", "tools", "crosscheck.py")


def identity_check(host_text, client_text, guest_pick=True):
    """The two identity assertions from the plan, on the seat that matters.

    Both come from measured failures, not theory:

    1. THE PICKER'S OWN SEAT. A client that picks a guest car must not be sitting in the
       session city's car of that number. Measured (try 2, vegas:1):

         [carhacks/net] told the session: VEGAS model 2
         [carhacks] import: the pick (VEGAS model 2) -> resident slot 7
         [mp] player 0 changed car: slot 0 -> 1 (the session city model 2)

       the import is right, the advert is right, and the picker is in CHICAGO's model 2 -
       "the client was still rio car 1", one city along.

       SCOPED TO THE PICKER. On the picker's own machine the local player is id 0, and a
       peer's rebuild line is not this failure: matching any `player \\d+` reported the
       host's own car as the picker's.

    2. CROSS-SEAT AGREEMENT. What the picker says it drives and what the host says it
       drives must be the same car. When they disagree, each machine is internally
       consistent and the match still shows the wrong vehicle to somebody.

    `guest_pick` says the chooser picked a city other than the host's, which is when "the
    level's own model N" is a symptom rather than the ordinary, correct description of a
    native pick (a car that resolves to the level's own vehicle legitimately reads that way,
    including right after a rebuild).
    """
    problems = []

    # The picker's OWN car. Identified by the ROW FIELD the engine logs ([local=1 ...]):
    # the old form matched any "player 0 changed car: slot 0 -> ..." line, which is the
    # HOST's row on a client (id 0, isLocal 0) rebuilding the host's own car - correct
    # behaviour, reported as this failure. A remote row and the local row both print
    # "player 0" depending on the seat, so the row field is the only reliable marker.
    for m in re.finditer(r"\[mp\] player \d+ changed car: [^\n]*"
                         r"\(the session city model (\d+)\)[^\n]*\[local=1",
                         client_text):
        problems.append(f"the picker's own car is the SESSION city's model {m.group(1)}")

    for m in re.finditer(r"\[mp\] rebuilt player \d+'s car on slot \d+ "
                         r"\(model \d+ from the session city\)[^\n]*\[local=1", client_text):
        problems.append("the picker's own car was rebuilt from the session city")

    told = set(re.findall(r"\[carhacks/net\] told the session: (\w+) model (\d+)", client_text))
    seen = set(re.findall(r"\[carhacks/net\] peer \d+ drives (\w+) model (\d+)", host_text))

    if told and seen and not (told & seen):
        problems.append(f"the picker says it drives {sorted(told)}; the host says {sorted(seen)}")

    # A player who chose a GUEST car must never be described by the level's own numbers.
    if guest_pick:
        for m in re.finditer(r"\[carhacks/net\] (?:player|peer) \d+ drives level model (\d+)",
                             host_text):
            problems.append(f"a seat reports the guest picker as the level's own model {m.group(1)}")

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


def run_try(index, spec, args, scenario=None, name=None):
    """One pair run. With `scenario` (a SCENARIOS entry), its player count, seconds, levers and
    require/forbid lines are added to the ordinary try for its pick."""
    if scenario is not None:
        spec = scenario["pick"]

    city, model, city_index = parse_try(spec)
    port = args.port + index - 1
    game_dir = args.game_dir or DEFAULT_GAME_DIR
    run_dir = os.path.join(args.out, f"try{index}" if name is None else f"try{index}-{name}")
    players = scenario["players"] if scenario is not None else 2
    seconds = scenario["seconds"] if scenario is not None else args.seconds

    # --scale shortens a scenario's own run time as well as its levers' seconds, so a whole
    # pass costs less wall clock. 30s is the floor: below that the level load dominates and
    # the scenario stops being the scenario.
    if scenario is not None and args.scale != 1.0:
        seconds = max(30, int(round(seconds * args.scale)))

    env = dict(os.environ)
    # A joining client that reaches the module's car screen and picks for itself: the
    # whole point is that the PICK drives the car, not a launcher argument.
    env["MP_TEST_FRONTEND_JOIN"] = "1"
    env["CHK_FORCE_MENU"] = "1"

    cmd = [sys.executable, LOCALPAIR,
           "--players", str(players),
           "--seconds", str(seconds),
           "--port", str(port),
           "--level", CITY_DIR[args.host_city],
           "--game-dir", game_dir,
           "--keep",
           "--host-car", args.host_car,
           "--seat-env", f"host=CHK_FORCE_CAR={args.host_slot}",
           "--seat-env", f"client=CHK_FORCE_ROSTER_CITY={city_index}",
           "--seat-env", f"client=CHK_FORCE_CAR={model}"]

    if scenario is not None:
        for spec_env in scenario["seat_env"]:
            cmd += ["--seat-env", scale_seat_env(spec_env, args.scale)]

        for want in scenario["require"]:
            cmd += ["--require", want]

        for bad in scenario["forbid"]:
            cmd += ["--forbid", bad]

    for want in args.require:
        cmd += ["--require", want]

    cmd += args.extra

    if scenario is not None:
        print(f"\n=== try {index}: scenario {name} - {scenario['what']} "
              f"({players} players, {seconds}s) ===", flush=True)
    else:
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

    identity, pages, release = [], [], []
    host_text = client_text = ""
    seat_text = {}
    host_log = None

    for seat in "abcdefgh"[:players]:
        log_path = os.path.join(pair_dir, seat, "JERICHO.log")
        label = "host" if seat == "a" else ("client" if seat == "b" else f"client{ord(seat) - ord('a')}")

        try:
            with open(log_path, errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue

        if seat == "a":
            host_text = text
            host_log = log_path
        elif seat == "b":
            client_text = text

        seat_text[label] = text

        if args.keep:
            os.makedirs(run_dir, exist_ok=True)

            try:
                shutil.copy(log_path, os.path.join(run_dir, f"{label}.JERICHO.log"))
            except OSError:
                pass

        identity += [f"{label}: {l}" for l in grep(text, EVIDENCE["identity"])]
        pages += [f"{label}: {l}" for l in grep(text, EVIDENCE["pages"])]
        release += [f"{label}: {l}" for l in grep(text, EVIDENCE["release"], limit=12)]

    passed = proc.returncode == 0
    problems = identity_check(host_text, client_text,
                              guest_pick=(city != args.host_city))

    # T6: the cross-city invariants on the host's log, after a slot was released under a car
    # that shares its pages.
    if scenario is not None and scenario.get("crosscheck") and host_log is not None:
        cc = subprocess.run([sys.executable, CROSSCHECK, host_log],
                            capture_output=True, text=True, errors="replace")

        if cc.returncode != 0:
            tail = [l for l in (cc.stdout + cc.stderr).splitlines() if l.strip()][-4:]
            problems.append("crosscheck.py on the host log failed: " + " | ".join(tail))

    # A SOFT requirement is one the run can legitimately not exercise (see T5's `soft`).
    # Report it; never fail a try for it.
    notes = []

    if scenario is not None:
        for seat, pattern, why in scenario.get("soft", []):
            if not re.search(pattern, seat_text.get(seat, "")):
                notes.append(f"{seat}: {why}")

    # An identity problem fails the try even when the harness said PASS: "correct on the
    # host but the client was still the old car" is exactly the failure a verdict cannot see.
    if problems:
        passed = False

    return {
        "try": index,
        "pick": f"{city}:{model}" if name is None else f"{name} {city}:{model}",
        "pass": passed,
        "verdict": verdict_line,
        "problems": problems,
        "notes": notes,
        "identity": identity,
        "pages": pages,
        "release": release,
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
    ap.add_argument("--scale", type=float, default=1.0, metavar="F",
                    help="shorten every --scenario by F, INCLUDING the seconds inside its "
                         "test levers (MP_TEST_PAUSECAR/_LEAVE/_ONFOOT/_RESTART/_CARCHANGE), "
                         "so a pass costs less wall clock without changing what it "
                         "exercises - a lever's city and model are left alone. The run time "
                         "has a 30s floor, because below that the level load dominates. "
                         "e.g. --scale 0.5 takes T2 from 120s to 60s per seat")
    ap.add_argument("--port", type=int, default=1450,
                    help="first port; each try takes the next one (default 1450)")
    ap.add_argument("--scenario", action="append", default=[], metavar="NAME",
                    help=f"run a car-switch release scenario ({', '.join(SCENARIOS)}, or 'all'); "
                         f"repeatable. Without --try, only the scenarios run")
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

    names = []

    for want in args.scenario:
        for one in (SCENARIOS if want.lower() == "all" else [want.upper()]):
            if one not in SCENARIOS:
                raise SystemExit(f"[tries] unknown scenario '{want}' - one of "
                                 f"{', '.join(SCENARIOS)}, all")
            names.append(one)

    tries = args.tries or ([] if names else DEFAULT_TRIES)
    results = [run_try(i, spec, args) for i, spec in enumerate(tries, 1)]
    results += [run_try(len(tries) + i, None, args, SCENARIOS[n], n)
                for i, n in enumerate(names, 1)]

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

        for note in r["notes"]:
            print(f"    [note] {note}")

        for label, key in (("identity", "identity"), ("pages", "pages"), ("release", "release")):
            for line in r[key] or ["<nothing matched>"]:
                print(f"    [{label}] {line}")

    print()

    return 0 if all(r["pass"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
