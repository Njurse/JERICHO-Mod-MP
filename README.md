# Multiplayer (mp)

**New here? Start with [`docs/JERICHO-MP.md`](docs/JERICHO-MP.md)** — what a match
is, who owns which car, how a player joins, and the things that cost days, in plain
language. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) is the deep version.

LAN multiplayer for REDRIVER2, built as a JERICHO deep mod (compiled into the
game like `levelhacks`, so it can read/write game globals). Host on TCP/UDP
**1318** (configurable), join over LAN with UDP discovery, and play a shared
world. The first gamemode is **Take a Ride**: the host picks city, time of day
and weather, and players spawn into the same level.

## How it looks

The menus are **real frontend screens** (not an overlay): the main-menu
**Multiplayer** entry opens a native menu tree, rendered and navigated by the
engine through the JERICHO frontend-menu API (`jer_frontend.h`):

```
Multiplayer
  Host Game ......... gamemode -> city / time / weather / enforce-mods -> Start
  Join Game ......... LAN server list / manual IP
  Options ........... Change Name / Enforce Mods / Port
  Local Split-Screen  (returns to the stock 2-pad flow)
```

`Enforce Mods` is the host's lobby setting (Off / By ID / By ID+Version): the
join handshake then admits or refuses clients by their enabled-mod manifest.

## How it works

- **Transport** — non-blocking TCP session + UDP discovery (`mp_net.c`).
- **Handshake** — client sends `MP_HELLO` (identity + mod manifest); the host
  validates version and applies the mod-match policy, answering `MP_WELCOME`
  or `MP_REJECT`.
- **Synchronized start** — the host broadcasts `MP_START` with the session
  config; every machine runs `SetState(STATE_GAMESTART)` with the host's city,
  time and weather. The host adds one `PlayerStartInfo` slot per remote player
  (`JER_EVENT_NET_SPAWN`), so everyone has a car.
- **Owner-authoritative car state** — each machine is the sole authority on the
  one car it drives, and broadcasts that car's whole rigid body (position,
  orientation, both velocities, model, colour) every frame (`MP_CARSTATE`).
  Every other machine adopts it verbatim, so a remote car can never rubber-band
  against its own driver: the only thing that moves your car is you.
- **Input replication is the fallback** — each machine also sends its pad
  (`MP_INPUT`); a car whose owner state has not arrived yet is driven from the
  replicated input (`JER_EVENT_NET_INPUT`) rather than stalling, so a slow link
  costs smoothness, never a frozen frame.
- **Contacts are handed off** — a machine can only move its OWN car, so when
  yours touches a peer's you push yours and report it (`MP_HIT`); the peer's
  machine pushes theirs. Both cars move and each stays its owner's truth. The
  contact comes from the engine's OWN collision event (so a hit is registered
  even when our engine has already absorbed it), and the receiving side applies
  the push once — not on top of its own engine's response to the same contact.
- **Chat** — press `T` to type a line, `Enter` to send, `Escape` to cancel; the
  owner echoes it and the host fans it out, so every seat sees it (`JPCX`).
- **The host owns the roster** — `MP_ROSTER` publishes who is in the match,
  ascending player id (host first), with each player's name, vehicle and ping.
  It goes out *before* a launch so every machine knows how many cars to spawn
  before its level loads, and is refreshed every couple of seconds. Car slots are
  handed out walking player ids, because that is the only ordering both machines
  agree on -- walking a per-machine registry let two sides put the same players in
  opposite slots, and a client that did not yet know about the host added no car
  at all and left a level AI car (a cop) in the remote player's slot.
- **Ping** — PONG echoes the tick from PING, so the host holds a round trip per
  peer and publishes it; that is the number in the pause menu list.
- **The frontend's idle demo is suppressed** while a session or lobby exists
  (`JER_EVENT_FRONTEND_IDLE`). A host waiting for players is idle by definition,
  and the demo's level load blocks the main thread, which used to drop everyone
  who was joining. `mp_host.bat` therefore sets `MP_AUTOSTART=host`: with the demo
  gone, nothing starts a match unless the host says so.
- **Addon network bridge** (`jer_net.h`) — any module can send named
  reliable / latest-wins channels over the session (`jer_net_send` /
  `JER_EVENT_NET_RECV`).

## Files

| File | Role |
|---|---|
| `mp.c` | the module's engine-facing entry point and hooks |
| `mp_net.c` | sockets: TCP session + UDP discovery, framing, per-peer ping |
| `mp_session.c` | handshake, start, owner-authoritative car sync, roster, chat, dispatch |
| `mp_players.c` | the player registry and the car accessors built on it |
| `mp_config.c` | config, module identity, build/manifest hashes |
| `mp_map.c` | the multiplayer-map blips (`JER_EVENT_DRAW_MAP`) |
| `mp_bridge.c` | the `jer_net.h` addon bridge |
| `mp_ui.c` | the frontend menus (registered via `jer_frontend.h`) |
| `mp_proto.h` | the wire protocol (framed, little-endian) |
| `tools/` | launchers + harnesses, documented in `tools/README.md` |
| `docs/ARCHITECTURE.md` | how it all works: the model, protocol, lifecycle, traps, roadmap |

## In game

While the pause menu is open, the players are listed down the left of the screen:
the host first and in cyan, then each player's name, index, the vehicle they are
in (`-1` = on foot) and their ping. The order is the roster's, so it reads the way
the match was built. `MP_PAUSE=1` holds the pause menu open for testing and
`MP_DEBUG` logs each row.

The engine's own pause menu keeps working, and the module adds a `Multiplayer`
page to it with three rows:

| Row | What it does |
| --- | --- |
| `Change car` | pick a city and one of its cars, then "Respawn as this car": your vehicle is replaced in place, and every peer re-models its copy of you. The city row offers the whole session when carhacks is installed (a session can mix cities' car data), and the session's own city alone when it is not |
| `My colour` | your character's suit colour, on or off, plus RGB. Off (the default) keeps the game's own colours |
| `Write diagnostics now` | writes `mp_diag.txt` next to the game |

`Restart` is handled specially in a match: instead of rebuilding the level under
everyone else, it puts YOU back at the world start, in your car, repaired and with
no wanted level, and the session carries on.

## Config (`JERICHO/CONFIG/mp.ini`)

| Key | Default | Meaning |
|---|---|---|
| `port` | `1318` | TCP session port. Discovery always uses a fixed UDP 1318 and advertises the session port in the beacon, so changing this does not hide your game |
| `player_name` | OS username | your name on the network |
| `host_name` | `<name>'s game` | the advertised server name |
| `beacon_ms` | `1000` | discovery beacon interval |
| `mod_check` | `0` | host lobby mod policy: 0 off, 1 by id, 2 by id+version |
| `strict_version` | `0` | host lobby setting: 1 also requires an identical build hash (off by default, because that hash tracks `git describe`) |

## Command line

`-help` (also `-h`, `--help`, `-?`) prints the full argument list **to the
terminal and the log**, then exits -- no window, no pop-up. It is handled before
any engine init, so it is instant.

Unknown arguments no longer dump the list or open a modal dialog: they raise a
toast in the frontend (`jer_error`, gentle red, left side, ~5 s) and the game
carries on.

    JERICHO_dev.exe -help
    JERICHO_dev.exe -host 1318
    JERICHO_dev.exe -join 192.168.1.20:1318

## If nobody can see your game

Two things have to be true, and the module only controls one of them.

1. **Windows Firewall.** Inbound **UDP 1318** (discovery) and **TCP 1318** (the
   session) must be allowed on the host. The module cannot create that rule,
   so if you never see another machine's game, check the firewall first.
2. **Discovery actually running.** If the discovery socket cannot be opened
   or its port is taken, the module now says so instead of failing quietly:

       [mp] discovery unavailable: the port is already taken by another program (UDP/1318)
       [error] LAN discovery is off: the port is already taken by another program (UDP/1318)

   A *second copy of this game* sharing the port is fine (both can still see
   each other); a different program holding it is not.

`-join <ip>[:port]` still works with discovery off, so a host can always be
reached by address.

## Testing

`tools/README.md` documents the whole launcher/harness set in `tools/` and says
which one to reach for; the short version is `tools/mp_pair.bat` (two real
instances on one PC), `mp_host.bat` / `mp_join.bat` (two machines), and
`mp_mock_host.bat` / `mp_dedi.bat` (no second engine).

`tools/mp_localpair.py` runs **two real instances on one PC** — one hosting, one
joining — and prints both sides' logs. Each instance gets its own working
directory (built from junctions, so nothing is copied) which is what keeps the
two `JERICHO.log` files and the two `mp.ini` files apart. It launches the
executables directly and kills exactly the PIDs it started.

    python tools/mp_localpair.py                # host + join, report, clean up
    python tools/mp_localpair.py --keep         # leave the run dirs to poke at
    python tools/mp_localpair.py --clean        # remove them again

Use it for anything that needs two real engines: a car that never appears, a mode
that launches wrong on one side, a connection that drops. `--clean` (and the
automatic cleanup) unlinks the junctions before deleting anything, so it can
never follow one into the real game tree.

### Watching a run, not just reading it afterwards

`--vramview` opens the VRAM viewer and the console on every seat, and `--shots DIR`
photographs each seat's windows on every car change:

    python tools/mp_localpair.py --vramview --shots tmp/shots --bot off

Every capture is three images - the game, the VRAM viewer, the console - and
`DIR/index.txt` pairs them with the log line that caused them **and** the VRAM line in
that same log at that moment, so "the car went invisible" can be read next to "pages 2
used / 28 free, 0 row(s) reclaimed". `tools/mpshots.py` does the same on its own (`grab`
photographs a running game once, `watch` is the log-driven kind), which is what makes it
usable on a hand-played session as well as a harness run.

### Test levers for the on-foot and mid-match paths

Set these on the harness (they are read from the environment, so every seat gets
them):

| Lever | What it exercises |
| --- | --- |
| `MP_TEST_ONFOOT=<secs>` | get out of the car that many seconds in, so the on-foot path runs at all |
| `MP_BOT` | drives the cars AND the on-foot Tanner (see `mp_bot.c`), so a run has motion without a human. `mp_localpair.py` defaults it to `chase` (the host flees, every joiner chases); `pursuit` hunts mutually; `catmouse` is the same pair DRIVEN BY THE PATHFINDER - the mouse runs to a place it chooses (far from the cat, preferring the road, reachable) and the cat plans to where the mouse is; `off` leaves a real player's car alone |
| `MP_TEST_PAUSECAR=<secs>[,<city>[,<model>]][;...]` | runs the pause menu's `Change car` apply path, so a mid-match vehicle change (including a cross-city one, with carhacks) is reproducible headlessly. A `;`-separated list (`30,3,1;45,1,2`) makes one change per entry, in order, each at its own time, counted in seconds from when the session starts running - enough to switch until the spare slots would run out |
| `MP_TEST_RESTART=<secs>` | fires the engine's own pause-menu answer, so a pass means the multiplayer soft restart is wired end to end |

The driving bots work from what the engine's own scenery test can see and have **no route
planner**: a straight line at the peer, a fan of headings probed with `CellEmpty`, and a
dodge they commit to for about a second (`MPBOT_DODGE_FRAMES` - re-deciding every half
second read as indecision). They never select reverse except in one case: a car wedged with
a wall DEAD AHEAD, where turning under power cannot move a car that cannot move. That case
backs off the wall briefly (`MPBOT_BACK_FRAMES`) and then turns out the OTHER way, so it
does not drive straight back into the wall it just left. Everywhere else a wedge is cleared
by turning round under power, with a short handbrake pulse while the wheels are rolling
(`MpBotTurnPad`), and a wedge is noticed after ~0.4 s of being stopped rather than 0.7 s.
The old unconditional "backing out" recovery put the car back where the wedge started - and
a stopped car given steer-only cannot turn at all - which is what made a pair shuffle on
the spot instead of chasing across the map.

The fleeing side scans rather than running blind. 180 degrees from the pursuer is one fixed
heading, so when a wall is there the fleer has nothing else in mind and circles the corner
it just ran into. `MpBotFleeWant` sweeps a fan of headings (up to +/- 135 degrees in 22.5
degree steps) and scores each by how far its CORRIDOR stays open - a heading still clear at
4800 units is a road, one that clears 1200 and then stops is a driveway into a wall - so the
flee turns down a street instead of into the wall behind it. The corridor test probes the
lines either side of the heading as well, because probing the centre line alone let it pick
headings that cleared walls by centimetres (the "it grazes along walls" symptom). Measured:
334 of 398 flee decisions in one 45 s pair were roads, 240 of them at full depth. A wide-open
street a few degrees off "straight back" therefore beats a narrow gap dead astern.

Following at minimum distance is not a reason to do anything dramatic. Inside
`MPBOT_FOLLOW_IN` (900) the chaser stops and waits with its nose on the target, and only
takes the chase up again once the gap has reached `MPBOT_FOLLOW_OUT` (2200). Two thresholds
rather than a distance derivative, so it cannot flicker between chasing and waiting; the
panic-turn at close range is gone. The same run: 18 recoveries, down from 31 over 45 s.

None of this is a pathfinder. It is a greedy, reactive probe with no lookahead, so it still
cannot plan around a building - 10 of those 18 recoveries were wedges. A real fix wants a
lookahead or a coarse route, and that is a separate unit, not a tweak.

### The AI library (`ai/`), and the `catmouse` behaviour set

That "separate unit" now exists. `JERICHO/MODS/mp/ai/` is a small, self-contained
collection of pieces that need no engine to be tested:

- `aimap` - the world model. A fixed 33x33 window of samples, re-probed around whoever
  asks, filled by one `CellEmpty` call per sample. That is why **fences and barrels are not
  walls**: `CellEmpty` skips `MODEL_FLAG_SMASHABLE` and chairs by design (objcoll.c:49), so
  the grid sees through exactly what a player drives through, without a special case. The
  road network is recorded as a PREFERENCE (cost, never a wall), and wall clearance is
  measured so a route hugs the middle of a street instead of grazing its edge;
- `aistar` - A* over that grid: fixed node pool, bounded expansions (the grid itself), no
  allocation, deterministic tie-breaking, an 8-connected neighbourhood with a corner-cut
  rule, and the path is SMOOTHED into straight runs by line of sight so a driver steers at
  turning points. Two cases are handled rather than refused: the start sample being blocked
  (which is what happens whenever a car is parked against a wall, because our probe radius
  is a car's width) and an unreachable goal (the path comes back INCOMPLETE with the best
  partial route, which is what a car that needs to be somewhere else actually wants);
- `ailocal` - the different KIND of search: one flood of the window answering "where is the
  nearest place worth being". The road rung is breadth-first, so it is the nearest road
  along ground the car can DRIVE - a road across a wall is not a way out, and a
  line-of-sight search would pick one. The fallback rung is the most open reachable ground,
  and if even that fails the caller keeps its own behaviour;
- `JERICHO/test/test_ai_path.c` (in the engine's excluded test directory, so it costs the
  exe nothing) drives the real cost model and the real A* against hand-built grids, in both
  C and C++: **125 checks, 0 failed**. It found two real bugs in the library and one bad
  budget while being written - a truncating sample index that pulled a point a whole step
  outside the window back onto its edge, an `onRoad` count that missed the first waypoint,
  and a 900-node expansion ceiling that a winding route exhausted, so the pathfinder
  reported "cannot reach" for a goal that was merely round the corner.

`MP_BOT=catmouse` is the behaviour set built on it. Measured over a 55 s city pair: the two
cars were a **median 12,390 world units apart** (about three map cells, up to 17,858), the
plans came back as 2-3 waypoints after 146-211 node expansions, and the mouse's chosen goal
was on the ROAD network - the road preference working in a real level. That is cat and
mouse, where `chase` is the pair closing on each other.

The `catmouse` recovery is one policy in one place: DRIVE when moving; TURN when something
the probes can see is in front; and PUSH - throttle only, no reversing - when the car is
stopped, in contact, and nothing is visible ahead. That last case is a fence or a barrel the
engine deliberately hides from every probe while the physics still stops the car on it, and
backing away from something the engine says is not there is exactly how a car ends up stuck
on it for the rest of a match. A real wall lands in PUSH too, so PUSH is bounded: after
~900 ms of shoving the car backs out and turns the OTHER way.

The other sets - `chase`, `fight`, `pursuit`, `random` - are deliberately untouched, so the
collision tests and the `mp_tries` verdicts do not move; a `--bot chase` pair run confirms
it (zero AI activity in the log, its own 137 flee-scan lines still there).

`MP_BOT_DRAW=1` puts the AI's thinking on the HUD while it runs - the goal and whether it is
a road, the route as its waypoint chain, where the car is aiming, the gap to the other car,
how many nodes the plan expanded and the pad - so the pathing can be watched happening
instead of reconstructed from the log afterwards. (The SDK has no world-space line
primitive, so this is a readout rather than lines drawn on the ground; drawing the route in
the world wants a new engine hook.)

One rule in the mouse's goal choice is worth knowing, because it is what fixed the most
visible complaint: **a mouse that is not on a road heads for the nearest road it can
reach**, and only starts running for distance once it is back on the network. Choosing the
furthest open point instead scored well between houses - far from the cat, and "open" to the
probe - and the car then wedged in a gap it could not leave.

A run wants `lost=0` and `dumps=0`.

`tools/mp_test.py` is a headless protocol harness (the game writes its log
relative to the CWD, so two live instances cannot share a folder). It drives
the real module against a scriptable peer:

```
# game as host (MP_AUTOSTART=host) <-> python mock client
python tools/mp_test.py client 127.0.0.1 --port 1318 --build 0x<buildhash> --channel demo
# python mock host <-> game as client (MP_AUTOSTART=join:127.0.0.1), start + lockstep + resync
python tools/mp_test.py host --port 1318 --city 1 --start --lockstep
# watch the host's discovery beacons
python tools/mp_test.py beacon --port 1318
```

`MP_AUTOSTART=host[:PORT] | join[:IP[:PORT]]` brings a session up at boot
without the frontend, and `MP_DEBUG=1` logs the bridge/session detail.

> Note: while testing, `JERICHO/CONFIG/modlist.ini` has every other module
> disabled so they cannot interfere; re-enable them afterwards.
