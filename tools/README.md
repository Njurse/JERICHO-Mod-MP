# mp testing tools

Everything for driving a multiplayer session lives here — launchers and harnesses
together, so there is one place to look instead of a scatter of ad-hoc command
lines. The `.bat` files are thin wrappers; the real work is in the Python.

Path handling: the launchers find the game relative to themselves
(`tools/` → repo root → `src_rebuild/bin/Release_dev`), so they work from a
checkout anywhere. Set `MP_EXEDIR` to point them somewhere else.

Packaging a build to hand to someone else is a different job in its own folder:
`pack_lan/` holds both packagers (the public one and the Driver 1 hand-off), the
launchers they ship, and its own README explaining which to use.

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
| `mp_localpair.py` | the two-instance harness `mp_pair.bat` wraps; prints a PASS/FAIL verdict and reads `JERICHO.log` (also runs 3..8 seats -- see `--players`) |
| `mp_smoke.py` | the smoke GATE around `mp_localpair.py`: turns the verdict into an exit code (PASS + `lost=0` + `dumps=0` + not STALLED). Exits 2 (skip) where the game assets are absent, so a CI job can tell "not runnable here" from "failed"; `--require-assets` makes a skip a failure |
| `mp_crashrate.py` | repeat `mp_localpair.py` N times and report the CRASH RATE, plus the free car-slot count the `PingInCivCar` breadcrumbs print. Exits non-zero if any run crashed |
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
`<13BI` there crashes both simulated hosts). That includes *removals*: a retired tag
is dropped from `TAG` too, so the mirror never advertises a tag the code does not have
(it no longer carries `JPSS`).

`mp_localpair.py` takes `--no-debug` to run WITHOUT `MP_DEBUG=1` -- the packaged
launchers (PLAY_HOST/JOIN) never set it, so it is the only way to test what a player
actually runs (a bug that appears only without `MP_DEBUG` is invisible otherwise).

### Asserting a result, not just an absence: `--require`, and `mp_tries.py`

`--forbid` says what must never appear. `--require` is its mirror: the run FAILS
unless the line appears.

```
--require "draws exactly that"              any seat may carry it
--require "client=[carhacks/net] peer 0 drives CHICAGO"   that seat's log only
```

It exists because "nothing crashed" is not the same as "the right car was drawn", and
the difference was invisible in a wall of logs. A missing `--require` is reported by
seat and pattern, so the failure names the thing that did not happen.

`mp_tries.py` runs the shape that matters most as ONE command: a host on its own city
and a client that joins picking a car from another city, repeated per try, with a
per-try table plus the identity and page evidence from BOTH seats.

```
python JERICHO/MODS/mp/tools/mp_tries.py                 # rio:1, vegas:1, havana:12
python .../mp_tries.py --keep --seconds 70 --try havana:12 --require "draws exactly that"
```

Each try is a full pair-run of its own, so the client starts from nothing -- exactly
as a fresh join does. `--keep` copies each try's two logs into `.mp-tries/tryN/`
before the next try reuses the pair dirs, so the evidence survives.

Two things it prints that a bare verdict does not:

* **the identity evidence per try**, from both seats -- "correct on the host but the
  client was still the old car" is a real failure mode here, and it is only visible if
  the two seats are read separately;
* **the page evidence per try** -- whether the imported pages were pool-pinned
  (`2 pin(s) (2 in the pool)` vs `(0 in the pool)`), and any line where the palette
  walk refused or evicted a world page.

A note on the numbers: `--try CITY:MODEL` names the car in the ROSTER menu, and the
harness's slot numbers do not line up with the engine's model numbers (`--try rio:1`
loads RIO model 2, because the roster's first row is not the city's model 0). Read the
try as "a car from that city"; the evidence lines carry the exact (city, model).

### Ending a run early, and trusting the tail
`mp_localpair.py` judges a RUNNING game -- `verdict()` reads both logs before anything
is killed -- so what it can see is decided by what the game has actually written to
disk. Two levers make that reliable, and the harness sets both unless you override
them:

* `JERICHO_LOG_FLUSH=1` flushes the engine's log per line. Without it the log is a
  buffered `FILE*` that is flushed only when a loading screen goes up
  (`PsyX_BeginScene`, which the engine calls from exactly two places), so the file
  silently loses its tail -- and a line written just before the game hangs never
  reaches it at all, which reads exactly like "that code never ran". It has already
  cost a session: an unflushed on-foot line was recorded as "the eject lever never
  fires", when the lever fires every time.
* `MP_HEARTBEAT=<secs>` has the module log its own tick once per N seconds. Every
  other periodic line in the module is `MP_DEBUG`-gated and `--no-debug` is exactly
  the case worth testing, so without this a FROZEN game and a quiet one look identical
  -- and a frozen game has already been recorded here as a healthier run than a
  working one.

Run `JERICHO_LOG_FLUSH=0` once and the line is simply absent -- the same test loses its
own evidence, and a lost tail has already been read here as a PASS.

The levers the harness then gives you:

```
python mp_localpair.py --until "getting OUT"       # stop the moment it appears
python mp_localpair.py --forbid "Lost the server"  # stop AND fail on this marker
python mp_localpair.py --stall 10                  # no tick for 10s -> STALLED
python mp_localpair.py --tail 3                    # print both tails while waiting
python mp_localpair.py --menu-host                 # host in the FRONTEND (no -level);
                                                   # fails unless it starts exactly ONE match
python mp_localpair.py --sp                        # a SINGLE-PLAYER level (omits -mp):
                                                   # the only rig that reaches region 0
```

`--sp` exists because **`-mp` always selects a multiplayer region** — the engine sets
`gBootMpLevel = 1` for any `-mp` (main.c), so every other rig here, and every packaged
launcher, only ever runs a multiplayer map. Anything keyed off `MissionHeader->region`
(the overhead map's placement is one) is therefore untested without it. Measured: with
`--sp` both seats come up on the same level at `subgame 0` with the remote car present.

One thing to expect while reading an `--sp` run's map lines: the single-player mini-map
is a **small window centred on the local car**, so a remote player's arrow only appears
while that player is inside it. A missing arrow there is the engine's own clipping (the
same the local marker gets), not a sync fault — watch the logged map coordinates instead
of the picture.

`--until` and `--forbid` are repeatable regexes matched against EITHER log, so a run
ends when the thing under test has happened instead of sitting out `--seconds`.
`--stall` watches the heartbeat: **a heartbeat that stops advancing means the
SIMULATION stopped, not that the game went quiet.** **STALLED is not a PASS**, and a
`--forbid` match is not one either. A frozen game leaves no crash dump and has already
logged every connection marker in its first seconds, which is why it used to read as a
good run.

`mp_test.py` is also the reference for the wire format — it packs every message by
hand, so when a field changes there is exactly one other place to update.

## More than two seats

    python mp_localpair.py --players 3                  # host + 2 joiners
    python mp_localpair.py --players 4 --stagger 20     # host + 3, staggered

`--players N` (2..8) runs the host plus N-1 joiners, each in its own run dir. The
FIRST joiner arrives before the match starts -- it is what starts it, via
`MP_AUTOJOIN_START=2` -- and every later one is staggered by `--stagger` seconds so
it joins a match that is ALREADY LIVE. That is the only way the late-join spawn
path and the host's relay (`MpHostRelay`) get exercised at all: with a pair, both
machines build their cars at level init and neither path runs.

The verdict is per seat rather than per pair: the host must log one join per joiner,
every joiner its own "accepted as player", and `never accepted: <seat>` names
whoever did not get in. A run whose window is too short for its seats says so and
raises `--seconds` itself.

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
builds `Release_dev`, and writes `JERICHO_mp_lan_<build>.7z` in the project
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

    JERICHO\MODS\mp\tools\remote\START_AGENT.bat -Bind <its LAN ip>   <- run ONCE on the other PC
    set MP_AGENT_TOKEN=<the token it printed>
    python JERICHO\MODS\mp\tools\remote\mp_remote.py run --peer 192.168.50.244 --seat host

Copy the two files in `tools\remote\` (they already ride along in the LAN package)
next to `JERICHO_dev.exe` on the other machine, run `START_AGENT.bat` once with that
machine's LAN address (as administrator the first time, for the firewall rules),
and leave the window open. That machine is then a **fixture**, not a chore:
everything below happens from here, and you never touch it again.

    status   what build/release it is on, whether a rollback exists, whether the game is up
    update   install a GitHub release there: the rolling 'alpha' by default, or --tag v0.9.1
    deploy   update (skip with --no-update), then start both seats
    run      deploy, wait, pull BOTH logs back, print a PASS/FAIL verdict
    logs     pull both logs and give the verdict
    rollback put back the build the last update replaced
    stop     close the game on both machines (PID-scoped: only the one we started)

**Nothing is pushed to the agent.** An update carries only a release tag; the agent
downloads that release's **Release** Windows zip (`JERICHO_Release_win64.zip`,
the only asset it accepts; every other asset is refused) from GitHub over
HTTPS itself, checks the zip's SHA256 against the digest GitHub publishes for the
asset (or, failing that, the `SHA256SUMS` asset published beside it — never a
manifest inside the zip),
unpacks it in a staging folder, and only then swaps the exe, its DLLs and `JERICHO`
in, keeping what they replaced in `_mp_previous` for `rollback`. A bad download
can never leave a half-applied build, and the 1.6 GB of game data is never touched.
Because the peer runs a PUBLISHED build, push to main (or tag) and let CI publish
before testing a change on two machines.

**Hands-free** means the agent is resident: if an update arrives while a game is
running it stops the game, installs, and starts it AGAIN with the same arguments.
Leave that PC running a client, ask for an update from here, and watch the new
build come up on its own. Its log of every command and what it did is
`mp_agent.log` next to the game.

Two things the loopback dry run taught us, worth knowing because they look like
the agent is broken:

* **A game started by the agent is stopped when a later `start`/`update` arrives.**
  It checks "is a game running" by process name, so on ONE machine the two seats
  cannot coexist — that is only an artifact of testing both halves locally, and is
  exactly what you want on two machines.
* **The first run needs the firewall.** `START_AGENT.bat` as administrator, once,
  or the connection is refused (it says so). The rules only cover the Private
  network profile and the local subnet.

**Security.** The agent listens on 127.0.0.1 unless given `-Bind <LAN address>`, and
refuses to bind every interface. Its token is random, generated on first start and
kept in `mp_agent.config.json` beside the game (gitignored, never logged); the old
default `jericho-mp` is refused. It is still a plain-text protocol meant for a LAN
you trust: do not forward port 1401 to the internet.

## A crash is not an Alt+F4

An access violation leaves `JERICHO.dmp` beside the exe; an Alt+F4 — or any clean
exit — leaves none, and the log simply stops. So:

    ls JERICHO.dmp                                # crash, or did someone close it?
    python tools/dmp_fault.py JERICHO.dmp         # exception + module + RVA
    python tools/map_lookup.py <exe>.map 0xC961   # RVA -> the function

`dmp_fault.py` prints `in module JERICHO_dev.exe at rva 0x....`; give that RVA to
`map_lookup.py` together with the `.map` beside the exe and it names the function.
Correlate with the log's own tail: the last `[mp]` lines are what it was doing. A log
written without `JERICHO_LOG_FLUSH=1` can lose its tail, which turns the last line
into an "around here" -- the harness sets the flush for you, so read a run you
launched by hand with that in mind.

**Give a run enough time.** `--settle` is the wait before the client joins and
`--seconds` is the TOTAL run, so a large `--settle` with a short `--seconds` leaves
almost no match to observe (both sides stop at ~150 carstates, symmetrically, which
looks like a fault and is not). `--seconds 60 --settle 5` gives a full window.

## Joining through the menus (so a CLIENT reaches the vehicle select)

`MP_TEST_FRONTEND_JOIN=1` makes a `-join` keep the front end instead of taking the
unattended path. Without it, every client lever (`-join`, `MP_AUTOSTART=join`) sets
`autoSession` — "no menus: launch as soon as we are in" — so the client never opens
the car screen, and "does a joiner reach the vehicle select and its roster?" cannot
be answered headlessly. With it the client takes the route a person at the machine
takes: `WELCOME` -> `MpUiOpenCarSelect` (screen 14) -> the menu -> `Ride`, which
hands the launch back through `JER_EVENT_MP_FRONTEND`.

Combine it with carhacks' own harness, which walks to the car screen and drives its
rows so no pad is needed:

    MP_TEST_FRONTEND_JOIN=1 CHK_FORCE_MENU=1 CHK_FORCE_CAR=8 CHK_FORCE_ROSTER_CITY=1 \
        python mp_localpair.py --players 2 --seconds 90 --settle 6

`CHK_FORCE_CAR` is the row index in the roster and `CHK_FORCE_ROSTER_CITY` picks
WHICH city's roster you are browsing (the pick becomes a cross-city car), so the
pair covers "scroll the cars", "scroll the cities" and the pick that follows.

## The driving bot, and the proximity it keeps

The bot (mp/mp_bot.c, on only when `MP_BOT` says so) is a **sparring partner, not a
navigator**: `chase`/`fight`/`pursuit` probe for scenery with the engine's own `CellEmpty`
and steer around what it sees, with no route. `catmouse` is the exception and the reason
the roadmap's "a real fix wants a lookahead" item closed: it drives the same pair from a
real pathfinder (`MODS/mp/ai/` - an occupancy grid, A*, and a local road search), so at
least one behaviour set DOES know the roads and drives a route. Whichever set is running,
its job is to make two (or three) real player cars meet, collide and hand those collisions
to their owners.

`--bot chase` (the rig's default) is the shape that does it: **the host flees and every
joiner chases the host**. Two rules keep the pair close enough to actually touch, because
the flee has no business opening the gap forever:

| gap | what the fleeing host does |
|---|---|
| under 7500 | full pace, proper running away |
| 7500 to 15000 | runs on (the old "lift the throttle" ease is gone - see below) |
| over 15000 | stops fleeing and drives BACK at the chasers, so they meet from both ends |

`MP_BOT_GAP=<ease>,<turnback>` moves both thresholds (defaults `7500,15000`) without a
rebuild - how close the pair should stay is a feel question. **Distance no longer
interrupts the driving**: the ease threshold only labels the branch in the log, because
lifting the throttle read as the car giving up mid-flee when its line was good; the
chaser also presses to 450 units before it pauses rather than idling 900 out. The chase log
line names the branch it took: `flee` / `flee-hold` (easing, no longer a throttle cut) /
`loop` (coming back) / `chase` / `fight`, printed once a second with the gap as `d=x,z`,
`diff` (steering error), the pad bytes and the stuck count. Measured on a 50 s city pair
with the current logic: neither threshold fired at all and the two stayed in contact.

Measured before and after the proximity rule (3 seats, chase):

     before:  d=938 -> 6017 -> 11607 -> 20077 -> 28434 -> 34924 (stuck=37) -> 38246 (stuck=97)
     after:   host max|d|=1939 mean=936, chaser max|d|=3621 mean=2098, max stuck 14

Useful assertions for a rig run (the harness takes `--until`/`--forbid` regexes over the
seat logs):

    --forbid "d=-?[0-9]{5}"     never 10,000+ units apart (the runaway this replaced)
    --forbid "stuck=[5-9][0-9]" never wedged for a second or more
    --until  "hit: "            a collision was actually handed off

A PASS from this rig also needs `lost=0` and `dumps=0`; note that a mid-run peer loss
still shows up as `lost=2` occasionally - that is the pre-existing disconnect, not the
bot (an unmodified-build control reproduces it).

## Giving each seat its own pick

`--seat-env SEAT=KEY=VALUE` (repeatable; seats are `host`, `client` = every
joiner, and `client1`, `client2` ... = one joiner) sets an environment variable for
that seat alone. Exported variables reach every seat, so a shared
`CHK_FORCE_CAR` has all of them ride the SAME car — which hides whether a peer's
pick is really respected on the other machines. Give them different ones:

    MP_TEST_FRONTEND_JOIN=1 CHK_FORCE_MENU=1 python mp_localpair.py --players 3 \
        --seat-env host=CHK_FORCE_ROSTER_CITY=3 --seat-env host=CHK_FORCE_CAR=0 \
        --seat-env client=CHK_FORCE_ROSTER_CITY=1 --seat-env client=CHK_FORCE_CAR=2

Each seat then logs its own `RIDE <CITY> slot N -> model M`, and the host logs per
peer whether it draws that car (`peer N drives <CITY> model M and this machine
draws exactly that (slot K)`) or not.

## Driving a car change by hand

`MP_TEST_CARCHANGE=<seconds>[,<exitSeconds>]` makes every machine it reaches get out
of its car and into the nearest civilian one that many seconds into a live match,
and — with the second number — get out again that long after. It calls the ENGINE's
own `ChangePedPlayerToCar` / `ChangeCarPlayerToPed`, i.e. the real ped path, so what
is under test is the game's own code.

    MP_TEST_CARCHANGE=8,10 python mp_localpair.py --seconds 60 --settle 5

Use it to check that the other machine's copy of your car becomes the vehicle you
actually got into (`[mp] player N changed car: model A -> B (slot S)`).

## Chat by hand (a headless run has no keyboard)

`MP_TEST_CHATKEY=<seconds>[,<text>]` feeds the chat KEY into the module's own key
handler once, that many seconds into a live match, and — with a text after the
comma — types it through the real character handler, backspaces once, presses
Enter and logs the buffer. So the whole open → type → send path runs with no
keyboard at all.

    MP_TEST_CHATKEY=10,hello python mp_localpair.py --seconds 45

In game, chat opens on **T** and closes on **Escape**, and **Enter** sends. A
received line is an ordinary notify, so every seat logs it as
`[mp] notify row '<name>: <text>'`.

## The manual address field, and joining by name

The manual join field is TYPED, not adjusted: Cross on the row starts the keyboard,
Cross again (or Enter) keeps what you typed, Escape undoes it. It takes an IPv4
address **or a DNS name**, and an optional `:port` -- `play.example.net:1318` and
`192.168.1.20:1318` are both fine. A port that is not a number (or is out of range) is
refused with a logged reason rather than silently dialled on the configured port.
`-join <host>[:port]` takes exactly the same thing.

The harness has no keyboard, so the field is exercised through a lever instead:

    MP_TEST_MANUALADDR=<host>[:port] python mp_localpair.py --seat-env client=MP_TEST_MANUALADDR=localhost:1400

It fills the field and presses Connect through the SAME code path the row uses, so a
run reports:

    [mp] test: manual address <- 'localhost:1400' (MP_TEST_MANUALADDR)
    [mp] joining manual localhost:1400

and a bad port gives `manual address '...' does not parse (want host or host:port)`.

`mp_localpair.py --join-host <name>` makes the client seats dial a NAME rather than
127.0.0.1, which is the DNS path end to end.

## The Single Player / Multiplayer prompt (a headless run has no pad)

The take-a-ride city confirm is the one place that asks Singleplayer vs
Multiplayer — through mp's own `mp.mode` menu inside a session, or levelhacks'
prompt outside one (levelhacks yields to mp whenever a session role is set). It
fires on CROSS (`JER_EVENT_FRONTEND`), and the frontend reads `Pads[0].mapnew`, so
a padless run cannot press it. `MP_TEST_CITYCONFIRM=<secs>` fires the confirm the
way the engine does, that many seconds after the frontend is up:

    MP_AUTOSTART=host MP_TEST_CITYCONFIRM=4 JERICHO_dev.exe -nointro -nofmv

A healthy host run logs, in this order:

    [mp] city confirmed - asking Single Player / Multiplayer
    [mp] test: MP_TEST_CITYCONFIRM -> city confirm fired (defer=1 role=1)
    [mp] opening mp.mode (resolved idx 4)

and NOT `the Single Player / Multiplayer menu did not open (idx …, on screen …)`.
So a wrapper run asserts it with:

    --require "MP_TEST_CITYCONFIRM -> city confirm fired"
    --forbid  "the Single Player / Multiplayer menu did not open"

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

A run is judged as well as read: `mp_localpair.py` ends with a verdict, and two of its
values are findings rather than quiet windows -- **STALLED** (the module's own tick
stopped advancing, so the simulation stopped) and a `--forbid` match. See "Ending a run
early, and trusting the tail" above.
