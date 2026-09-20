# mp testing tools

Everything for driving a multiplayer session lives here — launchers and harnesses
together, so there is one place to look instead of a scatter of ad-hoc command
lines. The `.bat` files are thin wrappers; the real work is in the Python.

Path handling: the launchers find the game relative to themselves
(`tools/` → repo root → `src_rebuild/bin/Release_dev`), so they work from a
checkout anywhere. Set `MP_EXEDIR` to point them somewhere else.

## Which one do I want?

| I want to… | Use |
| --- | --- |
| test anything, on one PC | **`mp_pair.bat`** |
| host on this machine, by hand, two machines | `mp_host.bat` |
| join, by hand, two machines | `mp_join.bat` |
| drive the real game as a client with no second machine | `mp_mock_host.bat` |
| leave a session running with no game window | `mp_dedi.bat` |

## `mp_pair.bat` — two real instances, one PC

**Start here.** It runs a real host and a real client side by side and prints both
logs. Nearly every multiplayer bug so far showed up here, and the mock could never
have found them: it only ever puts *one* real engine in the room.

```
mp_pair.bat                       host + join, report, clean up
mp_pair.bat --keep                leave the run dirs so you can read the logs
mp_pair.bat --settle 30 --seconds 80
mp_pair.bat --clean               remove the run dirs again
mp_pair.bat --help                everything else
```

It builds two throwaway run directories beside the game from directory junctions,
so the big trees are shared and nothing is copied. **Separate working directories
are the whole trick** — they are what stop the two `JERICHO.log` files and the
two `mp.ini` files fighting each other, which is why two instances could not be
tested before this existed.

Both executables are launched **directly**, so the PIDs are real and the cleanup
stops exactly what it started. That is the one caveat on the other launchers:
they use `start`, which detaches, so **there is no PID to kill afterwards** — close
the game window instead of hunting for a process.

The host instance gets `MP_AUTOSTART=host` (it has to start the match itself now
that the attract demo is suppressed) and the client deliberately does not.

## `mp_host.bat` / `mp_join.bat` — two machines, by hand

```
mp_host.bat                 host on 1400
mp_host.bat 1500            ... on another port
mp_host.bat lobby           stay in the lobby instead of auto-starting
mp_host.bat dry 1500        print what would run, launch nothing

mp_join.bat 192.168.1.42    join a host by LAN address
mp_join.bat 192.168.1.42:1400
mp_join.bat                 host and client on this one machine
mp_join.bat dry 10.0.0.5    print what would run, launch nothing
```

Joining by address skips LAN discovery and needs only the host's session port
open, so it is the path to trust on a LAN with a firewall. `mp_join.bat` clears
`MP_AUTOSTART` — a client launches when the host's start arrives, and must never
be told to host as well — while `mp_host.bat` sets it, and leaves an
`MP_AUTOSTART` you set yourself alone.

## `mp_mock_host.bat` — a headless host

```
mp_mock_host.bat --start                   accept, then run a match
mp_mock_host.bat --start --peer-pad 0x40   and drive the "host" car
mp_mock_host.bat --reject mods             refuse the join, with a reason
mp_mock_host.bat --port 1400 --city 1 --start
```

Then point the game at it with `mp_join.bat 127.0.0.1:<port>`. It is one-sided by
nature — one real engine — so it can prove the wire format and the client's own
behaviour, never what two engines do to each other.

## `mp_dedi.bat` — a headless dedicated server

A real session with no game window, so a client has something to join and the
server can be left up while the client side restarts. It logs joins and leaves,
which is the fastest way to tell a client that never connected from one that
connected and was then dropped. Ctrl-C stops it.

## The Python underneath

| File | What it is |
| --- | --- |
| `mp_localpair.py` | the two-instance harness `mp_pair.bat` wraps; prints a PASS/FAIL verdict and reads `JERICHO.log` |
| `mp_test.py` | mock host / client / beacon, plus the protocol checks |
| `mp_dediserver.py` | the dedicated server `mp_dedi.bat` wraps |
| `check_debug_independence.py` | fails if any debug `getenv` guard wraps control flow or state (see the traps doc) |
| `_liveness_probe.py` | connects as a bare client and reports WELCOME/START/PONG/EOF -- the quick way to tell a server that answers from one that is silent |

**Liveness contract for any mock/dedi host.** A server here must answer a client's
`PING` with a `PONG` (echo the tick) and keep the connection up; a client that gets
nothing after the handshake drops itself at its idle timeout. The mock used to close
0.5 s after `WELCOME` and the dedi never replied to `PING`, so both looked like
instant drops. The wire format lives in `mp_test.py` -- when `mp_proto.h` changes,
update it there (e.g. `WELCOME` is `<12BIB`, 12xu8 + u32 seed + u8 hostCar; a
`<13BI` there crashes both simulated hosts).

`mp_localpair.py` takes `--no-debug` to run WITHOUT `MP_DEBUG=1` -- the packaged
launchers (PLAY_HOST/JOIN) never set it, so it is the only way to test what a player
actually runs (a bug that appears only without `MP_DEBUG` is invisible otherwise).

`mp_test.py` is also the reference for the wire format — it packs every message by
hand, so when a field changes there is exactly one other place to update.

## The test bot (`MP_BOT`, off by default)

Drives a real player's car so a pair can be exercised without two humans. It is a
testing component: nothing in the session/network path may depend on it, and it is
inert unless `MP_BOT` asks for it.

| `MP_BOT` | what it does |
| --- | --- |
| `random` (also `MP_TESTDRIVE=1`) | the canned manoeuvre: a pad that changes every 0.5–4 s |
| `chase` | the HOST flees, the joiner chases |
| `fight` | both charge each other |
| `pursuit` | MUTUAL chase: both cars hunt each other, so the pair reliably meets and collides |

`mp_localpair.py --bot <mode>` sets it on both instances. `pursuit` is the one to
reach for when you want the pair actually driving a distance around scenery (so the
network layer is exercised under real motion): the bots pick a clear heading with
the engine's own `CellEmpty` out of a fan of 15° steps covering a full ±180° (so a
heading AROUND a wall, up to a U-turn, is findable), looking both near (so they do
not nose into a wall) and far (so they do not commit to a gap that closes), hold a
chosen heading until it is blocked (otherwise the steering flaps), turn round when
the gap stops closing for 150 frames, and back out when they wedge. Their progress
and the resulting host<->client deviation are in the `[mp] bot:` and `[mp] sync:`
lines.

`--host-car default` (and `--client-car default`) pass NO `-mpcar` on that side, i.e.
exactly what a player who just presses Host/Join does: the ENGINE/level-facing
assignment decides the car (`config.car` stays -1). Use both together to reproduce
a whole no-`-mpcar` session -- the case where the two machines used to disagree
about who drives what (see trap 12 in `docs/ARCHITECTURE.md`).

## Updating the other PC — one command

    JERICHO\MODS\mp\tools\pack_lan\sync_lan.bat

Regenerates the project files (so the build stamp is current — `premake5 vs2019`
bakes `git describe --tags --always --dirty` in as `JERICHO_BUILD_VERSION`),
builds `Release_dev`, and writes `REDRIVER2_mp_lan_<build>.7z` in the project
root. Copy that ONE file to the other machine and extract it over the folder; that
machine needs neither Visual Studio nor git.

**The stale-exe symptom.** An older exe on one side does not necessarily fail
outright — it half-works: the two players never see each other's car, or the HUD
lists a peer that never moves. Check the build before anything else:

* the startup line `[mp] multiplayer ready (... build be0d, mods 13bd)` — both
  machines must print the same `build` and `mods`;
* the pause-menu scoreboard, which prints those same digests under
  `-- PLAYERS --` as `build be0d  mods 13bd`.

`<build>` in the package name is that same string, so the file you copied and what
a machine reports can be compared directly. The package also ships
`JERICHO\CONFIG\mp.ini` with `strict_version = 1`, which REFUSES a peer on a
different exe at the handshake instead of letting it desync mid-race; set it to 0
if you want to test mismatched builds deliberately.

## Testing on the other PC without touching it (the agent)

    JERICHO\MODS\mp\tools\remote\START_AGENT.bat      <- run ONCE on the other PC
    python JERICHO\MODS\mp\tools\remote\mp_remote.py run --peer 192.168.50.244 --seat host

Copy the two files in `tools\remote\` (they already ride along in the LAN package)
next to `REDRIVER2_dev.exe` on the other machine, double-click `START_AGENT.bat`
once, and leave the window open. That machine is then a **fixture**, not a
chore: everything below happens from here, and you never touch it again.

    status   what build it is on, whether the game is up, how many files differ
    deploy   send only what CHANGED, then start both seats on the same build
    run      deploy, wait, pull BOTH logs back, print a PASS/FAIL verdict
    logs     pull both logs and give the verdict
    stop     close the game on both machines (PID-scoped: only the one we started)

`deploy`/`run` send the exe, `JERICHO` and `VERSION.txt` — a few MB — and never
the 1.6 GB of game data, because only those files ever change between builds. The
agent verifies every file's SHA256 against the manifest inside the package and
refuses the WHOLE update if one file disagrees, so a bad transfer can never leave
a half-applied build.

**Hands-free** means the agent is resident: if a sync arrives while a game is
running it stops the game, updates, and starts it AGAIN with the same arguments.
Leave that PC running a client, push a build from here, and watch the new build
come up on its own. Its log of every command and what it did is `mp_agent.log`
next to the game.

Two things the loopback dry run taught us, worth knowing because they look like
the agent is broken:

* **A game started by the agent is stopped when a later `start`/`sync` arrives.**
  It checks "is a game running" by process name, so on ONE machine the two seats
  cannot coexist — that is only an artifact of testing both halves locally, and is
  exactly what you want on two machines.
* **The first run needs the firewall.** `START_AGENT.bat` as administrator, once,
  or the connection is refused (it says so).

The token (`-Token`, default `jericho-mp`) is not a security boundary: it stops a
stray program on the same LAN from driving that machine by accident.

## A crash is not an Alt+F4

An access violation leaves `JERICHO.dmp` beside the exe; an Alt+F4 — or any clean
exit — leaves none, and the log simply stops. So:

    ls JERICHO.dmp                                # crash, or did someone close it?
    python tools/dmp_fault.py JERICHO.dmp         # exception + module + RVA
    python tools/map_lookup.py <exe>.map 0xC961   # RVA -> the function

`dmp_fault.py` prints `in module REDRIVER2_dev.exe at rva 0x....`; give that RVA to
`map_lookup.py` together with the `.map` beside the exe and it names the function.
Correlate with the log's own tail: the last `[mp]` lines are what it was doing —
though a buffered log can lose the final line or two, so read it as "around here",
not "exactly here".

**Give a run enough time.** `--settle` is the wait before the client joins and
`--seconds` is the TOTAL run, so a large `--settle` with a short `--seconds` leaves
almost no match to observe (both sides stop at ~150 carstates, symmetrically, which
looks like a fault and is not). `--seconds 60 --settle 5` gives a full window.

## Driving a car change by hand

`MP_TEST_CARCHANGE=<seconds>[,<exitSeconds>]` makes every machine it reaches get out
of its car and into the nearest civilian one that many seconds into a live match,
and — with the second number — get out again that long after. It calls the ENGINE's
own `ChangePedPlayerToCar` / `ChangeCarPlayerToPed`, i.e. the real ped path, so what
is under test is the game's own code.

    MP_TEST_CARCHANGE=8,10 python mp_localpair.py --seconds 60 --settle 5

Use it to check that the other machine's copy of your car becomes the vehicle you
actually got into (`[mp] player N changed car: model A -> B (slot S)`).

## Reading a run

The logs are chatty at `MP_DEBUG=1`; `JPPN`, `JPPO`, `pose:`, `JPIN` and `JPCS`
flood them. Filter those out:

```sh
grep -a "\[mp\]\|\[error\]" JERICHO.log | grep -av "JPPN\|JPPO\|pose:\|JPIN\|JPCS"
```

Useful markers: `launching: city N mode M (1=TAKEADRIVE, 0=MISSION!)` — mode 0
means the mission ladder, i.e. the launch went wrong — `car: player N slot S`,
`added N remote player car(s)`, `map: drew N remote blip(s)`, `list:` (the pause
menu rows), and `peer dropped (<why>)`.
