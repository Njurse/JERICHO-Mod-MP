# How mp works, and what is next

This is the deep version: the model, the protocol, the lifecycle, the engine
touchpoints, the traps that keep costing days, and a prioritised roadmap. It
assumes you have read `../README.md` (what the mod is, how to configure it) and
`../tools/README.md` (how to run it).

**Status.** A two-player match is verified end to end on one machine: a host and a
joiner load the same level, each gets its own car, and each machine mirrors the
other's car to within single-digit world units while both drive
(`mp_localpair.py`, PASS). Chat (open/type/send/receive) works in a live match, and
a player's suit colour now goes through the canonical JERICHO colour type. What is
still open is listed under "Roadmap" and "Unverified" at the bottom.

---

## 1. The model

Four decisions explain almost everything else.

**One player per machine.** `NumPlayers` stays at 1 locally. The stock renderer
decides between one view and a two-view split by `NumPlayers`, so a second local
player would mean a split screen, and the whole point is that everyone gets their
own screen. Remote players are therefore not "player 2" to the engine — they are
extra cars with **negative pad ids** (`padId = -slot`), which is the same trick the
engine uses for cars nobody is driving.

**Every car is engine-simulated, everywhere.** The first design replicated
*positions*: each machine told the others where its car was and everyone else's
car was a puppet whose transform was written each frame. That cannot work, and the
reasons are worth keeping:

- the puppet's `hd.speed` stayed at 0, and the car-vs-car collision loop skips
  pairs where neither car is moving — so two players could not collide;
- more subtly, the collision response *was* computed and then erased by the next
  packet, so a car you hit could never be pushed;
- nothing else came along either: wheels, damage, engine effects, the collision
  box's orientation.

So instead every machine simulates every car, driven by the owner's **input**.
That is what makes a remote car a real car, and it is why "car-to-car
interaction" is a consequence of the transport rather than a separate feature.

**Each machine owns its own car; the host owns the truth.** A player's own car is
driven locally from its own pad, immediately, with no round trip. The host does
not overrule it except to correct gross drift (§7).

**Input replication, not lockstep.** Lockstep — waiting for everyone's input before
simulating — was tried first and abandoned: the barrier blocked the main thread and
took the frame rate with it. What replaced it never waits. A car whose input has
not arrived repeats its last input, so a slow link costs smoothness, never a
stalled frame.

---

## 2. Lifecycle

### Boot

`JER_EVENT_BOOT` → `MpNetStart()`, config load, frontend menus registered. Nothing
listens, nothing connects, **nothing advertises** — a host that is not ready to be
joined must not appear in anybody's browser.

### Bringing a session up

Three ways in, all ending in the same place:

| How | Path |
| --- | --- |
| the frontend menus | `mp.root` → LAN → Host / Join |
| `-host [port]` / `-join <ip>[:port]` | `JER_EVENT_CMDLINE` (`MpOnCmdLine`) |
| `MP_AUTOSTART=host[:port]` / `join:IP:PORT` | `MpAutostartFromEnv` |

The command-line and env paths mark the session **unattended** (`gMp.autoSession`),
which means: nobody is going to press anything, so start and launch by yourself.
The frontend paths leave that off, because a human is right there.

`-host` also arms the autostart (`gAutoHostStart`, target 2 players): the host
launches the match as soon as somebody is in. This exists because the frontend's
attract demo used to do it *by accident* (§9).

### Hosting

`MpBeginHost()` — **a no-op if we are already hosting**, which matters more than it
looks (§10, trap 1). Otherwise: reset session, reset registry, claim player id 0,
listen on the session port.

### Joining

Client connects (non-blocking; the connect is finished over subsequent polls, so a
dead address cannot stall a frame) and sends `HELLO`.

### The handshake

```
client ── HELLO {protoVersion, sdkVersion, gameBuild, name, modManifest[]} ──▶ host
host: protocol/SDK version check, mod-match policy, capacity
host ── ROSTER ──▶ client        (who is in the match, BEFORE the welcome)
host ── WELCOME {playerId, maxPlayers, running, subGame, city, time, weather,
                 seed, modsEnforced, modsMatched} ──▶ client
   or ── REJECT {reason} ──▶ client
```

**The roster goes out before the WELCOME, and that order is load-bearing.** A live
joiner launches the moment it is welcomed; if it does not yet know who else is in
the match it spawns no car for them, and the level's own AI car — a cop — is left
sitting in that player's slot on that machine. That is the "placeholder cop car";
the ordering is the fix.

A rejection is delivered before the socket closes, and the peer is half-closed
(`shutdown(SD_SEND)`) rather than dropped, so the reason survives. Losing it was a
real bug: a rejected client used to see "Connection to the server lost!" and
nothing else.

Client marks itself READY, adopts the host's city/time/weather/subGame, and — if
unattended — asks to launch.

### Starting a match

Host `MpStartMatch()`: clamp unset lobby values, set `running`, **start
advertising**, build the seed, broadcast `START`, launch locally.

`running` is the flag the whole system turns on: it gates the input loop, the
snapshot resync, the roster refresh, and what the beacon reports.

### Level launch and spawn

```
host:  MpStartMatch ─▶ roster ─▶ START ─▶ SetState(STATE_GAMESTART)
client: START ─▶ adopt session ─▶ SetState(STATE_GAMESTART)
both:  engine creates player cars ─▶ JER_EVENT_NET_SPAWN ─▶ GAME_START
```

`JER_EVENT_NET_SPAWN` is where a network module adds the remote cars: the engine
asks how many extra player cars to create, and the module answers by filling
`PlayerStartInfo[slot]` and raising `numPlayersToCreate`. The extra cars get
negative pad ids and are appended to `active_car_list` by the normal physics path,
which is what makes them real.

At `GAME_START` every car exists.

**Every car keeps the position the ENGINE gave it.** The host used to broadcast a
`SPAWN` "meeting point" (its own car) and every machine teleported all the cars
onto it, spaced `MP_SPAWN_SLOT_DIST` in world X and all at the host's car Y. That
is what put the cars IN THE AIR: a single Y taken from the host, applied at every
other car's x/z, leaves each car above or below the ground actually under it, and
the engine then pulls it down — the logs showed the local car starting at y=75 and
falling to 26 within a couple of frames. It was believed the two machines spawned
on opposite sides of the map; they do not. With the line-up off, BOTH machines
place both cars at exactly the same x/z/y (the engine's own spawn is
deterministic), so the map's baked start is already agreed on. The line-up and its
`JPSW` meeting-point message have since been REMOVED outright, and a joining
client instead gathers itself beside the host (`MpHandleCarState`), resolving the
height under its OWN x/z. Do not reintroduce a single-Y teleport: to move a car,
move it in x/z and let the engine place its height, or offset it ALONG the road.

### The steady state

Per simulation frame (`JER_EVENT_PRE_SIM`):

1. `MpSendInput(MpLocalPad())` — our pad goes out. A client sends one row; the host
   merges every row it has seen and broadcasts the whole set.
2. `MpSendOwnCarState` — the one car we own goes out (owner-authoritative; there is
   no separate snapshot cadence).
3. Every 120 frames: the host refreshes the roster (names, vehicles, ping).
4. `MpNetPoll(0)` — receive everything available and act on it.

Per car per frame (`JER_EVENT_NET_INPUT`): a remote car is handed
`MpInputForPlayer(id)` instead of a pad of 0.

### Leaving

- A client disconnects → its car is removed (`controlType = CONTROL_TYPE_NONE`)
  and the others are told it left.
- The host leaves → everyone is told (`LEAVE`), the match ends and the clients are
  returned to the frontend **through the engine's own `EndGame(GAMEMODE_QUIT)`**,
  which tears the level down and stops the music. A bare `SetState(STATE_INITFRONTEND)`
  left sounds playing.

---

## 3. Wire protocol

Framed and little-endian (`mp_proto.h`); the framing is an 8-byte envelope of
length, 4-char tag, version and flags. Reliable frames are queued per peer and
flushed as the socket accepts them (§10, trap 2).

| Tag | Dir | Purpose |
| --- | --- | --- |
| `JPHL` | C→H | HELLO: identity + mod manifest |
| `JPWL` | H→C | WELCOME: player id + lobby + live state |
| `JPRJ` | H→C | REJECT: why, in text |
| `JPRS` | H→all | ROSTER: who is in the match, ascending id, host first |
| `JPST` | H→all | START: begin the level launch |
| `JPIN` | C→H, H→all | INPUT: this frame's pad set |
| `JPCS` | each→H, H→all | CARSTATE: the sender's own car, adopted verbatim by everyone else |
| `JPPN` / `JPPO` | both | PING / PONG (the PONG echoes the tick, so the host can compute RTT) |
| `JPCH` | both | addon channel payload (the `jer_net.h` bridge) |
| `JPLV` | both | LEAVE |
| `JPKK` | host -> client | KICK — the host removes a player. The kicked client prints "kicked by the host", then takes the same clean-leave path a deliberate quit uses (`MpLeaveSession`), so the roster row and the car leave every machine identically. Host-only (`MpKickPlayer`); a non-zero player id only. |
| `JPCX` | both | chat line (T to open, Enter to send, Esc to cancel; drawn in the status console — the speaker's name in their own colour, or cream when custom colour is off, and the message in a flat 240,240,240) |
| `JPCC` | client -> host | the car this player picked in the car select. Sent when the pick becomes known (at launch), because the `JPHL` hello goes out at CONNECT time, long before the player has chosen. The host records it, republishes `JPRS`, and - in a match already running - builds the vehicle then rather than at hello. No car is built for a player until this arrives, so a joiner's vehicle never appears before they have picked it. |

`JPSS` was reserved here for a standalone session/lobby broadcast. It is now **retired**:
nothing ever sent or handled it, the launch config rides inside `JPST`, and the tag is gone
from the code. Do not reuse the spelling.

---

## 4. The roster and the spawn contract

The single most important rule: **car slots are assigned walking ascending player
id.** Not registry-row order, not arrival order.

Why: the engine creates the player cars in the order `PlayerStartInfo` was filled,
and the module's registry hands out rows first-free. Walking *rows* therefore lets
two machines place the same two players in opposite slots — and each then drives
the other's car, or reads a level AI car as a player. Player ids are the one
ordering both sides already agree on.

`carId` is deliberately **local**: the host's slot numbering means nothing on
another machine, so a client does not adopt it from the roster. It assigns its own,
in the canonical order.

The roster also carries each player's name, host flag, vehicle and ping, which is
what the pause-menu list reads.

**And whether the HOST has a car for that player** (`MP_ROSTER_FLAG_CAR_READY`,
set from the same `MpPlayerCarReady` gate the spawn uses). That flag is how a THIRD
machine finds out that a late joiner's pick has landed — the pick message goes to
the host alone — and it is what a client needs to build that player's car:
without it a third machine added the player row but no car, so every carstate
entry for them was dropped and the player was invisible on it while still
colliding on everyone else's (the host sees all clients, a client sees only
itself and the host). The host republishes the roster after it spawns, so the flag
arrives one frame behind the car. Additive flag, no layout change: an older peer
ignores it and behaves exactly as before.

### The spawn contract: the level's own start, and never a y

The engine places a player car from a per-slot start record, built at level init:
slot 0 from the mission (or `levelstartpos`), slot 1 beside it at +600 in x
(`main.c:3401-3405`) -- and **only x/z are set**. `position.vy` stays 0 and
placement resolves the height later (`main.c:3384`, `main.c:641-642`).

**And the pass must not run before those records exist.** `MpSpawnLateJoiners`
copies player 0's record, and `PlayerStartInfo[0]` is NULL until the engine builds
it during a level load (`main.c:3485`). The level-init call site is safe -- the
event it hangs off fires *after* that (`main.c:3534`) -- but the frame-hook call is
not: it can arrive while a level is still coming up (a client joining a session and
walking the city screen does exactly this), and it dereferenced the NULL record.
That is the reported "access violation changing cities as a client joining the
game": `MpSpawnLateJoiners+0x188` in the JERICHO dump, an `0xC0000005`. The pass
now defers until the record exists, re-arming its own request (the caller clears it
before the call, so a deferred pass is otherwise lost), and it refuses a slot
outside the engine's `player[]` table (`MAX_PLAYERS`) instead of writing past it.

A LIVE join has no engine record for its slot -- the level was loaded for the
players who were there -- so `MpSpawnLateJoiners` builds one in that same shape:
the start point, one 600-unit lane per player id, the level's own heading, no y.
Two rules follow, both learned the hard way:

- **Do not copy `PlayerStartInfo[0]`.** Its position is the start point of the
  player who is ALREADY in the match, so a late joiner used to appear on the host's
  start point instead of its own.
- **Never carry a y across cars.** The old code forced
  `hd.where.t[1] = car_data[0].hd.where.t[1]` -- the local car's LIVE y -- onto the
  joiner. A y from one x/z applied at another is the "late joiner spawns above the
  host" report, and the engine then has to pull the car down. Move a car in x/z
  and let placement find the ground under it.

### A hop must be streamed before its ground is read

The gather writes the host's x/z onto the client's car and then asks the engine for
the ground there (`MapHeight`). That ask only means anything if the place is loaded,
and **a place you HOP into is not loaded**: the streamer follows where you drive, not
where you are put, so a region you are teleported into is never unpacked into the
engine's 2x2 barrel. `MapHeight` reads that barrel (`sdGetCell`), returns 0 when the
cell has no plane, and a car placed on that 0 is under the world -- the client's "no
cells, it fell into the void", and then the crash handler's "Unhandled exception!"
dialog with the OS access-violation text. Two things about the fix are worth keeping:

- **`resident` answers "is this a hop", never "is there ground here".**
  `jer_map_region_resident` means *unpacked*, not *in the barrel*: measured on this
  very path, the gather's destination reported `region 135 resident=1 hasData=1` and
  still gave `MapHeight 0`. So do not reason "resident, therefore the ground is fine";
  equally, do not stream on every gather just because resident is not proof.
- **`jer_map_spool_to` is the right call for a hop, and it is not free.** It is the
  SDK's "stream there" call, and it is what the arena and antfarm use for their own
  teleports. But when the region IS already resident it still runs
  `CheckLoadAreaData` + `StartSpooling` + `UpdateSpool` -- a synchronous spool pass --
  and calling it unconditionally on every gather stalled both harness rigs (measured).
  So: point `MainPlayer.spoolXZ` at the destination (it must outlive the call),
  stream only when the region is genuinely not in, and leave `spoolXZ` on the car
  afterwards so the streamer keeps following the car instead of the spot it left.

When the destination cannot be streamed at all (no data there -- off the map), the
gather places nothing and leaves the car on the level's own start, which is real,
streamed ground by construction, and retries on later carstates.

---

## 5. Input replication

- **Digital, not analogue.** The `NET_INPUT` hook originally hardcoded
  `t1 = 0, t2 = 1`. Those are `ProcessCarPad(cp, pad, PadSteer, use_analogue)`'s
  last two arguments, and `t2 = 1` means "read the analogue axis" — paired with
  `t1 = 0`, i.e. an analogue stick held at dead centre. Every remote car ignored the
  pad its owner was sending and could neither accelerate nor steer. A replicated
  pad is digital, so it passes `t2 = 0`. Analogue steering is not replicated yet.
- **The host relays.** A client sends its own row up; the host merges the rows it
  has seen and broadcasts the set, with its own row included. Nobody waits: a car
  whose input is late repeats the last one.
- **Nothing blocks.** The abandoned lockstep barrier is the reason this is stated
  twice.
- **Now only a fallback.** Since car sync became owner-authoritative (section 7), a
  remote car is placed by its owner's state, so the replicated pad no longer drives
  it: `MpOnNetInput` sends pad 0 and hands off, and only falls back to the
  replicated pad after `MP_INPUT_FALLBACK_FRAMES` of silence from the owner, so a
  snapshot gap coasts instead of freezing. The engine and the adoption must not
  fight over the same car.

---

## 6. Frontend integration

Menus are **real frontend screens**, not an overlay — `jer_frontend.h` registers
them into the engine's own screen table, so they get the stock styling, cursor and
input handling.

The engine fires `JER_EVENT_MP_FRONTEND` when the player is about to enter a
multiplayer menu point, and a module may *claim* it. mp claims the start-game
press and runs its own launch instead of the stock one. Getting that wrong is
expensive: the stock path launches with whatever `GameType` the frontend had, and
`GameType` defaults to 0, which is `GAME_MISSION` — the mission ladder, i.e.
**Undercover mission 1**. That is exactly what a joining player used to get.

Whether a client may start is a question about the **join state**
(`MpClientSessionLive`), never about `running`: joining a live game sets `running`
the moment the WELCOME lands, so gating on it refused the press and let the stock
path through.

---

## 7. Car sync: owner-authoritative

Each machine sends the ONE car it owns, every frame (`MpSendOwnCarState`), and every
other machine **adopts** that state in full (`MpHandleCarState`): position, heading,
the orientation quaternion and both velocities, written verbatim. There is no easing
and no tolerance -- the owner is the truth for its own car. The host relays a
client's car to the other clients (`MpHostRelay`), because each machine sends only
its own car.

This replaced a model in which every machine simulated every car from input that
arrived a round trip late, and a coarse resync (30-frame cadence, correct only past
600 units, eased a quarter per correction) tried to pull them back. Two
non-deterministic simulations cannot be reconciled that way -- the follower's view of
a remote car wandered +/-100-900 units while driving, and no threshold fixes that.
With adoption it is 0-2 units.

**Contacts are handed off, not simulated twice.** A machine can only move the ONE
car it owns, so when your car touches a peer's you push yours and report it
(`MP_HIT`); the peer's machine pushes theirs. Both cars move and each stays its
owner's truth. (The older "a car you drive into is not pushed" trade-off no longer
applies — see `MpHitFrame` / `MpHandleHit` in `mp_session.c`.)

---

## 8. The pause menu, and why a session is never paused

**A match's pause menu keeps the addons.** The engine picks the in-game pause layout
with `if (NumPlayers == 1 && gMultiplayerLevels == 0)` (pause.c:1176) — and
`gMultiplayerLevels` is set from `MissionHeader->region`, so a match on the `-mp`
multiplayer map takes the OTHER branch. That branch used to hand back a static
`MultiplayerPauseHeader` that never went through `JerPauseRootOr`, so the whole
"JERICHO Addons" tree (every module's pause page, including this mod's own "My
colour") was missing *exactly* in multiplayer and present in single player. Both
branches now splice it. A two-player split-screen match had the same hole.

In single player the START press opens the engine pause, which freezes the
simulation. In a network session that freezes only THIS machine while everyone else
keeps driving, so the two states disagree the moment play resumes. mp therefore
claims the press in a live match (the `JER_EVENT_PAUSE_MENU` hook, returning
`JER_RESULT_STOP` -- the same mechanism the sandbox overlay uses), so the engine
pause never opens and `pauseflag` is never set: the world keeps running.

The press toggles mp's own non-freezing player list instead: a small status panel
down the left, one line per player.

**It is drawn at HALF SIZE, and that is what makes one line per player possible.**
At the default 0.275 only about 33 characters fit across the 320-px screen, which is
why a player used to take two rows — a name row and an indented link row. At
`0.138f` (plus a little tracking, via `PrintStringHiresScaledSpaced`) a whole row
fits: `PLAYER`, `CAR`, `PING`, `RX`, `TX`, `LOSS` at fixed column positions
(6/92/130/164/206/250), so the columns never shift as rows come and go. A player
with no car reads `on foot` rather than `car -1`, and a figure that has not been
measured yet reads `--`.

**The panel has its OWN palette, deliberately not the chat's.** Chat is a
conversation — a speaker's name in their colour, the message in a flat near-white.
This is a readout, so it uses an accent for its title, a colour per ROLE (you /
host / other), dim greys for the labels and chrome, and amber for a link that is
dropping frames. Every colour goes through `MpInk`, which halves it: the HQ font is
drawn with the PSX texture filter on (×2), the same trap `jer_console`'s
`jerConsoleInk` handles. Without that halving nothing here can be dim — the panel's
old 200/170/150/140 all clipped to a single flat white, and only the host's
saturated cyan survived.

The build identity sits under the title in plain words — `all players must match:
build xxxx  addons xxxx` — because those are the same numbers the startup log
prints and `strict_version` compares, and a mismatch is the most common cause of
"we cannot see each other".

`MP_PAUSE=1` logs the rows for testing (it deliberately does not force the engine's
`pauseflag` — see the comment in the source); `MP_PANEL=1` shows the panel itself so
its layout and colours can be captured headlessly with no pad, which is how they get
verified without eyes on the screen.

---

## 9. Engine touchpoints

Hooks mp registers: `BOOT`, `FRAME`, `PRE_SIM`, `DRAW_OVERLAY`, `DRAW_MAP`,
`GAME_START`, `FRONTEND`, `FRONTEND_IDLE`, `LEVEL_LAUNCH`, `MP_FRONTEND`,
`NET_INPUT`, `NET_RECV`, `NET_SPAWN`, `CMDLINE`, `SHUTDOWN`.

Engine hooks this work *added*, which other modules can use too:

- **`JER_EVENT_NET_INPUT` / `NET_CAR_STATE` / `NET_PLAYERS` / `NET_RECV` /
  `NET_SPAWN`** — the synchronisation surface (substitute a car's input, capture or
  apply a transform, enumerate local player slots, receive a channel payload, add
  remote player cars). `NET_CAR_STATE` and `NET_PLAYERS` are declared for
  completeness but **no module registers them today** — mp drives its sync from
  `NET_INPUT`, `NET_RECV` and `NET_SPAWN`. Treat them as reserved, not live.
- **`JER_EVENT_LEVEL_LAUNCH`** — gained in/out `timeOfDay`/`weather` so a session's
  host can dictate the match conditions.
- **`JER_EVENT_DRAW_MAP`** — fires on **three** surfaces, and `flags` is how a module
  tells them apart: the multiplayer map (`0x20|0x2`), the overhead **mini-map on a
  single-player level** (`0x1|0x2`) and the full-screen map (`0xE`, `fullscreen = 1`).
  **A module must not transform its own world positions.** Give `DrawPlayerDot` the
  *world* position and the hook's own `flags`, the way the stock loops do: `0x20`
  makes it call `WorldToMultiplayerMap` and add the map offsets, `0x1` makes it call
  `WorldToOverheadMapPositions` and clip to the overhead rect. In particular
  **`WorldToMultiplayerMap` returns a constant `(32,32)` whenever
  `MissionHeader->region == 0`** — it only has the maths for multiplayer regions — so
  pre-transforming with it plots *every* player at the same wrong point. That was
  mp's bug: remote players were invisible on the single-player mini-map until the
  module stopped transforming and started passing `m->flags`. (Related: the hook's
  `suppressStockBlip` is only read back on the multiplayer surface; the single-player
  and full-screen sites pass it uninitialised and ignore it.)
- **World→screen for the nametag (`MpProjectWorldToScreen`, mp_ui.c)** — the overlay
  projects with a **yaw-only** transform (the camera pitch is small enough not to
  matter for a label): subtract `camera_position`, rotate by `-camera_angle.vy`,
  `f = 520/rz`, centre at (160,120), and reject behind-camera / too-far / off-screen.
  Two frame gotchas recorded so the next module author does not re-derive them: a
  **car's `hd.where.t[1]` is UP-positive** while a **pedestrian's `position.vy` is
  DOWN-positive** (`ground - 130`), converted with the same negation the engine uses
  in `ChangeCarPlayerToPed` (`hd.where.t[1] = -ped.vy`). Tag colour goes through the
  single seam **`MpNameTagColour`** (future team/gamemode → the player's own colour →
  white) and the text is **distance-scaled** `clamp(700/depth, 0.138, 0.275)` and
  centred via `StringWidth`.
- **`JER_EVENT_CMDLINE`** — a module picks up its own shortcuts after the engine
  parses its args. Fixing this is what stopped unknown arguments popping a modal
  message box, which used to block the main thread *before* the frontend and made
  `-host`/`-join` look broken.
- **`JER_EVENT_FRONTEND_IDLE`** — the attract demo can be vetoed.

**The attract demo.** The frontend boots it after ~30 s without input. A host
sitting in a lobby waiting for players is idle by definition, so the demo launched
a level nobody asked for — and that load blocks the main thread for its whole
duration, so every player joining went quiet, hit the idle timeout and dropped,
leaving the host alone in a demo it never asked for. mp suppresses it for as long
as a session or lobby exists.

**`jer_error`** — short red notices, left side, ~5 s, in the frontend and in game.
The data/API lives in the JERICHO core; the *drawing* is engine-side (`main.c`,
`FEmain.c`) because the core is C++ and cannot include `pres.h`.

---

## 10. Traps

These have each cost real time. They are not hypothetical.

1. **`MpBeginHost` resets the session.** Set lobby values (city, time, weather,
   gamemode) *after* it, never before — anything set first is wiped, and the match
   starts in the wrong place. (It took a host launching Chicago while `-level` said
   Havana to make this obvious.) Re-entering the host menu must also be a no-op:
   it used to tear the session down, drop every peer and send `LEAVE`, so a player
   was accepted and then told "the host ended the match" a moment later.
2. **Never call into the state machine from inside the poll.** The network poll
   runs inside a hook, so `SetState(STATE_GAMESTART)` from a message handler is
   re-entrant. It crashed the client outright, at a near-NULL address. Defer to the
   next frame (`gMp.pendingLaunch`).
3. **`-level` boots the engine straight into a city, frontend bypassed.** Giving it
   to a joining client means the client is already booting a level while the module
   drives its own join and launch — an access violation. `-level` belongs to the
   host; the client follows the session. (The arena/MP-map flag *is* local and must
   be given to both — it is not in the session config yet.)
4. **A reliable frame must never be dropped or half-written.** Non-blocking send
   returning "not ready" is normal, not an error. Dropping the frame cost a missing
   WELCOME and a client that waited out its timeout; a partial write leaves half a
   frame on the wire and desynchronises the stream for good.
5. **A level load silences both sides** for longer than the idle timeout, so the
   timeout must stand down while a launch is in progress (`gMp.busyUntilMs`).
6. **`GameType` 0 is `GAME_MISSION`.** Anything that launches without setting it
   gets the mission ladder — Undercover mission 1.
7. **`FEmain.c` has mixed line endings**, so a one-shot `"...\n..."` replacement can
   silently do nothing. Use `\r?\n`-tolerant patterns.
8. **A backslash in a generated file is an escape waiting to happen.** A path
   template containing `\bin` is a *backspace*; it silently corrupted every
   launcher's `EXEDIR`. Generate, then scan for stray control characters.
9. **A debug switch must never change behaviour.** The only `break` guarding
   `recv()` in `MpProcessConn` was nested inside a leftover
   `if (getenv("MP_DEBUG") ...)` whose log line had been deleted, so it fired only
   WITH `MP_DEBUG`. Without it -- every packaged/real launch -- the game called
   `recv()` with nothing to read and dropped the peer the instant it connected
   (`dropped (socket error); 0 byte(s) received`, which the old log blamed on a
   middlebox -- impossible on loopback). `tools/check_debug_independence.py` now
   fails if any debug `getenv` guard wraps control flow or state.
10. **A host loading a level looks dead to a client.** While the host loads it does
   not poll its socket, so it cannot answer a HELLO for seconds, and the client
   cannot see the host's busy flag. The 5 s handshake deadline must therefore apply
   only to a peer that connected TO us (`hostSide`); applying it to our OWN outbound
   link dropped good joins -- the intermittent "Lost the server (HELLO sent, no
   WELCOME)". Our link is bounded by the idle timeout, which the busy grace stands
   down during a load.
11. **A hand-built wire struct must copy EVERY field.** `MpSendCarState` built the
   client's `MP_CARSTATE` header but never `memcpy`'d it into the send buffer, so
   the client transmitted uninitialised stack (0xCC) for `frame`/`count`. The host
   read `count = 204`, clamped it, failed the length check and silently threw away
   EVERY one of the client's position snapshots -- so the host never corrected its
   view of the client's car and the two simulations drifted with no correction at
   all (the "physics/steering not synced"). The `[mp] sync:` deviation line makes
   this visible: it must print for BOTH remote cars.
12. **A player who chose no car must get the SAME car on EVERY machine — and a
   DIFFERENT one from everyone else.** With no `-mpcar` nothing names a player's
   car: HELLO/WELCOME carry `0xFF`, `config.car` is -1, and the engine's own
   default is not readable until the mission header is parsed. The old code
   assigned the level's car table (`carNumLookup`) to REMOTE players only and left
   the LOCAL player on the level's default, so the machines DISAGREED about the
   local player's car — the owner's palette then landed on a different model (the
   "on the client the host is a slot-0 police car with a palette that does not
   match") — and two un-chosen players could both come out as the same model.
   `MpAssignedCarModel(playerId)` = `carNumLookup[city][playerId % 4]` is now the
   single source for that: bounded by the level's pool, computed identically by
   both machines, different per player -- and the LOCAL player's `wantedCar[0]` is
   set from it too, so it agrees with what the others assign it.
   `--host-car default --client-car default` reproduces a whole no-`-mpcar` session.
13. **A blocking socket under a non-blocking send path stalls the whole frame.** The
   send path (`MpFlushConn`/`MpSendRaw`) QUEUES what the socket will not take, but
   the connection sockets were set BLOCKING, so when a peer's receive window filled
   `send()` blocked inside the game loop: the world froze for seconds (both cars
   logged `spd=0`, sitting in place), the peer then looked dead, the host timed the
   client out and the client's socket died mid-`recv` with `WSA_INVALID_HANDLE`
   (error 6 — OUR handle was already closed). Every connection socket must stay
   NON-BLOCKING; `recv` is already guarded by `select()` and treats
   `WSAEWOULDBLOCK` as "nothing yet". (See `mp-sockets-must-be-nonblocking`.)
14. **A "packet loss" readout on TCP is meaningless — measure delivery.** TCP
   retransmits, so the stack always says 0% while the peer's data may be seconds
   late. The scoreboard's `loss N%` is an application-level EWMA of the frames in
   which NOTHING arrived from that peer, sampled ONCE per sim frame — `MpNetPoll`
   runs several times a frame (frame hook, overlay, lockstep) and per-poll sampling
   counted the extra calls as "nothing arrived" (a phantom 77% on a healthy link).
15. **A timeout must never compare `now` against a stamp taken later in the same
   poll.** `MpNetPoll` reads the clock ONCE at its top, and the receive path stamps
   each arrival as it happens, several steps later — and `GetTickCount` moves in
   ~15 ms steps, so an arrival is regularly stamped a tick AHEAD of that `now`.
   `(now - lastRecvMs)` is then a small NEGATIVE number, which in unsigned
   arithmetic is enormous, so `> idle_drop_ms` was true in the very poll that heard
   from the peer: the connection was dropped for `timeout` immediately after a
   successful read (its own `mp_diag` said `heard 31ms ago`). That is the
   long-standing "it disconnects after about a minute" report — it needed the poll
   to straddle a tick, so it landed at 61-66 s, and at 114 s on a later run, and a
   control run with NO on-foot state reproduced it, so it was never an on-foot bug.
   All such comparisons now go through `MpElapsedMs`, which clamps a negative
   difference to 0. (This also LOOKED like a socket failure: whoever dropped first
   made the other side's next `send` fail — `WSAECONNABORTED`/`WSAECONNRESET` — with
   a wedged send queue, which is why the send path now logs its error code.)

---

### A burst must never cost the connection

The read loop in mp_net.c used to drain the socket until select() said nothing was pending
and *then* parse, so every byte of a burst had to fit in rbuf at once. A burst bigger than
that buffer dropped the peer with "overflow", and a burst is exactly what a briefly stalled
local game produces: one real session died that way mid-chase after 54239 bytes
receiver-side, with nothing wrong on the wire. The read is now bounded by the space actually
left, the frames are parsed at the end of the same call, and a full buffer STOPS READING
instead of dropping -- the rest waits in the kernel buffer for the next poll, and the poll
runs several times a frame.

The buffer must also hold one maximal legal frame, and it did not: the parser accepts
env.len up to 8192, i.e. a 8196-byte frame, against an 8192-byte buffer, so a maximal frame
could never be assembled and would have been dropped as "bad frame" while the send side was
entitled to send it. MP_RECV_BUF is now derived from MP_RECV_MAX_FRAME.

And a **bad handle (err 6, WSA_INVALID_HANDLE / EBADF) is not a transient error.** It says
the handle is not a socket any more, i.e. the connection has already been closed -- by us,
or by the send path right after the peer reset it. Classifying it as "transient, kept alive"
turned a plain teardown into a mysterious "send failed" drop carrying a huge byte count on
the other end. When a session ends this way, read the FIRST event, not the line the drop was
reported on: in the run that produced this, the real first event was `send error 10054` for
that connection, the peer having reset it, which is what the pair harness does when a seat's
window ends and the process exits (exit 0, no dump).

## 11. Testing

`tools/README.md` has the detail. The one thing to internalise: **the mock is
one-sided.** `mp_test.py` can prove the wire format and the client's own behaviour,
never what two engines do to each other. Every serious bug so far needed two real
instances, which is why `mp_pair.bat` / `mp_localpair.py` exists — two run
directories built from junctions (separate working directories are what stop the
two logs and the two `mp.ini` files fighting), direct launches so the PIDs are real.

Read runs with:

```sh
grep -a "\[mp\]\|\[error\]" JERICHO.log | grep -av "JPPN\|JPPO\|pose:\|JPIN\|JPCS"
```

Markers worth knowing: `launching: city N mode M` (mode 0 = the mission ladder, so
the launch went wrong), `car: player N slot S`, `added N remote player car(s)`,
`map: drew N remote blip(s)`, `list:` (pause-menu rows), `peer dropped (<why>)`.

### The driving bot is a sparring partner, not a navigator

`--bot chase` on the pair rig puts the player cars under mp's own test bot
(`mp_bot.c`, live only when `MP_BOT` says so) so that a run has two cars that actually
meet. `chase`/`fight`/`pursuit` are deliberately not navigation: they probe for scenery
with the engine's own `CellEmpty` and steer around what they see - no road knowledge, no
route. `catmouse` is the set that DOES navigate, driven by the `ai/` library below. What
every set has is PROXIMITY, because a flee that runs away forever produces no collisions
at all: measured, the host used to reach d=38246 and wedge there. The fleeing host now
simply RUNS - the old ease-off no longer lifts its throttle, so distance never interrupts
the driving - and only past `turnback` does it reverse roles and drive back at its
pursuers (`MP_BOT_GAP=<ease>,<turnback>`, defaults `7500,15000`), while the chaser presses
to 450 units before it pauses. On a 50 s city pair neither threshold fired and the two
stayed in contact, which is what makes contacts happen. `tools/README.md` has the table
and the assertion regexes; a PASS from that rig wants `lost=0` and `dumps=0`. The `lost=2`
that run used to report was the clock-underflow disconnect (trap 15) and is gone; a
`lost=N` now means a real drop, which is what makes the verdict worth reading.

---

## 12. Roadmap

Ordered by what is proven broken and what unblocks the most. Items marked DONE are
implemented and, where noted, observed.

> ✅ The traffic work has **landed** — `MP_TAG_TRAFFIC`, disjoint `car_data` bands and
> owner-following contacts are in the code (`MP_PROTO_VERSION` 9). The prose below has
> been brought up to date with it; the current acceptance view, the measured baseline
> (2026-10-07) and the v1 sign-off gate are in [`SYNC_CHECKLIST.md`](SYNC_CHECKLIST.md).
> Items **B, E, G and H** were re-statussed against that baseline on 2026-10-07.

### A. Verify the pair end to end — DONE

Verified: a two-instance run (`mp_localpair.py`, and `mp_pair.bat`) reaches a match
with both cars present and no cop placeholder, each machine mirroring the other's
car to within single-digit world units while both drive. The client's auto-launch
and the roster ordering are confirmed, not reasoned.

### B. Stop the frontend-driven second start — NOT REPRODUCIBLE (2026-10-07)

The host can be dumped into Chicago some time after hosting. The signature — a
second start, using the frontend's city rather than the session's — fits the frontend
menu flow re-entering `MpBeginHost`/`MpStartMatch`. An unattended session must be
authoritative over the menus. Also give `MpBeginHost`'s idempotency a test.

**Both the test and the answer now exist.** `mp_localpair.py --menu-host` is that test:
it launches the host **without** `-level`, so it comes up in the frontend and
`MP_AUTOSTART` drives it through the menus, and it FAILS unless the host starts
**exactly one** match. Measured: **1 launch in 3 of 3 runs** — the second start does not
reproduce, and the guards (`MpBeginHost` returning early when already host, and the
frontend START press being `claimed`) hold.

What those runs *do* hit is the genuine **joiner crash** — see §13. The old report named
Chicago, the same city the crash was first measured on, so that crash is the likelier
explanation than a second start; this item is closed as **not reproducible**, not as
"fixed by an unrecorded change". (The crash turned out not to be Chicago-specific.)

### C. A snap writes a rigid body — DONE

Implemented: the adopted state carries the orientation quaternion and both
velocities (`MP_CARSTATE_HAS_BODY`), and `MpHandleCarState` rebuilds the handling
matrix from them rather than poking `hd.where`.

### D. Car-to-car collision — implemented, engine-triggered

A contact is reported (`MP_HIT`) and each owner pushes its own car, so a collision
moves both cars without breaking owner-authority.

**The trigger is the engine's own contact**, `JER_EVENT_COLLISION` (fired by
`GlobalTimeStep` for the pair it actually resolved, before either car's impulse is
applied, so the velocities read there are still the pre-impact ones — the true
closing speed). It used to be a proximity probe run on a later frame, and that is
the bug the pair's own log shows: by then our engine has already absorbed the
closing velocity, so the probe read ~0 and nothing was sent for exactly the hits
that mattered — "the collision is not always registered on the remote player".
The probe survives as a FALLBACK for an overlap the engine never saw (a snapshot
teleport) and stands down for 90 frames after the engine reports one, so the
accurate trigger is not starved by its own fallback (measured before: 3..5 engine
contacts against ~68 probe hits).

**The units were wrong by 4096x.** The thresholds and cap were documented in whole
units/frame but computed and compared as fixed-point (`closing` is a dot product of
a fixed-point velocity with a fixed-point normal, so it carried the fix-point
twice), and the impulse divided by `MP_FIXEDH` once more. A capped push therefore
moved a car by 0.005 units/frame: the log said `push -20,0,0` and nothing happened.
The closing speed is reduced to whole units/frame and the impulse is one multiply
(`px` IS the normal in the velocity's own scale).

**Applied once.** Both machines simulate both cars, so each resolves the same
contact locally; the receiving half applies the peer's push only when our own engine
has not reported that contact within the last few frames, and the owner's half is
skipped entirely on the engine-triggered path (the engine has already moved us).
The impulse lands at PRE_SIM, before the frame's `StepCars` integration.

Measured over the two-instance harness with the pursuit bot: `closing 26, giving up
15 units/frame` and the peer receives `push 48032,0,44656` (11.7 units/frame), on
both seats, with the duplicate suppression visible as "our engine has it, kept".

### E. Damage and health — OPEN (half done)

A wreck should look the same on every machine. On a **traffic** car it now does: traffic
damage rides `MP_TRAFFIC_ENTRY.damage[6]` and the peer re-dents its copy from it (domain
12). On a **player** car it still does not — `totalDamage`, `ap.damage[]` and
`needsDenting` do not ride `MP_CARSTATE_ENTRY`, so a badly bent player car is straight on
another screen (limitation 17). The missing piece is a health field on the player
carstate, not the mechanism.

### F. The arena in the session config — DONE

`MP_WELCOME` carries `arena` and the client applies it with the city, so both sides
load the same multiplayer map without being told separately.

### G. Smoothing for latency — OPEN (unchanged)

Remote cars move on a fixed input delay and will rubber-band under loss. Standard
remedies apply (interpolation buffer, extrapolation cap), and C has made the underlying
state trustworthy, so this is now the top *quality* gap rather than a prerequisite:
`JERICHO-MP.md` advertises internet play by direct connect, and at 100–200 ms ping the
world moves in visible steps. It is the reason the mod is described as **LAN-tuned**.

### H. Out of scope for now

Matchmaking beyond LAN, host migration, and **latency smoothing** (item G above).
Traffic/police replication is **no longer out of scope** — it landed (§ the traffic
work, `MP_TAG_TRAFFIC`, `MP_PROTO_VERSION` 9); the current acceptance view is
[`SYNC_CHECKLIST.md`](SYNC_CHECKLIST.md) domain 12. Chat is implemented (open on `T`,
send on Enter; join/leave and chat lines go to the engine status console,
`jer_console.h`, which scrolls and is greppable). The lobby's "Enforce Mods" policy is
implemented; nothing exercises it yet.

#### The AI (`MODS/mp/ai/`)

The mp bots are a testing component, but "drive to a place" is a general problem and the
pieces are therefore kept out of the session code entirely: `ai/` is compiled into the mod
(`premake5.lua:419` globs `MODS/<mod>/**.c`, so a new file there needs no premake project
edit beyond a regeneration) and depends on nothing but the engine queries below.

The split matters more than the code. `aimap.c` FILLS the grid and is nothing but engine
probes; `aimapgrid.c` (queries and cost), `aistar.c` (the pathfinder) and `ailocal.c` (the
local road/flee search) are PURE, which is what lets
`src_rebuild/Game/C/JERICHO/test/test_ai_path.c` build a grid by hand and exercise the real
cost model and the real A* with no game running - the same trick `carpinref.h` uses for the
car-switch release logic. That test is not decoration: writing it found two bugs in the
pathfinder and one bad budget.

Engine dependencies, and the traps in them:

- **`CellEmpty(pos, radius)`** (objcoll.c) is the scenery test. It anchors its CELL LOOKUP at
  the probe point and the radius only widens the box test - so a wall the car is already
  touching sits behind the first probe and reads clear. That is why the grid also tests the
  car's own position, and why a car parked against a wall is "inside a blocked sample" by its
  own probe radius. It also skips `MODEL_FLAG_SMASHABLE` and chairs by design (objcoll.c:49),
  which is the whole reason fences are drive-through;
- **`JerRoadAt` / `JerRoadInfoAt`** (dr2roads.c, added by this work) expose the road network:
  the surface at a point, its lanes and AI-lane bits, and `connect[4]`, the road GRAPH. Three
  traps are written down in the code because each cost time: `roadbits.h`'s
  `ROADS_GetRouteData` is a stub that always returns 1, so it would call every heading a road;
  `GetSurfaceIndex` returns the GROUND surface minus 32, whereas the road tables are
  indexed by `plane->surface - 32` as returned by `RoadInCell` (dr2roads.c:276-279,
  :530-537), which also reports -1 for an unstreamed region - i.e. the map edge; and, found
  last and the reason the "stays on the roads" item finally moved, **`GetSurfaceRoadInfo`
  returns 0 for a JUNCTION surface** (it fills the lane data only for straights and curves,
  because `civ_ai.c` treats a junction as a special node). So `JerRoadInfoAt` used to report
  EVERY intersection as "no road" - a hole in the network at exactly the places the AI has to
  cross, which made a car standing on a junction read as off-road. The hook now answers
  driveability itself (`kind=2`, `connect[]` from `ExitIdx`) and leaves `GetSurfaceRoadInfo`
  untouched for the engine's own users;
- **`MapHeight(pos)`** so each sample is probed at the ground height THERE. `CellEmpty`
  compares heights, so probing a distant sample at the car's own height would invent walls
  and lose real ones.

Nothing in the library divides by `MAP_CELL_SIZE` or `MAP_REGION_SIZE`: those are level-header
fields (map.h) that are ZERO in the frontend, and this module has crashed on them before. The
grid is positioned in world units, and the world is only sampled once `cells_across` says a
level is loaded.

`connect[4]` is deliberately NOT used for routing yet, even though it is exposed: the slots
are frequently -1 and the Chicago/Vegas loaders hand-patch missing links (dr2roads.c:174-257),
so it is a sparse, directional graph and not a navigable mesh. The grid is the substrate; the
graph is there for a future route-follower that starts and ends on a road.

#### Traffic and police sync — LANDED (these were the design notes)

**Implemented** in `MP_PROTO_VERSION` 9 — acceptance view is
[`SYNC_CHECKLIST.md`](SYNC_CHECKLIST.md) domain 12: a disjoint `car_data` **band per
machine**, owner-authoritative `MP_TAG_TRAFFIC` state (model + city + palette +
position + `damage[6]`), `MP_TRAFFIC_REMOVE` for a despawn, and owner-following
contacts and damage (a mirrored re-dent moves no vertices). The notes below are the
reasoning that got there, kept because the obvious approach is still the wrong one.

Before it landed neither was replicated: each machine spawned and drove its own civs
from the level data, so a car you hit on one screen might not be on the other.

**The prerequisite is a slot agreement, not a wire format.** Every option below
needs both machines to agree that "traffic car X" lives in the same `car_data`
slot on both sides, or the state on the wire lands on the wrong car. That is the
resident-slot pool `carhacks` already owns (`CarPageFindSlot`, the pin/owner
tracking #12/#14 added) — reuse it rather than inventing a second allocator, for
the same reason the texture import does.

**Authority per car, not per feature.** Pick one owner per car and have only that
owner simulate it, everywhere, including traffic. Mixed ownership is what makes
this look easy and then desync in a corner.

**The options, cheapest first:**

- *Cosmetic parity only* — replicate the **consequences** (a hit, a knock, a
  wreck) and let each machine keep its own traffic motion. Cheap, exercises the
  same wire path, and fixes the complaint that actually gets noticed ("I hit a car
  and nothing happened on your screen"). Good first step.
- *Suppress-and-draw* — the host owns traffic inside a radius of any player and
  sends compact state (position, velocity, model, damage — a car's state is small);
  the client hides its own traffic in that radius and draws the host's. Needs a
  stable slot for the incoming cars, i.e. the prerequisite above, and a fade at the
  radius edge so cars do not pop.
- *Full lockstep* — the engine is already a fixed-step simulator and the mp module
  already steps the world deterministically for player cars, so traffic would follow
  for free **if** both machines start from the same state and consume the same
  random stream. `Random2` is frame-deterministic (combatd2 relies on it), so a
  shared seed is possible — but any divergence compounds silently and there is no
  cheap way to detect it. Attractive, and the reason it is last.

**Police are a separate problem from traffic**, even though they share the machinery:
a cop's *target* is chosen per-machine, so replication has to carry a **player id**,
not a local pointer or a car slot — the machine that receives it may resolve that id
to a different `CAR_DATA*`. Police also change behaviour on contact with a player,
which is exactly where a desync becomes visible. Do traffic first.

**Do not hand a synced car to the traffic AI.** `players.c:164` will put our cars
under `CONTROL_TYPE_CIV_AI`, and `PingInCivCar` then reads AI data a
module-created car never had — this already cost one access violation and has a
guard (commit `4c79e966`). Any replication that puts cars into the civ population
must keep that guard meaningful.

---

## 13. Unverified

Kept honest and separate, because the difference matters when picking this up.

> See [`SYNC_CHECKLIST.md`](SYNC_CHECKLIST.md) for the current, per-domain acceptance
> view, the measured baseline (2026-10-07) and the v1 sign-off gate. This section has
> been reconciled with the code: **traffic sync is landed**; the **late joiner dropped
> for `timeout` while it loads** is fixed (the poll-gap credit in `mp_net.c`); and the
> old "**~65 s drop**" is re-described below as a *freeze*, which is what it is.

**Observed working:** the transport (HELLO/WELCOME/REJECT/roster/START/INPUT/PING all seen on the wire), discovery and the beacon, a client being accepted and
launching into a live match, remote cars being engine-simulated with the right
`controlType`/`padId` and present in `active_car_list`, a remote car accepting
throttle from replicated input, a client gathering itself beside the host, map blips firing
(`map: drew N remote blip(s)`), the crash from `-level`-on-the-client and its fix.

**Reasoned but not observed:** that the roster ordering removes the cop placeholder
on both machines; that the client's unattended auto-launch reaches the level from a
fresh pair; that the pause-menu list shows the right names/vehicles/ping on screen
(the row *contents* are verified from the log, the drawing is not).

Car-to-car collision is now OBSERVED in both directions: with the pursuit bot the
same contact appears as "we bumped player N (engine contact: closing 26, giving up
15 units/frame)" on one seat and "player N bumped us (push 48032,0,44656)" on the
other, and the two sims stay within 1-3 units (the adopt lines), so the push lands.

**The "~65 s drop" is really a freeze, and it is measured (2026-10-07).** Repeated at
60 s the pair run **stalls 2 of 3 times**, always on the **joiner**, and the joiner's
*simulation* stops — its lockstep heartbeat stops while its socket keeps polling —
with **no `LEAVE`, no `timeout`, no drop** (`lost=0`) and **no crash dump**. So the
old wording ("drops the joiner mid-match … a transport bug of its own") named the
wrong layer: it is not the transport, and it is not necessarily the unmodified build's
fault. Root cause unidentified; `tools/mp_smoke.py` is the gate that now fails on it.

**Known broken / open:**

- **A session can crash the joiner** in `crumpleDeformInternal+0x2A8` — the only crash
  seen in this workstream, and first recorded as 4/4 with `--level chicago`. **Corrected
  2026-10-08: it is not Chicago-specific.** It reproduced on **rio on a single-player**
  level while the `--sp` rig was being added, and a second such run passed, so it is
  **intermittent** and the city is not the trigger — the 4/4 reflects the conditions it
  was first measured under. The fault lands immediately after mirrored traffic
  (`recv JPTF` / `traffic mirror slot N re-dented`), so an `mp`↔`crumple` interaction is
  still the lead. Re-measure before attributing it. **It is also very likely what the
  old "frontend-driven second start (**Chicago**)" line was seeing** — the `--menu-host`
  rig starts exactly **one** match in 3/3 runs, so a second start does not reproduce,
  while this crash does.
- **Damage on a PLAYER car is not synced** — `totalDamage`/`ap.damage[]`/
  `needsDenting` do not ride `MP_CARSTATE_ENTRY` (limitation 17). Traffic damage **is**
  synced (domain 12).
- **Car-swap stress exhausts the resident slots** on a 6-module modlist
  (`no spare resident slot` ×40 in 60 s of cycling) — see the baseline in
  `SYNC_CHECKLIST.md`; re-measure with a clean `mp`-only modlist before fixing.

**Open, from the 2026-09 four-seat runs (re-taken with a clean modlist — the first
attempt had cainescrossfire enabled by accident, which rewrites car handling):**

- **Getting out of a car crashed the session — RESOLVED.** `MP_TEST_ONFOOT` called
  `ChangeCarPlayerToPed` directly, which points `player[0].spoolXZ` at the player's
  ped — but in a match the player is put straight into a car by `InitPlayer` and
  never had a ped, so `spoolXZ` went NULL-adjacent. The next civ-AI pass faults on
  it in two places that both read `spoolXZ`: `CivControl -> CheckPingOut` (civ_ai.c
  reads `MainPlayer.spoolXZ->vx`, `JERICHO_dev.exe+0xE3B5`) and `PingInCivCar`
  (reads `player[playerNum].spoolXZ->vx`, `+0x102A0`) — one root cause under both
  addresses. The levers now call `ActivatePlayerPedestrian` first, exactly like the
  engine's own leave-car path (handling.c), so the ped and spoolXZ are valid.
  Re-entry was broken too: the vacated car was parked as `CONTROL_TYPE_PLAYER`,
  which `TannerCanEnterCar` refuses (it only accepts CIV_AI). It is now left as the
  stopped/empty civ car, which the AI ignores and the player can re-enter
  (commits 89fb0f83, 8c523f5b).


- **The third joiner was cainescrossfire.** With it off, all four seats join:
  `host_joins=3/3 joiners_accepted=3/3`. Do not trust a 3+ seat result with another
  module enabled; check the boot log's module inventory first.
- **Late joiners now start where the level starts.** On the host:
  `player 2 -> slot 2 at the level's own start 174886,1712 (+1200 lane), no y` and
  `player 3 -> slot 3 at the level's own start 175486,1712 (+1800 lane), no y` — i.e.
  the map's own start (173686) plus the lane, with the height the engine placed. The
  earlier `-17249,-60129` reading (which looked like `PlayerStartInfo[0]` being
  rewritten) did not recur, so it is most likely another symptom of the same accident
  rather than a real hazard — but capturing the start once at level launch is still
  the belt-and-braces answer.
- **The remaining blocker: a late joiner is dropped for `timeout` while it loads.**
  The dropped peer's stage was `WELCOME received, awaiting the level`; the client's
  own log says `dropped (timeout)`; the host then sees the socket go invalid
  (`recv error 6`, then `send failed`) and drops its side. The shape of this is a
  load, not a dead peer: the idle check counts wall-clock time since the last packet,
  and a machine inside a long blocking level load is not polling — so it returns from
  the load, sees that it "has not heard from the host" for the whole load, and tears
  the session down. `MP_BUSY_LAUNCH_MS` already exempts the launch that both sides
  order together; the late joiner's own load needs the same treatment (or the timeout
  must be measured against the time the module actually polled).
- **FIXED: a model change rebuilt nothing, and the wire carried a SLOT.** Both halves
  are done: `MpAdoptRemoteCar` now rebuilds the mesh (`ap.carCos` +
  `CreateDentableCar`, the only writer of the drawn vertex dump), and the wire
  carries a (city, model) pair -- `MP_CARSTATE_ENTRY.model` plus `modelCity` --
  which the receiver resolves to ITS OWN resident slot (`MpResidentSlotForCar`).
- **LANDED: the hotload** (carhacks + the engine, `carhacks/MP_ADAPTER.md`). A machine
  that loaded its level BEFORE a peer's pick can now materialise that peer's imported
  car in full: geometry (`JerHotLoadCarModel`, into the engine's own pool - the level's
  `malloctab` is rewound per load), the per-slot source city, the COSMETICS
  (`JerHotLoadCarCosmetics`: wheels, shadow corners, collision box, COG), and the
  TEXTURE PAGES + rows (`JerHotLoadCarTpages`, which re-runs the level-load pin walk;
  `CarPinRecord` is idempotent so the cars already pinned are untouched). mp's swap then
  rebuilds a car already on the road and logs it.
- **LANDED: the catch-up.** A joiner learns who drives what from the per-player table
  (`CHK_NET_CARS`) and the host's agreed set, and both now BUILD what they name - its
  level loaded before any of those cars existed in it, so without this it drew them as
  the level's own car of that number ("player 3 didn't see player 2's imported car").
- **The slot mapping is canonical** (`chkImportCanonicalSlot`, carhacks): lowest owning
  player id first, taking the i-th spare the LEVEL leaves free. Append-only (a joiner
  cannot displace a car already in a slot), and the same mapping on every machine, which
  is what makes the page indices and palette rows baked against a slot agree. A peer's
  assigned car is not a choice and is not published (`MpLocalCarChosen` /
  `jer_net_local_car_chosen`).
- **OPEN: palette and page placement with more than one guest city.** On the host a
  second imported city's textures and colours come out wrong while the first is right
  ("the vegas car imported proper but not the havana one's textures and colors"), and the
  machine that imported a car gets SCENERY textures contaminated. Both are collisions in
  the shared pool/bank rather than missing imports. The bank holds three guest cities
  (`CIV_CLUT_ROWS 32` / `CIV_CLUT_IMPORT_ROW 8` / `CIV_CLUT_BLOCK_ROWS 8`) and the tpage
  remap indices are `110..127`; test with a DIFFERENT palette number per player so a
  mix-up is unambiguous.
- **OPEN: the host's mapping is not yet authoritative** when a client's own level load
  precedes a lower-id peer's pick: measured, client2 derived slot 7 for its own car where
  the host derived 9. The clients already receive the host's set, so adopting it (and
  moving a car that must move, with a re-hot-load) is the shape of the fix.



## 14. Version identity, and shipping one build to both machines

`JERICHO_BUILD_VERSION` is baked in at PREMAKE time (`git describe --tags --always
--dirty`), so the digest the game reports — `MpBuildHash()`, printed as
`[mp] multiplayer ready (... build be0d, mods 13bd)` and on the pause-menu
scoreboard under its `PLAYERS` panel — describes the tree the vcxproj was generated
from, NOT the source as it stands now. That is why `sync_lan.bat` runs
`premake5 vs2019` BEFORE the build: a stale `build/` directory would otherwise ship
the previous release's stamp. `MpModHash()` folds the enabled module list in the
same way, so a mod enabled on one machine only is a mismatch too.

`strict_version = 1` makes the handshake REFUSE a peer whose build or mod digest
differs, instead of accepting it and desyncing mid-race. It defaults to 0 so a pair
mid-iteration can still connect; the LAN package ships it as 1, because that
package is always copied whole.

One command produces the package:

    JERICHO\MODS\mp\tools\pack_lan\sync_lan.bat

(regenerate -> build -> `JERICHO_mp_lan_<build>.7z`: the exe, the DLLs,
`config.ini`, `VERSION.txt`, `DRIVER2` minus the FMV, `JERICHO` with `modlist.ini`
`mp = 1`, an `mp.ini` with `strict_version = 1`, and the launchers.) Two traps live
in that script: `build_dev.bat` hands msbuild a RELATIVE project path, so it needs
`src_rebuild` as the current directory; and computing the root as `..\..\..` breaks
`cd /d "%~dp0"` (the trailing backslash escapes the quote), so the root is
normalised with `pushd`.

## 15. Following a player who changes car

The ENGINE owns changing cars: `ChangePedPlayerToCar` / `ChangeCarPlayerToPed`
(players.c) mutate `player[]` and the car's pad link IN PLACE and never call
`InitPlayer`, so no spawn path sees it — and there is no enter/exit event either.
The mod therefore watches the engine's own `player[0].playerCarId` (a char, -1 = on
foot) every sim frame (`MpFollowLocalCar`) and adopts the change: slot, model,
palette. `player[0]` is always US — every machine runs its one local player in
engine slot 0, and the remote players live in the higher slots the mod inits.

The change travels in the per-frame carstate: `MP_CARSTATE_ENTRY` carries the driven
model NUMBER and its city (`model` + `modelCity`, 0xFF = on foot). The peer resolves
the (city, model) to its OWN
resident slot (`MpResidentSlotForCar`) and matches the VEHICLE **in place**:
`cp->ap.model` -- the slot it already drives for that player -- and the colour.

Two hard-won rules:

* **NEVER move a player onto the car the owner named.** Slot numbers do not mean the
  same car on two machines (traffic is not replicated), so that warps the player
  into an unrelated car — seen as "the host teleported into the client's old car and
  the client ended up warped in as a traffic car". The hijacked car also belongs to
  the LOCAL traffic system, which then recycles or steps it and CRASHES
  (`PingInCivCar` on one side, `StepSim` on the other, both read out of dumps).
  There is deliberately no carSlot on the wire at all — see `MP_CARSTATE_ENTRY`.
* **A car the mod created (`InitPlayer`) has no civ-AI state, so it may reach the
  traffic AI ONLY as a stopped, empty car.** Getting OUT leaves the car as CIV_AI
  with thrustState STOP / ctrlState EMPTY (what `ChangeCarPlayerToPed` set):
  `CivControl`'s STOP branch does no work (`CivAccelTrafficRules`' STOP case is an
  empty break), so the AI never touches the uninitialised nav fields, and
  `TannerCanEnterCar` still accepts it so the player can walk back and get in. It
  must never reach an ACTIVE civ state (driving/turning) — that is what used to
  crash inside `CivSteerAngle` (rva 0xC961 in one dump).

A model the renderer has not loaded is NOT applied (`gCarCleanModelPtr[model] ==
NULL`): pointing `ap.model` at a mesh that does not exist is a crash, not a
cosmetic glitch — it keeps the old model and logs.

Headless: `MP_TEST_CARCHANGE=<seconds>[,<exitSeconds>]` performs a real change (the
engine's own `ChangePedPlayerToCar`, onto the nearest civilian car) and optionally
gets out afterwards. It fires on every machine the env var reaches.

It cannot test the MID-MATCH IMPORT, though, and the reason is worth keeping: it takes
over a car that is already in the level, so the peer holds that car too and there is
nothing to import. What has to be imported is a car the peer does NOT hold. So:

`MP_TEST_CITYCHANGE=<seconds>,<city>[,<slot>]` takes the pause menu's own action
(`MpChangeCar`) onto a car from a DIFFERENT city, which is exactly the import case.
`city` indexes the cities the car mods know (`MpCarQueryCities`), `slot` indexes the
engine's per-city model table (`carNumLookup`), and it does nothing at all unless the
value has that shape — an absent or malformed value must never move a car by itself.

Measured on a RIO pair with `--seat-env host=MP_TEST_CITYCHANGE=10,1`: the host
hot-loaded a foreign car mid-level (`hot-loaded HAVANA model 1 into resident slot 7`,
geometry and cosmetics), and the client logged `player 0 drives VEGAS model 2 on the
wire; we render slot 0 (model 1, src -1, mesh present); this machine holds it in slot
-1` — the #13 gap, reproduced on demand instead of described.

## 16. Testing against the other PC, without touching it

A two-machine bug is diagnosed from two logs, so the rig exists to get both logs
with one command -- but it is built as a resident **agent** rather than a one-shot
copy, because a test machine you have to visit between iterations is the thing
that makes two-machine testing not happen.

`tools/remote/mp_agent.ps1` runs on the other PC (`START_AGENT.bat`, once, in its
own window) and serves a fixed command set over TCP: `ping`, `status`, `update
[tag]`, `rollback`, `start`, `stop`, `log`, `dump`, `quit`. It is PowerShell
because that is already on every Windows box -- no Python, no install, no admin
beyond the firewall rule -- and it only ever writes inside the folder it is
pointed at.

`tools/remote/mp_remote.py` is the client: `status`, `update`, `deploy`, `run`,
`logs`, `rollback`, `stop`. It drives the LOCAL seat directly (`Popen`, so a real
PID we can kill without guessing) and the remote seat through the agent.

The two properties that make it hands-free, both of which are worth keeping if
this is ever rewritten:

* **It pulls a published build; nothing is pushed to it.** `update [tag]` makes
the agent fetch that GitHub release (the rolling `alpha` pre-release CI refreshes
from main, by default) over HTTPS, and the command carries nothing but the tag. It
installs only the Release_dev Windows asset (`JERICHO_Release_dev_win64.zip`, which
carries `JERICHO_dev.exe`); the plain Release assets are refused, by name and by
content (an archive carrying `JERICHO.exe`), until they count as real releases. It
replaces only the exe, its `.pdb`/`.map`, `SDL2.dll`, `OpenAL32.dll`, `JERICHO`
(keeping the live `CONFIG` files) and `VERSION.txt`. The game data (1.6 GB) and
`config.ini` are deliberately outside that set because they do not change between
builds.
* **The agent is resident and restarts the game itself.** An update that arrives
while a game is running stops it, installs the build and starts it again with the
same arguments. Leave the other PC running a seat, ask for an update, and the new
build comes up on its own.

Why pull and not push (issue #1): the push version unpacked whatever zip arrived
on the port and checked it against a manifest inside that same zip, so anyone who
could reach port 1401 -- with a published default token -- could replace the exe.
Now the zip's SHA256 must match a digest from a separate source: GitHub's per-asset
`digest` field, or failing that a `SHA256SUMS` asset published beside the zips
(for a release that has one). With neither, the agent refuses to install. The listener binds
127.0.0.1 unless given `-Bind <LAN address>` (never 0.0.0.0), refuses the old
`jericho-mp` token, generates a random one into `mp_agent.config.json` on first
start and never writes it to `mp_agent.log`; the firewall rules cover only the
Private profile and the local subnet. That is still a LAN tool, not something to
expose to the internet. A later step could sign the archives and pin the public
key in the agent, which would also cover a compromised release.

Traps found by actually running it (each one defeated the rig until fixed):

* A build is **verified before it is applied** and refused whole if anything
disagrees -- download and unpack to `_mp_staging`, check the hash (and that no zip
entry escapes the staging folder), then swap. The swap is a set of renames on one
volume, undone completely if any of them fails, and what it replaced goes to
`_mp_previous` for `rollback`. A half-applied build is worse than a failed update.
* `Get-FileHash` returns **UPPERCASE** hex while GitHub's digest and `sha256sum`
are lowercase, so normalise case before comparing or every verification fails.
* The rolling `alpha` git TAG does not move when CI refreshes the release (only the
assets are replaced), so the commit an alpha build came from is read from the
release body ("Rolling alpha build from `main` at <sha>"), not from the tag.
* Never derive control flow from a function's return value in PowerShell: every
helper emits its own output, so `$quit = Invoke-Command ...` read "OK stopped" as
"quit" and shut the agent down on the first sync.
* Don't name a script-scope variable after a parameter: `$script:Token = ...` in
the agent silently overwrote the `-Token` parameter, so the check that refuses the
default token was looking at the wrong value.
* The log is read with `FileShare.ReadWrite`. The game holds `JERICHO.log` open
while it runs, and pulling a LIVE log is the point -- `ReadAllBytes` fails with
"being used by another process" exactly when the log matters most.

---

## 17. Changing car, and restarting, mid-match

Both of these are things the ENGINE does not know how to do in a match, and both
are reachable from the Multiplayer pause page.

### Change car

`Multiplayer` -> `Change car` is a two-row cycler and an apply row (`mp.c`,
mirroring the colour editor): a city, one of that city's cars, then "Respawn as
this car". A row is a CYCLER and not a list because the pause menu's item set is
fixed when the page opens and a module menu cannot nest a second level
(`jer_pause_menu.h`) -- so a 12-car roster is reached by cycling.

Applying it calls `MpChangeCar(city, model)` (`mp_session.c`), which does the
SAME in-place re-model a peer's car goes through (`MpAdoptCar`, the session-free
core that `MpAdoptRemoteCar` wraps in two lines): only the
cosmetic model and the mesh change, on the slot we already drive, so nothing
about this world's car slots -- and so nothing about anybody else's car -- is
disturbed. On foot, the player is put into their parked car first, because
"change car" from the pavement has to end with a car. The choice then travels the
ordinary way: our carstate carries the new `(city, model)` from that frame on, so
every peer re-models its copy of us, and a client also tells the host so the
host's roster names the car it is really driving.

The page is registered UNCONDITIONALLY, so it is there in SINGLE PLAYER as well.
Out of a session `MpChangeCar` takes the car from the engine
(`MainPlayer.playerCarId`, `MainPlayer.playerType`) and does the same local
re-model with no roster row to update and nothing to publish. The picker offers
the WHOLE roster of every city, foreign ones included, exactly as it does in a
session -- because choosing a car that is not resident here yet IS the request
that brings it in: `MpChangeCar` asks carhacks to import it (`MpCarQueryLoad`)
before it re-models, in single player as much as in a match. (It once offered only
what the level could already build out of a session, on the theory that anything
else could only be refused. It never prevented a refusal -- the import was right
there -- it just made every city list nothing, which read as "no cars available".)
A pick that still cannot be loaded is refused with a reason
(`MpChangeCarRefusal`), leaving the current car in place.

**Where the cities come from.** mp can only offer what this machine can actually
hold, and a second city's car data is carhacks' business, so mp ASKS:
`mp_carquery.h` is a three-event contract (custom JERICHO event ids, so no shared
header needed beyond that one file) answered by `carhacks/mplive.c`:

| event | question | answer |
| --- | --- | --- |
| `MP_CARQ_CITIES` | which cities can this session offer? | 0..3 indices, or 0 = "nobody knows" |
| `MP_CARQ_LOAD` | make `(city, model)` available here | `ok` |
| `MP_CARQ_CHOSEN` | (notice) the local player now drives `(city, model)`, or the switch did not happen | none - carhacks tells the session and releases the old car's slot |

No answer is not an error: mp falls back to the session's own city and its
frontend roster (`CarAvailability[city][slot]` + `carNumLookup[city][slot]`,
the same two arrays the stock car screen and carhacks' own picker read), which is
the whole feature minus the cross-city half.

`MP_CARQ_LOAD` is not new machinery on carhacks' side either: it is the sequence a
mid-match peer pick already goes through -- put the car in the session's canonical
spare slot, then `chkImportHotLoad`, which reads that city in, builds the slot's
geometry in the engine's own pool, applies its cosmetics and records its texture
pages. mp only swaps once `MpResidentSlotForCar` finds the car AND its mesh is
built (`gCarCleanModelPtr`), because pointing a car at an unbuilt slot is a
crash, not a cosmetic glitch.

After the swap, `MpChangeCar` fires `MP_CARQ_CHOSEN` (`changed = 1` when our car
really is on the new car's slot, `0` when it is not), and `MpFollowLocalCar`
fires it when the player gets back into a car on foot. That notice is when
carhacks learns which car this player drives: it advertises the car
(`chkNetAdvertisePick`, and `chkNetPublishSet` on the host) so every other
machine loads it, and releases the old car's slot -- only once no player names
that car and no car is still on the slot (`carhacks/MP_ADAPTER.md`, "Releasing a
slot"). Advertising after the swap rather than in `MP_CARQ_LOAD` means nobody is
told about a car that never got driven. A direct car-to-car move (`CARCHANGE`,
without leaving a car) does not fire it.

That advert is re-sent every few seconds even when the car has not changed
(`CHK_NET_ADVERT_FRAMES`, carhacks' FRAME watcher). It is the session's only
one-shot announcement -- everything else is re-broadcast by the host on a timer --
so a lost advert, or an import it triggered that failed, would otherwise leave the
host drawing that player's old car for the rest of the match. `CHK_DROP_ADVERT=<n>`
on a seat throws its first `n` adverts away, which is how the recovery is tested.

Test lever: `MP_TEST_PAUSECAR=<secs>[,<city>[,<model>]][;...]` runs the same
call the Apply row does, so a mid-match change is reproducible headlessly; a
`;`-separated list makes several timed changes, each counted from when the
session starts running (`tools/mp_tries.py --scenario T2` uses seven).

To WATCH one of those changes rather than read it afterwards, run the pair with
`--vramview --shots DIR`: each car change leaves three pictures (the game, the VRAM
viewer, the console) and an `index.txt` row naming the line that caused them and the VRAM
state at that moment (`tools/mpshots.py`, which also works against a hand-played session).

### Restart is a soft reset

The stock Restart calls `EndGame(GAMEMODE_RESTART)` and rebuilds the level
(`main.c`). In a match that is not one player's to do: everybody else is in that
same level and their session is not ours to reset. So JERICHO now hands a module
the pause menu's ANSWER before acting on it -- `JER_EVENT_GAME_QUIT`, fired in
`main.c` with the engine's `MENU_QUIT_*` code -- and `JER_RESULT_STOP` means "I
handled it", so the engine runs none of its endings.

mp claims RESTART and runs `MpSoftRestart()` instead: the player is put back at
the level's own start (`PlayerStartInfo[0]`, x/z only with `t[1] = 0` so the
engine resolves the ground, exactly as a car created at level init), velocities
and speed zeroed, the handling matrix rebuilt for the new spot (a teleport that
leaves it behind collides at the OLD place), the car REPAIRED (`totalDamage = 0`,
`ap.damage[]` zeroed, `CreateDentableCar` -- the only thing that rebuilds the
drawn vertices from the clean model -- and `JER_EVENT_RESET_CAR`), and the wanted
level cleared (`felonyRating` AND `pedestrianFelony`, because `GetPlayerFelony`
picks between them). On foot the player is put back into their parked car first.
Then mp unpauses: claiming the code means the engine does not, and the pause menu
has already closed itself.

The others see the car arrive at the start, because the owner-authoritative
carstate carries the pose like any other move. Quit and the other codes are left
alone -- leaving a match is a real thing a player should be able to do.

Test lever: `MP_TEST_RESTART=<secs>` fires the SAME event the engine fires, so a
pass means the hook, mp's claim and the reset are all wired.

### The on-foot bot

The test bot drives the on-foot Tanner as well as a car (`mp_bot.c`). Tanner is
tank-steered (`TANNER_PAD_GOFORWARD`/`GOBACK` walk him, `TURNLEFT`/`TURNRIGHT`
rotate him, `pad.h`), which is close enough to a car that the SAME logic works:
probe the scenery with the engine's own `CellEmpty`, pick a clear heading, steer
at it. Only the body read (our `player[0].pPed`) and the pad emitted change, and
a pedestrian needs a shorter probe and a smaller clearance than a car
(`MPBOT_PED_*` / `MPBOT_PED_CLEAR`). He walks at the nearest other player and
presses `TANNER_PAD_ACTION` when he reaches a car, so the whole get-out -> walk ->
get-back-in loop runs without a human. It reaches the engine through
`JER_EVENT_PED_INPUT`, which fires immediately before `ProcessTannerPad` -- the
on-foot twin of the car's `JER_EVENT_NET_INPUT`.
