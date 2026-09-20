# How mp works, and what is next

This is the deep version: the model, the protocol, the lifecycle, the engine
touchpoints, the traps that keep costing days, and a prioritised roadmap. It
assumes you have read `../README.md` (what the mod is, how to configure it) and
`../tools/README.md` (how to run it).

**Status at the time of writing.** The join path works: a player can host, another
can join, both get a car, they start together, and each drives the other. The
transport is proven. What is *not* proven is a clean two-player match end to end —
see "Roadmap" and, specifically, "Unverified" at the bottom, which lists exactly
what has been observed and what has only been reasoned about.

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

At `GAME_START` every car exists. The host then broadcasts `SPAWN` — the meeting
point — and every machine lines up on it, spaced `MP_SPAWN_SLOT_DIST`, all facing
the host's heading, with the orientation matrix rebuilt (the collision box is
built from the matrix, so a heading alone is not enough) and velocity zeroed.

The meeting point is the host's own car, and the host is the only machine that can
decide it. A per-machine "gather the others next to me" makes the two sides
disagree, the resync sees the divergence and drags the cars back and forth.

### The steady state

Per simulation frame (`JER_EVENT_PRE_SIM`):

1. `MpSendInput(MpLocalPad())` — our pad goes out. A client sends one row; the host
   merges every row it has seen and broadcasts the whole set.
2. Every `MP_SYNC_INTERVAL` frames: snapshot exchange.
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
| `JPSS` | H→C | SESSION: config broadcast |
| `JPST` | H→all | START: begin the level launch |
| `JPSW` | H→all | SPAWN: the meeting point |
| `JPIN` | C→H, H→all | INPUT: this frame's pad set |
| `JPCS` | H→C | CARSTATE: resync snapshot |
| `JPPN` / `JPPO` | both | PING / PONG (the PONG echoes the tick, so the host can compute RTT) |
| `JPCH` | both | addon channel payload (the `jer_net.h` bridge) |
| `JPLV` | both | LEAVE |
| `JPCX` | both | chat line (scaffolding) |

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

## 7. Resync

Snapshots exist to correct drift between two simulations, never to carry the pose.
The host sends them every `MP_SYNC_INTERVAL` frames; the receiver snaps only when
the incoming position is further than `MP_SYNC_SNAP_DIST` from where its own
simulation has the car.

Between snaps the engine owns every car. Applying a snapshot every frame is what
turned remote cars into puppets and erased collision responses.

**Known gap:** a snap writes position and heading (`hd.where.t`, `hd.direction`)
but not the rigid body — not the orientation quaternion, not
`st.n.linearVelocity`. `hd.direction` is an *output* the engine re-derives; a snap
that relies on it is not a proper body state. This is a real defect, not a
stylistic one.

---

## 8. The pause-menu player list

While the pause menu is open, the players are listed down the left: the host first
and in cyan, then each player's name, index, vehicle (`-1` = on foot: no car slot,
or a car standing there with nobody driving it) and ping. `MP_PAUSE=1` holds the
menu open for testing; `MP_DEBUG` logs each row, which is how the content gets
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
  remote player cars).
- **`JER_EVENT_LEVEL_LAUNCH`** — gained in/out `timeOfDay`/`weather` so a session's
  host can dictate the match conditions.
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

---

## 11. Testing

`tools/README.md` has the detail. The one thing to internalise: **the mock is
one-sided.** `mp_test.py` can prove the wire format and the client's own behaviour,
never what two engines do to each other. Every serious bug so far needed two real
instances, which is why `mp_pair.bat` / `mp_localpair.py` exists — two run
directories built from junctions (separate working directories are what stop the
two logs and the two `mp.ini` files fighting), direct launches so the PIDs are real.

Read runs with:

```sh
grep -a "\[mp\]\|\[error\]" REDRIVER2.log | grep -av "JPPN\|JPPO\|pose:\|JPIN\|JPCS"
```

Markers worth knowing: `launching: city N mode M` (mode 0 = the mission ladder, so
the launch went wrong), `car: player N slot S`, `added N remote player car(s)`,
`map: drew N remote blip(s)`, `list:` (pause-menu rows), `peer dropped (<why>)`.

---

## 12. Roadmap

Ordered by what is proven broken and what unblocks the most.

### A. Verify the pair end to end — do this first

The client's auto-launch is built but never confirmed against a running pair, and
the roster fix is reasoned, not observed. Everything below is guesswork until a
two-instance run reaches a match with both cars present and no cop placeholder.
`mp_pair.bat` on Havana arena 0, host car 0, client car 12.

### B. Stop the frontend-driven second start

The host can be dumped into Chicago some time after hosting. The signature — a
second start, using the frontend's city rather than the session's — fits the frontend
menu flow re-entering `MpBeginHost`/`MpStartMatch`. An unattended session must be
authoritative over the menus. Also give `MpBeginHost`'s idempotency a test.

### C. A snap must write a rigid body

Position and heading are not a body state. Write the orientation quaternion and
`st.n.linearVelocity` — reuse the engine's own handling-matrix helper rather than
poking `hd.where` — and only then is the resync a real correction. Without this,
the divergence between two simulations has no correct way to be fixed.

### D. Make car-to-car collision a tested feature

This is the payoff of input replication and it has never been demonstrated. Both
cars are non-local-`controlType` engine cars, so the collision loop should pair
them; what needs checking is that the pair survives the `hd.speed` guard, that the
box orientation is right after a spawn, and that both sides feel the push.

### E. Damage and health

A wreck should look the same on every machine. `totalDamage`, `ap.damage[]`,
`needsDenting` are not synced at all, so a car that is badly bent on one screen is
straight on another.

### F. Put the arena in the session config

Right now the multiplayer map/arena is a *local boot flag*, so the client has to be
told with `-mp` and a mismatch silently loads a different map. It belongs in
`WELCOME`/`SESSION` with the city.

### G. Smoothing for latency

Remote cars move on a fixed input delay and will rubber-band under loss. Standard
remedies apply (interpolation buffer, extrapolation cap), but only once C makes the
underlying state trustworthy.

### H. Out of scope for now

Matchmaking beyond LAN, chat/rally beyond the scaffolding in place, host migration.
The lobby's "Enforce Mods" policy is implemented; nothing exercises it yet.

---

## 13. Unverified

Kept honest and separate, because the difference matters when picking this up.

**Observed working:** the transport (HELLO/WELCOME/REJECT/roster/START/SPAWN/INPUT/
PING all seen on the wire), discovery and the beacon, a client being accepted and
launching into a live match, remote cars being engine-simulated with the right
`controlType`/`padId` and present in `active_car_list`, a remote car accepting
throttle from replicated input, the meeting point being adopted, map blips firing
(`map: drew N remote blip(s)`), the crash from `-level`-on-the-client and its fix.

**Reasoned but not observed:** that the roster ordering removes the cop placeholder
on both machines; that the client's unattended auto-launch reaches the level from a
fresh pair; that the pause-menu list shows the right names/vehicles/ping on screen
(the row *contents* are verified from the log, the drawing is not); car-to-car
collision actually pushing both cars.

**Known broken:** the frontend-driven second start (Chicago); a snap does not write
a rigid body; damage is not synced.
