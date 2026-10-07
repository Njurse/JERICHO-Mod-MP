# JERICHO MP — how multiplayer works

A plain-language tour of the multiplayer mod, written so a reader who has never
opened `mp_session.c` can follow a match from "press Host" to "four cars driving
around". The deep version — every field, every trap, every decision — is
`ARCHITECTURE.md` next to this file. Start here.

---

## 1. The idea in one paragraph

Everyone plays on their own machine, with their own screen and their own car. The
engine is single-player code that has no concept of a network, so the mod does not
try to make it multiplayer: **it makes every machine simulate the same match, and
keeps the few things that differ in step.** Each machine runs the whole world —
traffic, peds, the map — on its own. The only things that travel are the things a
human decides or does: which car each player drives, where each player's car is,
what input they are holding, and which of their cars has just been hit.

That is the whole design. Everything below is a consequence of it.

---

## 2. What a player does

1. **Host** — the frontend gets a multiplayer menu (`-host` does the same). The
   host opens a TCP listener on port 1318 and starts advertising a UDP beacon, so
   the game appears in everyone else's LAN list.
2. **Join** — pick a game from the list (or `-join <ip>`). The client connects,
   says hello, and gets a car and a player number.
3. **Choose the match** — the host picks the city, the multiplayer map, gamemode,
   weather and time of day. Those travel in the handshake, so everyone loads the
   same place. Nobody else has to configure anything.
4. **Drive.** Each player sees the others' cars moving, and their own car responds
   to their own pad with no delay. Collisions push both cars.
5. **Talk.** Press `T` to open a chat line, type, and press `Enter` to send it to
   everyone (Escape cancels). Every player sees the line at once.

---

## 3. Who owns what (the one rule that explains the rest)

Every car belongs to exactly one machine: **the machine of the player driving it**.

- The owner sends *its* car every frame — position, heading, orientation, both
  velocities, its model, its colour.
- Everyone else adopts that state verbatim for that car.

This is called owner-authoritative, and it is why a remote car never rubber-bands
against its own driver: the only thing that can move your car is you. It is also why
a collision has to be reported (`MP_TAG_HIT`) rather than computed: if my car touches
yours, my machine can only move *mine*, so it pushes mine and tells your machine to
push yours. Both cars move, each from its owner.

The **host is the hub**. Clients send to the host; the host fans the messages out to
the other clients. A client never talks to another client directly, so a client
knows one address (the host's) and the host knows all of them.

---

## 4. How a match comes up

```
   client                          host
     |  HELLO  (name, build, mods)  |
     | ---------------------------> |   version/mods checked, a player id is
     |                              |   assigned (1..7; 0 is the host)
     |   WELCOME (your id, car      |
     |            hint, level)      |
     | <--------------------------- |
     |                              |
     |   both load the SAME level now, from the session's city/map
     |                              |
     |   START / roster / spawn     |
     | <--------------------------- |
     |                              |
     |   per-frame: input, car state, peds, hits, colours
```

Two things are worth noticing.

**A player who joins a match in progress is a different code path.** At level load
the engine creates the player cars itself, from a start record per player slot. A
late joiner arrives when that has already happened, so the module has to seat it:
`MpSpawnLateJoiners` builds a start record in the same shape the engine uses — the
level's own start point, one lane per player, **x/z only, no y** — and creates the
car through the engine's own `InitPlayer`. See §4 of `ARCHITECTURE.md` for why a y
must never be carried from one car to another.

**Everybody must agree on the level before anyone loads it.** The city, the
multiplayer map, the gamemode and the weather are decided once, by the host, and
travel in the handshake — a machine that loads a different level cannot see the
other players' cars, and the failure looks like a desync rather than a mismatch.

---

## 5. The cars themselves

**One player per machine.** `NumPlayers` stays 1 locally: the stock renderer splits
the screen when it is 2, and everyone is supposed to have their own screen. Remote
players are therefore not "player 2" to the engine — they are extra cars with
negative pad ids, the same trick the engine already uses for cars nobody drives.

**Each player gets a car of their own, and every machine agrees which.** A chosen
car (`-mpcar`, or the frontend's picker) travels in the handshake as a city AND a
model number (`-mpcar [city:]model`, so a cross-city car is expressible). A player
who chose nothing gets the model its **player id** is assigned, computed identically
on every machine from the same table, so no two players get the same car. That table
is bounded by the level: each city has five domestic models resident, two spare slots,
and its own special in slot 7 — which is why a ninth player does not exist and why
the assignment names a model, never a "slot 3".

**Colour** rides the same per-frame car state, so the owner is the authority on its
own car's paint. Each player's **character** (the on-foot Tanner) is the same idea:
a player's preferred colour travels per player and their stand-in is painted with
it. mp paints it as a *default* — a mod that sets a character's colour (factions,
teams, whatever claims the ped) overrides it, because mp's ped hook runs first and
only fills in what no mod coloured.

**Getting out** is a separate (and younger) path: the car is left standing where it
was and a pedestrian stands in for the player. It is the least finished part of the
mod.

---

## 6. What is deliberately not synchronised

> ⚠ Traffic sync has since **landed** (owner-authoritative, disjoint `car_data` bands —
> `MP_TAG_TRAFFIC`). See [`SYNC_CHECKLIST.md`](SYNC_CHECKLIST.md); the paragraph below
> predates it.

- **Traffic and pedestrians.** Every machine runs its own; they will not match, and
  nothing tries to make them. This is why a car slot number means nothing on another
  machine (it may hold a different car there), and why matching a *model* by number
  is the only safe way to talk about "the car they are driving".
- **Dents and damage.** Nothing about a car's condition is synced: `totalDamage`,
  `ap.damage[]` and `needsDenting` never travel, so a wreck can look different on
  two screens. (There is no health field on the wire — see `MP_CARSTATE_ENTRY`.)

---

## 7. Things that cost days (read this before debugging)

- **A model number and a resident slot index are not the same thing.** `cp->ap.model`
  is a *slot* (0..7) into the level's table; the car's *model number* is what the
  table holds (`residentCarModels[slot]`). The wire therefore carries a (city, model)
  pair and never a slot -- a slot means a different car on another machine. The
  receiver resolves it with `MpResidentSlotForCar` (model + `GetCarModelSourceCity`),
  and a car it cannot hold is kept as-is and reported once.
- **A runtime model change needs the engine's car setup**, not just `ap.model = m`.
  `CreateDentableCar` is the only writer of the vertex array the renderer draws with,
  so a bare model write draws one car's polygons against another car's vertices.
  `MpAdoptRemoteCar` now does both.
- **Sockets must be non-blocking.** A blocking `send()` inside the game loop freezes
  the match for as long as it blocks, and the peer then times you out.
- **A blocked frame is not silence.** Counting wall-clock time since the last packet
  punishes a peer that is inside a long blocking level load; the load is not evidence
  that the peer is gone.
- **The log lies unless it is flushed.** The engine's log is a buffered file that is
  only flushed on specific events, and a live reader (the harness) sees whatever
  reached the disk. `JERICHO_LOG_FLUSH=1` flushes per line; without it a line written
  just before a hang is invisible, and a frozen run looks like a quiet one.
- **`JerNpc*` is a `PEDESTRIAN*` in disguise.** `jer_npc_spawn_model` returns the
  pedestrian cast to a one-field phantom struct (`{ void* ped; }`); dereferencing it
  reads the ped's `pNext`, not the ped. Compare the stored pointer directly.
- **A spawned ped's `padId` is uninitialised.** A pooled slot keeps whatever the
  previous occupant held, so "is this ped mine?" must not be answered by a `padId`
  scan — a stand-in can match a real player's id and take their colour (or refuse to
  stand in at all). Stamp stand-ins `padId = -1` and identify the local ped by
  `player[0].pPed`.

---

## 8. People and their things

| what | where |
| --- | --- |
| the mod | `JERICHO/MODS/mp/` — `mp.c` (hooks/registry), `mp_net.c` (transport), `mp_session.c` (session + sync), `mp_ui.c` (frontend), `mp_players.c`, `mp_bot.c` (test bot) |
| config | `JERICHO/CONFIG/mp.ini` (port, name, version strictness, colour) |
| command line | `-host [port]`, `-join <ip>[:port]`, `-mpcar [city:]<model\|slotN>` |
| how to run it | `README.md` in this folder, and `tools/README.md` for every harness |
| how it works, deeply | `ARCHITECTURE.md` |
| testing two or four machines | `tools/mp_localpair.py --players N` (one PC), `tools/remote/` (a second PC) |

**When testing, turn the other mods off.** `JERICHO/CONFIG/modlist.ini` should have
`mp = 1` and everything else `0`: cainescrossfire and friends rewrite car handling,
and a run with them on is not a run of this mod. Check the boot log's module
inventory — it names every module and whether the modlist or a default enabled it.

---

## 9. Who can join whom

Every join is checked by the **host**, on its side, in this order:

| check | when | what it means |
| --- | --- | --- |
| `protoVersion` | always | the two builds speak the same wire protocol (`MP_PROTO_VERSION`) |
| `sdkVersion` | always | same JERICHO SDK |
| enabled-mod manifest | per `mod_check` in `mp.ini` | `OFF` admits anyone, `VERSION` wants the same enabled mod ids, `EXACT` their versions too |
| build identity | only with `Strict Version` on — **default off** | same **release series** |

The build identity compares the **series** of `JERICHO_BUILD_VERSION`, not the
string. That string is `git describe --tags --always --dirty`, so one release
reads differently depending on who built it and how — and comparing it raw
refused every pair that was not identical in provenance, which is every
cross-platform pair and every pair where one side built the game locally:

| what a build reports | series | joins a `0.9.0` host under strict? |
| --- | --- | --- |
| `0.9.0` — the tagged release | `0.9.0` | yes |
| `v0.9.0` — same, before the leading v is stripped | `0.9.0` | yes |
| `0.9.0-dirty` — built on a machine whose tree looked dirty | `0.9.0` | yes |
| `0.9.0-3-gabc1234` — a tree three commits past the tag | `0.9.0` | yes |
| `0.9.1` | `0.9.1` | no — different release |
| `alpha-2-g26b6fa4a` — the rolling prerelease | itself | no |

A string with **no version in front is left whole**, so strict stays as strict as
it can be when there is no release to be strict about: the rolling prerelease's
`alpha` tag, and a tree with no tags fetched at all, must still match exactly.

**Only the host applies this**, so the change is a loosening rather than a new
handshake: a host running an older build still hashes the raw string and keeps
refusing its own series, and no `MP_PROTO_VERSION` bump was needed. A Windows
release and a Linux release of one version join under strict; for a mixed pair,
at least the host has to be running a build that knows what a series is.

