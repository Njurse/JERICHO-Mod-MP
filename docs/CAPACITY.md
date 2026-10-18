# Capacity: how many players, and where a car lives

A session holds **16 players** (host + 15 clients). This is the map of the car
pool, why it is shaped that way, and what each limit costs when you raise it.

## The slot map

`MAX_CARS` is **32** (`src_rebuild/Game/dr2limits.h`), split into three named
ranges that nothing is allowed to grow into by accident:

```
car_data slot   0 ......... 15   16 ...... 19   20 ............... 31
                players           JERICHO-      traffic (civilians
                (one per id)      managed spare and police)
```

| Range | Constant | Size | Owner |
|---|---|---|---|
| players | `JER_SLOT_PLAYERS_FIRST` | `MAX_PLAYERS` (16) | one `CAR_DATA` per mp player id; id == slot |
| managed | `JER_SLOT_MANAGED_FIRST` | `JER_SLOT_MANAGED_COUNT` (4) | anything else JERICHO-managed: imported/cross-city cars, a module's own spawns |
| traffic | `JER_SLOT_TRAFFIC_FIRST` | `JER_SLOT_TRAFFIC_COUNT` (12) | civilians + police, replicated per slot |

`#error` checks in `dr2limits.h` fail the build if the three ranges stop adding up,
if `MAX_PLAYERS` no longer fits below the traffic pool, or if `MAX_CARS` exceeds 32
(the collision bitfield's width).

**Why the traffic pool is a fixed 12.** It was `MAX_CARS - MP_MAX_PLAYERS` = 12 at
20 cars and 8 players, i.e. it *shrank as players joined*: a full lobby ate the
map's civilians. The pool is now independent of the lobby, so a map's population
is the same at 2 players and at 16. The extra capacity went to players and the
managed pool instead. That is the whole point of the split: **more players, same
traffic**.

**Why only traffic needs a shared slot number.** A player car is replicated by
MODEL (a slot number means a different car on each machine). Civilians are
replicated by SLOT — the disjoint band per machine makes "slot 23" the same car
everywhere, and that is the only identity that can be agreed ahead of any state
being sent. So the traffic range is the one region that must never be used for
anything else.

## The limits this change touched, and what each one costs

| Limit | Was | Now | Why it had to move |
|---|---|---|---|
| `MAX_CARS` | 20 | 32 | 16 players + a 12-slot traffic pool is 28; the surplus is the managed pool |
| `hd.mayBeColliding` | `int` | `unsigned int` | it is a 32-bit car-index bitfield, so the highest pair is bit 31 — a signed int must not shift into its sign bit |
| the "always precise" marker | `1 << 31` written into that bitfield | `cp->hndType != 0` read directly | at 32 cars bit 31 is a REAL car, so the sentinel became a phantom collision pair |
| `carsOnBoat` | `int` | `unsigned long long` | its bits are car slots plus `TANNER_COLLIDER_CARID` above them; that no longer fits in 32 bits (`1 << 32` is UB — MSVC C4293) |
| `MAX_CAR_RESIDENT_MODELS` | 12 | **12 (raise to 16 deferred)** | 16 players sharing a 12-model pool means some drive duplicates. The raise was tried and pulled back: it landed in the same breath as this capacity work and cross-city imports started showing palette/texture faults, so it needs its own VRAM measurement first |
| `SPECIAL_CAR_SLOT` | `MAX_CAR_RESIDENT_MODELS-1` | pinned `11` | it is a MODEL number compared against `cp->ap.model` (Vegas ambulance, mission 33's limo). Pinning it is what makes a future resident-pool raise safe — otherwise the pool growing silently re-points those mission checks |
| `MP_MAX_PLAYERS` | 8 | 16 | the lobby cap itself; also the engine's `MAX_PLAYERS` on PC |
| `MP_PROTO_VERSION` | 11 | 12 | `MP_ROSTER` is sized by the player count and memcpy'd whole, so an 8-player build would overrun its buffer on a 16-player roster |

Spare resident model slots stay `-1` = "no model" — a module claims one and gets the
same deal stock levels get: nothing loaded, nothing to undo.

## What breaks at 16, and what was done about it

- **Traffic bands collide once machines outnumber slots.** `MpTrafficBandFor` used
  to clamp every surplus machine onto the pool's last slot, so two machines
  spawned into one slot and mirrored each other's car into it. A machine with no
  private slot now gets an EMPTY band (`first = last = -1`): it spawns no traffic
  of its own and lives off its peers' mirrors.
- **Spawn placement is a grid, not a line.** The engine's own "player 2 at +600 in
  x" step, applied per id, put player 15 at +9000 — past the edge of a multiplayer
  arena, where placement resolves the ground to nothing and the car drops into the
  void. Spawns now fill a 4-wide grid (600 apart in x and z, ~2.4k units across);
  id 0 keeps the level's start untouched. The rule "no y override, let placement
  resolve the ground" is unchanged.
- **The lobby list is paged.** The frontend menu holds `JER_FE_MAX_ITEMS` (12) rows
  and the pinned ones (status / Start Match / Leave) take three, so the old loop
  stopped at the ninth player and 10..16 were invisible. A `More players (n/m)`
  row turns the page.
- **A session cannot fast-forward.** BACKSPACE sets the engine's debug `FastForward`,
  and a fast-forwarded sim runs at a different rate from every other machine's — a
  desync by definition. mp clears the flag every frame while a match is running, so
  the key stays useful in single player and is inert online.
- **A 17th player is told why.** `MpAllocPlayerId` has no id left, so the HELLO
  handler answers `MP_REJECT_FULL` / "server is full" and shuts the connection
  down. (This path already existed at 8; it now covers 16.)
- **Bandwidth grows, and stays fine on a LAN.** Each machine sends its own car
  state, not a relayed world, so the per-machine cost is linear; the host's input
  relay is the only N² term and is ~4 bytes per player per frame.

## Not raised, deliberately

- **`MAX_PEDESTRIANS` (28).** Sixteen players on foot plus ambient pedestrians is
  the next ceiling, but it needs sixteen people out of their cars at once in one
  place to notice. Left alone; raise it with the same reasoning if it bites.
- **`MAX_PLAYERS`.** 16 on PC is the engine's own table (`ReplayStreams` /
  `player[]`), and mp ids map onto it 1:1. Going past 16 is an engine rework, not
  a constant.
- **The resident model pool (12).** See the table above: sixteen players can want
  more distinct cars than the pool holds, and the answer for now is duplicates.
  Raising it is a car page + CLUT budget question (`MP/docs/CAPACITY.md` is not the
  place for that measurement; carhacks' `docs/VRAM.md` and `PALETTES.md` are).
- **The traffic pool size.** Deliberately unchanged (see above). A bigger pool
  would be a density change for every existing session.

## Verifying a capacity change

```
# stock single-player: the resident map and the VRAM budget
JERICHO_dev.exe -level chicago          ->  grep "residents=" JERICHO.log
                                        ->  grep "JERICHO-VRAM" JERICHO.log

# a session (traffic bands, spawn contract)
JERICHO_dev.exe -level chicago -mp 0 -host

# the mp release gate
python JERICHO/MODS/mp/tools/run_gate.py
```

The boot line names the pool in order, so a slot that changed meaning is visible
without a debugger:

```
residents= 1 2 3 0 4 -1 -1 -1 -1 -1 -1 10 -1 -1 -1 -1
                                          ^^ special (11)
                                             ^^^^^^^^ spare, "no model"
```
