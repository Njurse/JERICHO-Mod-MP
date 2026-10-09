# MP synchronization checklist — v1 ("take a ride, swap cars")

The definition of **done** for synchronization in the first multiplayer release.
The release it gates is one where a small LAN group can **take a ride together**
(the stock *Take a Ride* free-roam gamemode) and **swap cars mid-match**.

This is an **acceptance checklist**, not a work plan: every domain below states the
invariant that must hold, the command or lever that exercises it, the exact log
lines that carry the evidence, and the pass bar. Domains that are knowingly
imperfect are collected under [Accepted limitations of v1](#accepted-limitations-of-v1)
and are **not** release blockers.

- What a match *is*, in plain language: [`JERICHO-MP.md`](JERICHO-MP.md)
- The deep internals (every field, every trap): [`ARCHITECTURE.md`](ARCHITECTURE.md)
- The car-identity / cross-city adapter: [`../../carhacks/MP_ADAPTER.md`](../../carhacks/MP_ADAPTER.md)
- Every harness and launcher: [`../tools/README.md`](../tools/README.md)

> Provenance: written against branch `fix-car-switch-release` (HEAD `88fcb6c6`,
> 2026-10-05), `MP_PROTO_VERSION` 9. Some prose in `ARCHITECTURE.md` and
> `JERICHO-MP.md` predates the traffic work and the load-timeout fix; where this
> document and the code disagree, **the code wins** — see
> [Accepted limitations of v1](#accepted-limitations-of-v1).

---

## How to read this checklist

Every domain is one block:

| field | meaning |
| --- | --- |
| **Invariant** | the property that must hold for the release to be "synchronised" |
| **Verify** | the harness/lever + command that exercises it, and the **exact log lines** to grep |
| **Pass** | the concrete accept/reject outcome — no "looks fine" |
| **Status** | `Verified headless` · `Verified LAN` · `Accepted limit` |

A domain is *signed off* when the identical command produces the pass bar on **both
seats** (the harness reads each seat's own `JERICHO.log`). One seat passing is not a
pass: "correct on the host but the client was still the old car" is the single most
common real failure here, and it is invisible unless the two logs are read
separately.

Two rules for judging a run:

- **STALLED is not a PASS.** A game that stopped simulating looks quiet, not broken.
  Keep `MP_HEARTBEAT=<secs>` on (the harness sets it) and treat a heartbeat that
  stops advancing as a failure.
- **A crash is not an Alt+F4.** An access violation leaves `JERICHO.dmp` beside the
  exe; a clean exit leaves none. Check for the dump (`mp_localpair.py` reports
  `dumps=N`), and attribute it with `tools/dmp_fault.py` + `tools/map_lookup.py`.

### What "synchronised" means here (the one rule)

Every machine simulates the whole world on its own; **the only things that travel
are the things a human decides or does** — identity, which car each player drives,
where that car is, the input they are holding, and which car has been hit. Every car
belongs to exactly one machine: **the machine of the player driving it**
(*owner-authoritative*), and everyone else adopts its state verbatim. The same holds
for traffic: a machine replicates only the traffic/police cars **it owns** (domain 12).
Everything else — how the traffic you do *not* own drives, pedestrians, the map — is
**not** synced and is not meant to be; see [`JERICHO-MP.md` §6](JERICHO-MP.md).

### The rigs

| rig | what it is |
| --- | --- |
| `python JERICHO/MODS/mp/tools/mp_localpair.py` | two (or `--players N`) real engines on **one PC**, a PASS/FAIL verdict, per-seat logs |
| `python JERICHO/MODS/mp/tools/mp_carstress.py` | 3 seats cycling **every car** the session offers, fast — the car-swap stress rig |
| `python JERICHO/MODS/mp/tools/mp_tries.py` | a host on its own city **plus a client picking a foreign city**, repeated, per-seat evidence |
| `mp_host.bat` / `mp_join.bat` (`tools/`) | two real machines, by hand — the final LAN pass |
| `JERICHO/MODS/mp/tools/pack_lan/sync_lan.bat` | one command: regenerate → build → the shippable LAN `.7z` |

Harness flags worth knowing (full list in [`../tools/README.md`](../tools/README.md)):
`--no-debug` (run the way a player runs — no `MP_DEBUG`), `--require PATTERN` (the run
**fails** unless the line appears), `--forbid PATTERN` (fails if it does),
`--until`/`--stall`/`--tail`, `--seat-env SEAT=KEY=VALUE`, `--players N`, `--keep`
(keep the run dirs), `--seconds`, `--settle`.

Levers are read from the environment, so an exported one reaches **every** seat —
use `--seat-env` to give seats *different* values.

---

## The completion checklist

Each domain below is a checkbox. v1 ships when domains 1–13 are Pass and 14–19 are
understood and accepted.

- [ ] 1. Session admission
- [ ] 2. Roster and identity
- [ ] 3. Car assignment (the ride key)
- [ ] 4. Car pose replication
- [ ] 5. Input replication is the fallback only
- [ ] 6. Mid-match car swap
- [ ] 7. Car colour / palette ownership
- [ ] 8. Spawn placement
- [ ] 9. Car-to-car collision handoff
- [ ] 10. Death, and respawn back into the match
- [ ] 11. Joining a live match (catch-up)
- [ ] 12. Replicated traffic
- [ ] 13. Pause keeps the world running, and chat
- [ ] 14–20. Accepted limitations (reviewed, not blocking)

---

### 1. Session admission

**Invariant** — Two machines on the same release admit each other; a peer whose
build/mods differ is **refused at join with a reason**, never left silently unable to
see the other.

**Verify** —
- Same release, headless: `python JERICHO/MODS/mp/tools/mp_localpair.py --players 2`
- The banner and the accept: `[mp] multiplayer ready (port %d, name '%s', firstRun=%d, build %04x, mods %04x)`,
  `[mp] hosting as '%s' (mod enforcement=%d)`, `[mp] accepted as player %d (matched=%d gamemode=%d city=%d)`,
  `[mp] conn: %s | peer %s | role %s | stage %s`
- The refusal path: `[mp] reject: version mismatch (proto %d/%d, sdk %d/%d, strict=%d, host ser...)`,
  `[mp] reject: mod mismatch (enforcement=%d)`, `[mp] join refused: %s (reason %d)`
- Shipped `mp.ini` must carry `strict_version = 1` (the LAN package ships it).

**Pass** — The pair's two seats both log the banner and reach a live match; their
`build`/`mods` digests agree. A deliberately mismatched peer is refused with a
`reject:`-class reason and **nothing half-connects**. No `[mp] join FAILED` on the
same-release pair.

**Status** — Verified headless. Re-confirm the refusal path on the LAN cut.

---

### 2. Roster and identity

**Invariant** — Every machine agrees on the **same set of players**, their ids and
names; every player is distinct and addressable.

**Verify** —
- `mp_localpair.py --players 3` (host + 2 joiners); `--players 4 --stagger 20`.
- The roster rows (the pause-menu log lever `MP_PAUSE` prints them):
  `[mp] player %d '%s' joined (%d/%d)%s`,
  `[mp] list: (build %04x mods %04x) %s #%d car %d slot %d %s%s`,
  `[mp] local row is player %d (row %d, car %d) - from localPlayerId`
- A leaver: `[mp] player %d is no longer in the roster - taking their car out of the world`

**Pass** — N seats produce N **distinct** rows; the rows' ids and names are identical
on every seat; each seat's "local row" names itself. A player who leaves disappears
from every roster.

**Status** — Row *contents* verified from the log; the on-screen drawing is verified
by eye at the LAN pass.

---

### 3. Car assignment (the ride key)

**Invariant** — For each player id, **every machine computes the same car model** —
whether the player chose one or was assigned one — and the players are **distinct**
unless they deliberately chose the same car.

**Verify** —
- The normal LAN/menu case (nobody passes `-mpcar`):
  `python JERICHO/MODS/mp/tools/mp_localpair.py --host-car default --client-car default`
  → `[mp] no car chosen -> assigned model %d (player %d, city %d)` and `[mp] assigned cars (city %d):`
- An explicit pick: `-mpcar [city:]model` or `[city:]slotN`; `[mp] car chosen: %s model %d (%s)`;
  headless `MP_TEST_CARSELECT=<roster slot>`.
- Distinctness across seats: `--seat-env host=CHK_FORCE_CAR=8 --seat-env client=CHK_FORCE_CAR=2`
  (a shared value makes every seat ride the same car and hides whether a peer's pick
  is respected — that is the whole point of per-seat env).
- How a peer resolves it: `[mp] MODEL: player %d drives %s model %d on the wire; we render slot %d`.

**Pass** — Player id P resolves to the **same** `(city, model)` on every seat; players
are distinct; the **local** player's own car is taken from the assignment rather than
left to the level default; no seat shows the slot-0 cop placeholder for a peer.

**Status** — Verified headless (both the default and the chosen path).

---

### 4. Car pose replication

**Invariant** — Each machine is the **sole authority** on the one car it drives and
sends that car's whole rigid body (position, orientation quaternion, both velocities)
every frame; every other machine adopts it **verbatim**, so a remote car can never
rubber-band against its own driver.

**Verify** —
- `python JERICHO/MODS/mp/tools/mp_localpair.py --seconds 60 --settle 5` (a human-like
  run) and `--bot pursuit` (motion + a guaranteed meeting).
- The per-snapshot deviation (**`MP_DEBUG=1` only** — the math is inside the guard, so a
  `--no-debug`/packaged run pays nothing and prints nothing):
  `[mp] adopt: player %d snap %u |d|=%ld d2=%ld (dx=%ld dy=%ld dz=%ld) dh=%d pal=%d md=%d`
- The pose census: `[mp] pose: player %d local=%d car %d at %d,%d,%d spd=%d ct=%d pad=%d hnd=%d vy=%d list=%d/%d`
- Read past the flood: `grep -a '\[mp\]' JERICHO.log | grep -av 'JPPN\|JPPO\|pose:\|JPIN\|JPCS'`

**Pass** — `|d|` on the adopt line is a **small steady** value (single digits of world
units is the measured norm); it must not grow run-long (drifting sims) or spike (the
resync fighting the engine). The remote car's `ct=` is `CONTROL_TYPE_PLAYER` and it is
present in `active_car_list` (`list=N/M`). The pair never reads 10,000+ units apart
(`--forbid "d=-?[0-9]{5}"`).

**Status** — Verified headless. The adopt line's `|d|` is *the* number to watch — it is
what "the cars are not where they are on the other machine" looks like as a number.

**The map marker is the same fact drawn.** A remote player's arrow is placed by
`JER_EVENT_DRAW_MAP` (`mp_map.c`), so it fails and passes with this domain — and it has
its own trap, because the hook fires on **three** surfaces and each needs a different
transform:

- **Verify** — `python JERICHO/MODS/mp/tools/mp_localpair.py --sp --level rio --seconds 30`
  for the **single-player** mini-map, and the plain (no `--sp`) run for the multiplayer
  map. Both print, under `MP_DEBUG=1`:
  `[mp] map: drew N remote blip(s)` and
  `[mp] map: flags 0x%x; first remote blip is player %d at world %d,%d`
- **Pass** — the flags name the surface (`0x22` = the multiplayer map, `0x3` = the
  single-player overhead map), and the world position **matches that player's `pose:`
  line and moves with it**. A single fixed value for every player means the module is
  transforming the position itself, which is wrong: `WorldToMultiplayerMap` returns a
  constant `(32,32)` when `MissionHeader->region == 0`, so on a single-player level
  every remote arrow lands on one wrong point and none of them show. Hand the *world*
  position and `m->flags` to `DrawPlayerDot` and let the engine place it.
- **Not a bug** — the single-player mini-map is a small window centred on the local
  car, so a remote player's arrow only appears while that player is inside it. That is
  the engine's own clipping, the same as for the local marker.

**Status** — Verified headless on both surfaces (2026-10-08): flags `0x3` with the blip
tracking the remote car's live world position on a single-player level, flags `0x22` on
the multiplayer map, both seats agreeing on the level. The reason this survived every
earlier harness run: `-mp` always selects a multiplayer region (`main.c` sets
`gBootMpLevel = 1`), so no rig reached the single-player map until the `--sp` one.

---

### 5. Input replication is the fallback only

**Invariant** — Replicated input (`MP_INPUT`) is a **snapshot-gap fallback**, never the
primary driver: a car whose owner state has arrived is driven by adoption, and the
owner's own pad always wins.

**Verify** —
- `[mp] netinput: car %d <- player %d pad %#x (fallback)`
- `[mp] netinput: car %d has no player row`
- The budget in code: `MP_INPUT_FALLBACK_FRAMES` = 8 (`mp.h`); the gate reads
  `pl->lastStateFrame`, armed on each adopted snapshot.

**Pass** — Fallback lines appear **only in gaps** of the owner's state (a handful, not a
flood), and a remote car still keeps pace — a slow link costs smoothness, never a frozen
frame nor a rubber-band. In the normal (non-`--no-debug`) run the fallback is rare
enough to be incidental.

**Status** — The mechanism and the gate are in code; confirm "not a flood" on the LAN
pass.

---

### 6. Mid-match car swap

**Invariant** — When a player changes car, every other machine re-models **its copy of
that player's car in place** to the vehicle they now drive — rebuilding the mesh — and
never warps a peer onto an unrelated car, never hands a mod-created car to the traffic
AI.

**Verify** —
- Headless levers: `MP_TEST_CARCHANGE=<secs>[,<exitSecs>]` (get out / into the nearest
  civilian), `MP_TEST_PAUSECAR=<secs>[,<city>[,<model>]][;...]` (the pause menu's
  `Apply`, including a cross-city one), `MP_TEST_CARCYCLE` (cycle every offered car).
- The re-model on each seat:
  `[mp] player %d changed car: slot %d -> %d (%s model %d), mesh rebuilt [local=%d localId=%d row=%d]`
- The invisible-car instrument (**must end `mesh loaded`**):
  `[mp] car status: driving model %d (resident %d, source %s); mesh %s`
  (`MISSING - this car is not drawn` here = the car is invisible; it must never appear.)
- The stress rig, every car as fast as it can:
  `python JERICHO/MODS/mp/tools/mp_carstress.py` (3 seats, ~700 ms between changes)
- The full cross-city pick, end to end: `MP_TEST_PAUSECAR=40,1 ...` → on the host
  `rebuilt player 0's car on slot 7 (model 2 from HAVANA)`, on the client
  `[carhacks/net] cars (from the host): 0=HAVANA model 2`.

**Pass** — N changes (cycle far enough that the spare slots *would* run out) all report
**`mesh loaded` on both seats**, the two seats agree on the new model, and none of
`no spare resident slot`, `keeping slot N`, `not loaded here`, or a `mesh MISSING` line
appears. `mp_carstress.py`'s per-seat report shows every offered car driven, 0 refused,
0 meshless. No `JERICHO.dmp`.

**Status** — Verified headless (`MP_TEST_CARCHANGE`, `MP_TEST_PAUSECAR`, `mp_carstress.py`).
The invalidation rules the two seats must obey (match the vehicle in place; a mod car
reaches the traffic AI only as a stopped, empty car) are in [`ARCHITECTURE.md` §15](ARCHITECTURE.md).

---

### 7. Car colour / palette ownership

**Invariant** — The car's **owner is the colour authority**: the palette the owner
reports is the palette every peer draws.

**Verify** —
- The palette rides the per-frame carstate: `MP_CARSTATE_ENTRY.palette` (= the owner's
  `cp->ap.palette`, what the renderer hands to `DrawCarObject`).
- `[mp] palette: player %d reports %d, drawn as %d%s`
- Use a **different palette number per test player** so a mix-up is unambiguous.

**Pass** — `reports == drawn` for every player on every seat, and the colour lands on
the peer's copy of the **right model**. (With the models agreed by domain 3, the palette
reaches the correct car.)

**Status** — Verified headless for a single guest city. The multi-guest-city palette
case is an accepted limit — see domain 15.

---

### 8. Spawn placement

**Invariant** — The module **moves no car**: every player's car starts on the ground at
the level's own start (offset along the lane), with the height the engine placed. The
module never teleports a car to a single Y — a Y taken from one car and applied at
another car's x/z leaves it above or below the ground, and that is the "cars spawn in
the air" bug.

**Verify** —
- `[mp] spawn: player %d -> slot %d at the level's own start %d,%d (+%d lane), no y`
- `[mp] gathered next to peer at %d,%d,%d (ground under us, not the peer's y)`
- Diagnostic (the module must not apply a foreign Y): `[mp] spawn y: slot %d vy=%d (local slot 0 vy=%d) surface y=%d`
- `MpPlaceSpawns` is a logged **NO-OP**; `MpOnNetSpawn` does not override the remote
  slot's position.

**Pass** — Every car's first placed frame is **already on the ground** (no fall from
y≈75 down to y≈26); a late joiner's slot is at the level's own start + lane, not the
host's x/z. No car spawns in the air; no car is dropped from a height.

**Status** — Verified headless. The single-Y teleport that caused it is removed; do not
reintroduce it.

---

### 9. Car-to-car collision handoff

**Invariant** — A contact between two player cars is **reported**; each owner gives up
the closing component on **its own** car; both cars move. No machine ever moves a car it
does not own.

**Verify** —
- The trigger is the engine's own contact (`JER_EVENT_COLLISION`), fired before either
  car's impulse is applied; the proximity probe survives only as a fallback for an
  overlap the engine never saw, and stands down 90 frames after a real contact.
- Contacts are armed only after `MP_HIT_ARM_FRAMES` (120) so the spawn drop cannot kick
  anyone.
- On one seat: `[mp] hit: we bumped player %d (%s: closing %ld, giving up %ld units/frame)`
- On the other: `[mp] hit: player %d bumped us (push %ld,%ld,%ld)`
- Duplicate suppression: `... our engine has it, kept`
- Rigs: `MP_TEST_TRAFFIC_HIT=<secs>`, `mp_localpair.py --bot pursuit`,
  `mp_localpair.py --until "hit: "`.

**Pass** — The **same contact** shows as "we bumped" on one seat and "bumped us" on the
other; the two sims stay within a few units (the adopt lines); the push **lands** (the
pre-fix bug moved a car 0.005 units/frame and read as "nothing happened"). `lost=0`,
`dumps=0`.

**Status** — Verified headless in both directions (pursuit bot).

---

### 10. Death, and respawn back into the match

**Invariant** — Falling off the world or drowning does **not** end the match: the engine
asks a module "may I end the game over?", the session refuses, and the player is put back
on the map in their car — repaired, no wanted level, controls back.

**Verify** —
- `MP_TEST_FALLOFF` (drop the car off the map).
- `[mp] death in a session: respawned at %d,%d (ground=%d) in slot %d - game over refused`
- `JERICHO: game over REFUSED by a module ...` (the engine released the lock)
- Soft restart: `MP_TEST_RESTART=<secs>`; see it with `--shots DIR`.

**Pass** — Respawn is **upright and on the ground**; the camera is re-seated (no
roll/flip); no lingering "your vehicle's wrecked"; the controls are live immediately; no
felony survives the respawn. The five symptoms to report are listed in the LAN test
checklist.

**Status** — Verified headless. (The "infinite black game-over" lock this replaces is
gone.)

---

### 11. Joining a live match (catch-up)

**Invariant** — A client that joins a session already in progress learns who drives what
and **builds** those cars, so it sees every other player's car — and its own.

**Verify** —
- Menu rig: `MP_AUTOSTART=host MP_AUTOJOIN_START=2` (waits in the frontend for a 2nd
  player — do **not** pass `-level`).
- `[mp] joined a LIVE match - pick a car`
- `[mp] late joiner: player %d -> slot %d model %d (city %d)`
- The car-ready roster flag (`MP_ROSTER_FLAG_CAR_READY`) is what makes clients visible to
  each other, not only to the host. A peer's car this machine cannot hold is reported
  once: `[mp] player %d drives %s model %d, which this machine does not hold (the h...)`
- Foreign-city evidence per try: `python JERICHO/MODS/mp/tools/mp_tries.py`

**Pass** — Every seat (host **and** each client) sees every other player's car — no
invisible peer car, and the invisible one still collides. A joiner's own pick is built,
or reported as not-held rather than silently wrong. `mp_tries.py` shows the right car on
**both** seats.

**Status** — Verified headless (same city) and via `mp_tries.py`. The former "late joiner
dropped for `timeout` while it loads" is **fixed in code** (the poll-gap credit,
`mp_net.c` — a poll gap longer than the timeout credits the time back); confirm on LAN.
Cross-city live join leans on the host's slot authority — an accepted limit, domain 16.

---

### 12. Replicated traffic

**Invariant** — Traffic and police are replicated as **owner-authoritative** cars, and
the two machines hold **disjoint `car_data` bands** — so a `car_data` **slot is the shared
identity** here (unlike a player car, which is matched by *model* precisely because a
slot means nothing for those). A contact or a dent on a traffic car follows its owner.

**Verify** —
- The band split: `[mp] traffic band: machine %d of %d owns car_data slots %d..%d`
- The population: `[mp] traffic: %d civ car(s) in our band %d..%d, nearest %d, culled %d this window`
- A mirrored re-dent: `[mp] traffic mirror slot %d re-dented (totalDamage %u)`
- A handoff: `[mp] hit: player %d bumped our traffic slot %d (push %ld,%ld,%ld)` /
  `... (our engine has it, kept)`
- The wire: `MP_TAG_TRAFFIC` (`JPTF`) and `MP_TRAFFIC_ENTRY` (`carSlot`, `model`,
  `modelCity`, `palette`, `damage[6]`, ...), with `MP_TRAFFIC_REMOVE` for a despawn.

**Pass** — Each seat holds its own band; a contact on one seat's traffic car appears on
the other; a wrecked traffic car is wrecked on both (the mirror re-dents from `damage[]`
alone, moving no vertices); `dumps=0` (no `PingInCivCar` / `CivSteerAngle` fault). The
standing rule holds: a mod-created car may reach the traffic AI **only** as a stopped,
empty car.

**Status** — Landed on this branch (`MP_PROTO_VERSION` 9). The doc drift this note used to
warn about is **fixed** (2026-10-07): `ARCHITECTURE.md` §12/§13 and `JERICHO-MP.md` §6 now
describe traffic sync as landed, not "NOT started" / "not synchronised".

---

### 13. Pause keeps the world running, and chat

**Invariant** — Opening the engine's pause menu does **not** freeze the match — the world
keeps stepping on every machine — and a chat line reaches every player.

**Verify** —
- Pause: `MP_PAUSE` (logs the pause player list); `[mp] pause menu opened; the world keeps running`;
  the module calls the exported `StepSim()` while paused (the engine skips its own), and
  `UnPauseSound()` once on the way in. Pad input is swallowed while the mp pause menu is
  open.
- Chat: `MP_TEST_CHATKEY=<secs>[,<text>]`; in game **T** opens, **Enter** sends,
  **Escape** cancels. The lever feeds the REAL handlers (`MpOnDebugKey(MP_KEY_CHAT_OPEN)`
  then `MP_KEY_CHAT_SEND`), so what it exercises is the key path, not a shortcut. It also
  types `<text>` through the character handler and then one **BACKSPACE**, so the line it
  sends is one character shorter than the argument (`…,helloo` sends `hello`).
  Evidence: `[mp] chat: prompt open (type; Enter to send, Esc to cancel)`,
  `[mp] test: chat buffer now 'hello'`, `[mp] test: chat SEND`, and then the row on the
  console of **every** seat — `[console] <name>: <text>`. (Chat goes to the status console
  since 2026-10; the old `[mp] notify row '%s'` line no longer exists — grep the `[console]`
  row instead. Measured 2026-10-08: the host logs its own echoed line and the joiner logs
  the received one, both as `[console] LocalA: hello`, with the wire frame as
  `[mp] recv JPCX len=100` on the joiner.)

**Pass** — With one player paused, the others keep moving (nobody freezes); a chat line
sent on one seat appears on the console of every other seat (`[console] <name>: <text>`).

**Status** — Verified headless (`MP_PAUSE`, `MP_TEST_CHATKEY`); the on-screen drawing by
eye at the LAN pass.

---

## Accepted limitations of v1

These are knowingly imperfect, **measured rather than assumed**, and **not release
blockers**. A release signed off against domains 1–13 may carry all of them. Each names
the thing to watch if it ever becomes a complaint.

**14. Joining a session already in progress is best-effort.** A client that joins a *running* match must have the peer cars it names built on the fly. Most of that machinery has landed — the **hotload** (geometry, cosmetics, texture pages) and the **poll-gap credit** that stopped a level load being read as a dead peer — so a same-city joiner is fine and §13's "late joiner dropped for `timeout`" no longer applies. But a cross-city joiner still leans on the host's slot authority (item 16), and a car the host's level cannot serve is **reported, not loaded**. Treat "join a live session" as best-effort; a session joined at the start is the reliable path.

**15. Two guest cities at once collide in the shared palette/page pool.** On the host, a
*second* imported city's textures and colours come out wrong while the first is right
("the vegas car imported proper but not the havana one's textures and colors"); and the
machine that imported a car gets its **scenery** textures contaminated. Both are
collisions in the shared pool/bank — not missing imports — because the bank holds three
guest-city `civ_clut` blocks (rows 8..31) and the import admits one foreign city by
default. **v1 is one guest city per session.** Details: `carhacks/MP_ADAPTER.md`.

**16. Host slot authority on a cross-city live join.** When a client's own level load
precedes a lower-id peer's pick, the two can derive different resident slots (measured:
client2 derived slot 7 where the host derived 9). The clients already receive the host's
set; **adopting it** — and moving a car that must move, with a re-hot-load — is the shape
of the fix. **Until then, a cross-city live join can mis-slot.** Same-city joins are fine.

**17. Player-car damage and dents are not synced.** `totalDamage`, `ap.damage[]` and
`needsDenting` do not ride the player-carstate, so a bent car can look straight on
another screen. Deliberate for v1 — the player carstate has no health field. (Traffic
damage **is** synced — domain 12.)

**18. Traffic motion and pedestrians are not made identical.** The disjoint-band design
means each machine drives its own traffic; a car you see need not be the same car on the
other screen. This is by design — it is *why* a slot is meaningless for a player car — and
`ARCHITECTURE.md` §12's "out of scope" (matchmaking beyond LAN, host migration) still holds.

**19. Extra-panel palette rows.** The engine gives a car set **eight** palette rows, so a
set with more texture pages borrows the first page's palette for the extra ones: vehicles
with **extra panels** — the Vegas ambulance, large SUVs, long cars — can show minor colour
corruption. Cosmetic, known. (`README_LAN_TEST.txt` says the same.) The five Driver 1
cities carry eleven to thirteen pages and so hit this hardest, but they ship as a
**separate content release**, not in this one.

**20. Doc drift — CORRECTED 2026-10-07.** `ARCHITECTURE.md` §12/§13 and `JERICHO-MP.md` §6
used to predate the traffic work and the load-timeout fix; both have now been brought up to
date (traffic sync landed, the `timeout` drop fixed, the "~65 s drop" re-described as a
freeze). Where any doc still disagrees with the code, **the code is current** — this
document tracks the code.

---

## Ship gate (sign-off)

A v1 release counts as *synchronization complete* when all of the following hold. This is
the gate the release cut runs; a domain still owed a row in the table below is a domain
still owed a run.

1. **Domains 1–13 pass on both seats** (each domain's Pass bar — read the two `JERICHO.log`
   files separately), and limitations 14–20 are reviewed and accepted.
2. **The headless smoke is green** — PASS verdict, `lost=0`, `dumps=0`, no STALLED:
   ```
   python JERICHO/MODS/mp/tools/mp_localpair.py --seconds 60 --settle 5
   python JERICHO/MODS/mp/tools/mp_carstress.py --seconds 30
   python JERICHO/MODS/mp/tools/mp_tries.py
   ```
3. **The two-machine LAN pass is green** on real hardware: both machines run the **same
   unpacked package** (`PLAY_HOST.bat` / `PLAY_JOIN.bat`), drive together, **swap cars**
   (both the pause menu's `Change car` and a real get-out / get-in), collide, die once, and
   chat. `README_LAN_TEST.txt` is the tester-facing reduction of this step.
4. **One build identity**: both machines print the **same** `build`/`mods` digest on the
   startup line and the pause scoreboard (the shipped `mp.ini` has `strict_version = 1`).
5. **No crash dumps**: `JERICHO.dmp` is absent beside the exe on both machines.

One command builds the shippable package (regenerate → build → `JERICHO_mp_lan_<build>.7z`):

```
JERICHO\MODS\mp\tools\pack_lan\sync_lan.bat
```

### Measured baseline (2026-10-07, one machine, headless)

The first recorded run of the rigs against a current build, taken to turn the
"verified headless" prose above into numbers. **Every run here had SIX modules
enabled** (`carhacks`, `crumple`, `d1cars`, `levelhacks`, `mp`, `sandbox` — boot
inventory: `6 module(s) active`), *not* the clean `mp`-only modlist this project's
own docs prescribe (`JERICHO-MP.md` §8), so these numbers are a **lower bound and
not a sign-off**. They are recorded as measured, with the confound named.

| rig | command | result |
| --- | --- | --- |
| pair, 60 s | `mp_localpair.py --seconds 60 --settle 5 --keep` | **STALLED** — host sim reached heartbeat frame 301; the joiner's sim froze at **frame 1** and its log ends mid-match on `recv JPTF len=176`; `lost=0 dumps=0` |
| pair, 60 s ×3 | same, repeated to get a rate | **2 of 3 STALLED** — one PASS (both seats heartbeat frame 1501); one joiner frozen at frame 1; one joiner frozen at frame 900 |
| crash rate | `mp_crashrate.py --runs 5` (34 s each) | **0/5 crashed, dumps=0**; the rig's own note fires — `5 run(s) produced NO PingInCivCar breadcrumbs at all` |
| car-swap stress | `mp_carstress.py --seconds 60` | **FAIL** — 3 seats, 249 changes: host 30/48, client 32/48, client1 35/48 cars driven (list not covered); `no spare resident slot` ×40 on the host plus `keeping slot`, `not loaded here`, refusals. The pair verdict itself was PASS (`lost=0 dumps=0 stopped=no`) |
| cross-city tries | `mp_tries.py --keep` | **3/3 PASS** on the pair verdict; tries 1–2 carry real cross-city identity + release/reclaim evidence on both seats; **try 3 is vacuous** (its pick was refused) |
| menu host (`--menu-host`, new) | `mp_localpair.py --menu-host --seconds 50` | **second-start check PASSES: exactly 1 launch, 3/3 runs** — but every run then **crashes the joiner** in `crumpleDeformInternal+0x2A8` (the one crash of this whole session) |
| the same crash, isolated | `mp_localpair.py --level chicago` vs `--level rio` (both `-mp 1`) | **Chicago 1/1 CRASH, rio 1/1 PASS** — within the multiplayer-region rigs, the city mattered |
| the same crash, again | `mp_localpair.py --sp --level rio` (region 0) | **CRASHED the joiner too** — so the trigger is not the city; see the correction below |

The menu-host result is worth separating from the crash. The check this rig exists
for — *does the host start a second, frontend-driven match?* — comes out **exactly
one launch in 3 of 3 runs**, so roadmap B's failure mode did not reproduce; the
`MpBeginHost` idempotency guard and the claimed frontend START hold. What the rig
surfaced instead is a **new, reproducible joiner crash**: `EXCEPTION_ACCESS_VIOLATION`
at `?crumpleDeformInternal@@YAXPEAU_CAR_DATA@@PEBF@Z+0x2A8`, always the **joiner**,
faulting immediately after mirrored traffic (`recv JPTF` / `traffic mirror slot N
re-dented`) — a `crumple`/`mp` interaction (it is `crumple`'s function, and `crumple`
is one of the six modules on). Not previously recorded anywhere.

**Correction (2026-10-08): it is not "the Chicago crash".** Re-measured while adding
the `--sp` rig, the same fault reproduced on **rio**, on a **single-player** level
(`--level rio`, no `-mp`, `subgame 0`) — and a second such run passed. So the city is
not the trigger and the crash is **intermittent**; what the earlier 4/4 really showed
is that it reproduces reliably under the menu-host/Chicago conditions *it was measured
under*. Treat it as "some levels/conditions crash the joiner in crumple", and
re-measure before attributing it. The `--menu-host` line in the doc's old "known
broken: the frontend-driven second start (**Chicago**)" note named the same city this
crash did, and the crash remains the likelier explanation for that report than a
second start — but "Chicago" should not be read as its cause.

Three caveats the numbers carry:

- **The joiner freeze is real and frequent, and it is not the documented timeout.**
  Repeated at 60 s it stalled **2 of 3** runs, at *different* points (joiner frozen at
  frame 1 in one, frame 900 in the other), and it is always the **joiner**, never the
  host. In both cases the joiner's *simulation* stops — its lockstep heartbeat stops
  advancing while the module is still polling (its log goes on receiving `JPCS`/`JPTF`)
  — and there is **no `LEAVE`, no `timeout`, no drop** (`lost=0`), so this is not the
  "late joiner dropped for `timeout`" that §11 declares fixed. No `JERICHO.dmp` is
  produced either, so it is a hang, not a crash, and `dmp_fault.py` cannot attribute it.
  A control run of **two staggered instances with no session** survived 70 s, so it is
  not simply two windows on one GPU. Root cause **not identified**.
- **`mp_crashrate.py` cannot answer its own question.** Its regexes
  (`PINGIN: enter dist=… freeSlots=(…)`) no longer match the engine, which now
  emits `JERICHO-DIAG PINGIN: slot=… cookie=…` and only under
  `JERICHO_DIAG_PINGIN=1` (`civ_ai.c:32-40,1871`), so the free-slot correlation is
  dead tool drift. It also scores `clean` from crash dumps alone, so a STALLED run
  is counted as clean — which is why its 5×34 s "0/5 crashed" says nothing about
  the freeze.
- **`mp_tries.py` PASSes on the pair verdict alone.** Try 3's whole cross-city
  request was refused (`no car 12 in HAVANA's roster (9 available)`) and the run
  still printed PASS with an empty evidence block; only `--require` makes a try
  mean something.

Consequently the sign-off cells below stay **blank**: no run here was under the
prescribed clean modlist, and no two-machine LAN pass has been recorded.

### Triage: what is a blocker, and what is an accepted limit (2026-10-07)

Classified from the baseline above. **Blocker** = the gate cannot pass until it is
resolved. **Tooling** = the instrument is wrong, not the mod — but two of these hide
real failures, so they are blockers too. Nothing here changes the accepted
limitations 14–20, which are unaffected.

**Fix-now blockers**

| # | finding | why it blocks | domain |
| --- | --- | --- | --- |
| B1 | The joiner's match **freezes** mid-play (rio), **2 of 3** at 60 s | "take a ride together" that stops after a minute is the feature failing; a freeze is invisible to a log-only PASS | 4, 12, 13 |
| B2 | A session **crashes the joiner** in `crumpleDeformInternal+0x2A8` (4/4 under the menu-host/Chicago conditions it was first measured under; intermittent, and it reproduced on single-player rio too) | it is the only crash seen, it kills the joining machine, and it is `crumple`/`mp` interaction territory | 1, 4 |
| B3 | Car-swap stress **exhausts the resident slots** (`no spare resident slot` ×40, `keeping slot`, refusals; 30/48–35/48 covered) | car swap is one of v1's two headline features | 6 |
| B4 | **No CI job runs any mp rig** | every fix is verified by hand and can silently regress — which it has (the crash "came back" once already) | all |
| B5 | The **two-machine LAN pass has never been run** | it is the only artifact that proves the scope; headless is not LAN | all |

B3 is measured on a 6-module modlist, so it must be re-measured clean before it is
fixed — the *measurement* is a blocker, the fix may not be.

**Tooling defects (each is why something above went unseen)**

| # | finding | why it matters |
| --- | --- | --- |
| T1 | `mp_crashrate.py`'s breadcrumb regexes no longer match the engine | it cannot report the free-slot correlation it exists for, and its own note says so |
| T2 | `mp_crashrate.py` scores `clean` from crash dumps alone | a **STALLED** run is counted as clean — this is how B1 stays hidden in a "0/5 crashed" line |
| T3 | `mp_tries.py` reports PASS on the pair verdict alone | its default try 3 was refused outright (`no car 12 in HAVANA's roster`) and still printed PASS with an empty evidence block |
| T4 | `mp_tries.py`'s default try 3 names a non-existent car | the documented "havana car 12" example is stale against the roster |

**Not a blocker** (measured, and it is good news)

- **The `PingInCivCar` crash class did not reproduce** — 0/5 crash-rate runs, no dumps,
  and the whole session produced exactly one crash (B2). T1 means this is *absence of
  evidence*, not proof the fix holds.
- **The frontend-driven second start (roadmap B) did not reproduce** — `--menu-host`
  gives exactly 1 launch in 3/3 runs. What the docs called "the frontend-driven second
  start (**Chicago**)" is better explained by B2: the crash, first measured on that same
  city, and no second start. §12-B and §13 have been re-worded accordingly.

**Unchanged** — accepted limitations 14–20 stand as written; nothing measured here
contradicts them.

### Sign-off table

One row per domain, one column per environment. Fill the date (and who ran it) when the
domain's Pass bar is met; a blank cell is a domain still owed a run. **v1 ships when the
table is full and 14–20 are accepted.**

| # | Domain | Headless | LAN | Notes |
| --- | --- | --- | --- | --- |
| 1 | Session admission | | | Pair PASS ×7 (2026-10-07, 6-module modlist) |
| 2 | Roster and identity | | | `mp_tries` 3/3 pair PASS; identity agreed on both seats (2026-10-07) |
| 3 | Car assignment | | | `late joiner: player 1 -> slot 1 model 2` (2026-10-07) |
| 4 | Car pose replication | | | `adopt |d|` 39→13→2 converging; frames to 751 (2026-10-07). One 60 s run STALLED |
| 5 | Input fallback only | | | |
| 6 | Mid-match car swap | | | **✗ FAIL 2026-10-07**: `mp_carstress` covered 30/48, 32/48, 35/48; `no spare resident slot` ×40; needs a clean-modlist re-run |
| 7 | Colour / palette | | | `palette: player 0 reports 5, drawn as 5` (2026-10-07) |
| 8 | Spawn placement | | | |
| 9 | Collision handoff | | | |
| 10 | Death / respawn | | | |
| 11 | Live join (catch-up) | | | `mp_tries` 1–2 cross-city join healthy on both seats; try 3 vacuous (refused pick) |
| 12 | Replicated traffic | | | `JPTF` mirrored + `traffic mirror slot 8/9 re-dented` (2026-10-07) |
| 13 | Pause / chat | | | |
| 14–20 | Accepted limitations — reviewed | n/a | n/a | |
