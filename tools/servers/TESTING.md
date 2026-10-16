# Testing the server stack on one machine

How to exercise the master discovery server and the dedicated server **on this PC**,
what has actually been observed, and what each observation blocks. Nothing here needs
a second machine, a port-forward or an internet connection.

The deployment target (a Linux VPS, likely Docker) is a separate step — see
[The VPS footnote](#the-vps-footnote).

## The rig — `local_stack.py`

```sh
python JERICHO/MODS/mp/tools/servers/local_stack.py                    # master + dedi + 1 real client
python .../local_stack.py --clients 2 --seconds 60                     # two clients
python .../local_stack.py --clients 2 --beacon --keep                  # and leave the logs behind
```

It starts, in order:

1. the **master** (`masterserver/masterserver.py`) on `--master-port` (default 8100);
2. the **dedicated server** (`dediserver/dediserver.py`) on `--dedi-port` (default
   1400), `--no-beacon` by default so a run is deterministic, registered with the
   master unless `--no-master`;
3. **N real `JERICHO_dev.exe` clients**, one per junctioned run dir, each launched with
   a captured PID (never via `start`, never minimised) and `-join 127.0.0.1:<dedi port>`.

Then it prints, per seat, whether the client was accepted, reached the match, how far
its sim ran, and whether it left a crash dump — plus the dedi's own view (joins,
leaves, refusals) and whether the dedi unregistered cleanly.

| option | meaning |
| --- | --- |
| `--clients N` | how many real clients join (launched `--stagger`s apart) |
| `--seconds S`, `--settle S`, `--stagger S` | run length, pre-first-client wait, gap between clients |
| `--city N` | the GameLevel the dedi tells clients to load |
| `--dedi-port N`, `--dedi-config FILE` | the dedi's port, and a `dediserver.json` to hand it |
| `--master-port N`, `--master-ttl S`, `--master-interval S`, `--no-master` | the master side |
| `--beacon` | let the dedi beacon on UDP (off by default: a run stays deterministic) |
| `--game-dir DIR`, `--exe NAME` | where the build is (default `src_rebuild/bin/Release_dev`) |
| `--no-debug` | run WITHOUT `MP_DEBUG`, the way a player's launcher does |
| `--keep`, `--shots DIR` | keep the run dirs, and photograph the seats with `mpshots.py` |

Exit codes: `0` every seat connected and its sim ran, `1` a finding, `2` no game build
to run (skip, like the other rigs).

**It is a separate script on purpose.** The topology (no host seat — the dedicated
server *is* the host) is different from `mp_localpair.py`'s, so the topology lives
here. Everything that is *not* topology is imported from `mp_localpair` as a library:
the junctioned run dirs, the real-PID launch, the per-seat log reading and the
PID-scoped kill. `mp_localpair.py` is not modified.

**Teardown is PID-scoped** and the shared `bin/Release_dev/JERICHO.log` and
`JERICHO.dmp` are never written to — each client runs with its own run dir as its
CWD and logs there (`build_run_dir` skips `*.log` / `*.dmp`).

### The layer beneath it

The Python layer has its own tests and they are the first thing to run, so a red
Python layer is never mistaken for a real-client finding:

```sh
python JERICHO/MODS/mp/tools/servers/proto_check.py
python JERICHO/MODS/mp/tools/servers/masterserver/test_masterserver.py
python JERICHO/MODS/mp/tools/servers/test_e2e.py
```

All three are in CI's **Checks** job and need no game assets.

## Findings

### F1 — a real client's level ends by itself on a full city (intermittent)

**The dedi's `WELCOME` reports `mpLevel=0, arena=0`**, so a joining client launches a
**full-city** level:

```
[mp] launching: city 0 mode 1 (1=TAKEADRIVE, 0=MISSION!) subgame 0 map full-city arena 0 time 1 weather 0 players 1
```

A real pair run launches the **multiplayer map** instead (the host's `WELCOME` says
`mpLevel=1, arena=1`):

```
[mp] launching: city 0 mode 1 (1=TAKEADRIVE, 0=MISSION!) subgame 1 map MULTIPLAYER arena 1 time 1 weather 0 players 1
```

On the full city the level **sometimes ends on its own** after ~12–15 s: the game
returns to the frontend, the module leaves the session, and the process exits cleanly.

```
[jer_sound] frontend entry: 7 locked voice(s) released (0/16 held after)
[mp] conn: leaving the session | peer 127.0.0.1 | role client | stage n/a | why: deliberate
[console] [mp] leaving the session  127.0.0.1
---- LOG CLOSED ----
```

Measured over six one-client runs: **3 of 6 self-exited** (2 frontend entries,
`---- LOG CLOSED ----`, last sim frame 300), **3 of 6 stayed** until the rig killed
them (1 frontend entry, last sim frames 571/811, and one 150 s soak). The pair-run
seats never self-exit (1 frontend entry each, all runs).

**Why it matters:** it blocks the dedi from hosting a real client reliably. **Most
likely cause:** the dedi has no way to say "this is the multiplayer map" — it packs
`mpLevel=0, arena=0` into `WELCOME` unconditionally. Not verified further; this rig
reports.

### F2 — no `MP_TAG_ROSTER`: the client invents a phantom player 0

The dedi sends **no roster**. In a whole one-client run the client received exactly
three frame types:

```
12 x recv JPPO     (pong)
 1 x recv JPWL     (welcome)
 1 x recv JPST     (start)
 0 x    JPRS       (roster)   <-- never
```

Left to itself, the client mints a **player 0** row for the host slot — which does not
exist on a dedicated server — and a car for it, while its own row is player 1:

```
[mp] accepted as player 1 (matched=1 gamemode=0 city=0)
[mp] netspawn: player 0 -> slot 1 model 1 (city 0, asked -1)
[mp] pose: player 0 local=0 car 1 at 5685,30,-223340 spd=0 ct=1 pad=-1 hnd=0 vy=0 list=1/8
[mp] pose: player 1 local=1 car 0 at 6216,30,-222456 spd=0 ct=1 pad=0 hnd=0 vy=0 list=1/8
[mp] netinput: car 1 <- player 0 pad 0 (fallback)
```

Note `local=0` on player 0: the phantom is not the local player, yet it owns car 1 and
its "input" drives that car. This is the roster gap made concrete — the client has
nobody to tell it who is really in the match, so it invents the host.

#### Confirmed with two and three real clients: nobody sees anybody

The prediction above holds exactly. Each seat knows **the phantom player 0 and itself,
and no real peer**:

| seat | accepted as | rows it knows |
| --- | --- | --- |
| c1 | player 1 | `player 0 local=0 car 1`, `player 1 local=1 car 0` |
| c2 | player 2 | `player 0 local=0 car 1`, `player 2 local=1 car 0` |
| c3 | player 3 | `player 0 local=0 car 1`, `player 3 local=1 car 0` |

Every seat independently invents the *same* phantom player 0 with car 1.

**The relay itself works** — this is not a car-state problem. The dedi forwards the
union of everyone's state and the framing scales with the player count, which the
client's own receive log shows by the frame length (8-byte header + 53 bytes/entry):

```
recv JPCS len=61    8 + 1 x 53   (one player)
recv JPCS len=114   8 + 2 x 53   (two)
recv JPCS len=167   8 + 3 x 53   (three)
```

So each client *receives* the other players' car state and **silently discards it**
for want of a roster row — no "unknown player" complaint, nothing on screen. The
dedi's relay is sound; what is missing is the roster that tells the client who is in
the match.

**Why it matters:** this is the blocker for the whole point of a dedicated server.
Two players can join, both reach the level, both sims run, both links are clean — and
they still cannot see each other.

### F3 — a refused client is told nothing

The dedi refuses correctly — with `--max-players 2` the second client is turned away
and the dedi logs it:

```
[WARNING] refused 127.0.0.1:65331: server is full (1/2)
```

But the **client is told nothing**. Its own log shows a dropped socket and not one word
about why:

```
[mp] peer 127.0.0.1:1406 dropped (socket closed (bad handle)); 0 byte(s) received
```

Zero bytes: the `REJECT` frame the dedi sends did not survive the close.

**The game's own host already solved this**, and the dedi does not do what it does:

* the host half-closes — `shutdown(..., MP_SHUT_WR)` (`mp_net.c:518`, `mp_net.c:1607`),
  with the reasoning at `mp_net.c:496`: *"The fix is a proper half-close: send, push a
  FIN with shutdown(SD_SEND), then..."*;
* `docs/ARCHITECTURE.md:110` states the contract: *"A rejection is delivered before the
  socket closes, and the peer is half-closed (`shutdown(SD_SEND)`) rather than dropped,
  so the reason survives. Losing it was a real bug: a rejected client used to see
  'Connection to the server lost!' and nothing else."*
* the dedi sends the `REJECT` and then `conn.close()`s immediately
  (`dediserver.py`, the full-server branch) — no half-close.

**Why it matters:** *"Connection to the server lost!"* instead of *"server is full"* is
exactly the bug the game side already fixed for itself, reintroduced on the server.
It is a one-line-shaped fix, but it is a fix, so it is reported not made.

**A contrast that confirms the reading:** kill the dedi under a *connected* client and
that client does receive bytes — `peer ... dropped (socket closed (bad handle)); 22470
byte(s) received` — the difference is that a live session has sent plenty before the
close, where a refusal has sent one small frame.

### F*) — a minor one for the record

Any TCP connection to the dedi that is not a valid handshake is logged as
`[WARNING] <ip>:<port>: bad handshake (tag None), dropping`. A port scan or a
health-check probe therefore shows up in the dedi's log. The rig itself no longer
causes this (it waits on the dedi's own "listening" line), but an external monitor
would.

## Discovery — what is proven, and what is not

### The master (verified end to end)

The dedi registers at start, its **live** player count tracks the session, and a clean
stop removes it at once. The rig prints the listing mid-run, while clients are
connected:

```json
{"name": "LocalStack", "host": "127.0.0.1", "port": 1400, "players": 2,
 "maxPlayers": 8, "city": 0, "gamemode": 0, "inProgress": 1,
 "protoVersion": 11, "sdkVersion": 2, "ageSeconds": 3.2}
```

and, from the dedi's own log, `registered with master ... as 127.0.0.1:1400` on the
way up and `unregistered from master` on the way down.

### The LAN beacon (verified)

With `--beacon`, the dedi's UDP advertisement is real — `mp_test.py beacon` receives
it (9 beacons in 4 s), carrying the same identity the listing shows:

```
[beacon] from 172.18.208.1: proto=11 port=1401 host='LocalStack' players=0/8 mode=0 city=0 enforced=0 inProgress=1 modHash=0x0000
```

### The in-game Join browser is **not** headlessly drivable

The module's only frontend-menu test lever is `MP_TEST_MANUALADDR` (`mp_ui.c`), so a
run can fill the manual address field but cannot open the browser and read its rows.
That half stays a **by-hand check** (host with `--beacon`, open Join, look).

What *is* verified headlessly is the path a player would use without a browser row —
typing the address — and it works against the dedi:

```
[mp] test: manual address <- '127.0.0.1:1400' (MP_TEST_MANUALADDR)
[mp] connecting to 127.0.0.1:1400
```

(`local_stack.py --join-manual` drives exactly this.)

### The boundary: internet listing does not exist yet

`--master` proves **registration**, not in-game internet browsing. A client cannot
list remote games from inside the game until the C-side work in
[`docs/MASTER_SERVER.md`](../../docs/MASTER_SERVER.md) lands — so "internet discovery"
is at present: hosts register (any process), and clients join by IP:port. Nothing in
the rig above should be read as having tested more than that.

## Ranking — what each finding blocks

| # | finding | what it blocks | severity |
| --- | --- | --- | --- |
| **F2** | no `MP_TAG_ROSTER` → no client sees any other player | the entire point of a dedicated server; therefore the LAN step | **BLOCKER** |
| **F1** | `mpLevel=0`/`arena=0` → a full city, on which the level self-ends (3 of 6 runs) | a stable session for even one real client | **BLOCKER** (intermittent) |
| **F3** | a refused client is told nothing (no half-close) | legibility of a full/refused join | worth fixing |
| **F\*** | a bare TCP connect is logged as `bad handshake` | nothing gameplay; log noise for an external monitor | cosmetic |

**Three checks the plan named, and why they could not be measured yet:**

* **"a peer car moves"** — the peer's car state *does* arrive (F2 shows frames carrying
  every player), but with no roster row there is nothing to render it against, so peer
  motion cannot be judged at all until a roster exists. Not "it does not move"; "there
  is no row to move".
* **"chat relays"** — not exercised: chat needs a keyboard, and a headless run has
  none (the mp tools have a lever for this, `mp_join.bat`/the chat flags, but it was
  not part of these runs).
* **"a leave is observed"** — the **dedi** observes leaves and logs them
  (`player 2 'Seat2' left (1/8)`), and its player count tracks them (3/8 → 2/8 → 1/8).
  The remaining **clients** cannot observe a peer leaving, for the same reason they
  cannot observe one arriving: no roster.

## Stability

| what | result |
| --- | --- |
| **join/leave cycling** — 4 cycles of 2 seats against one long-lived dedi | every cycle PASS; each cycle the seats get **players 1 and 2 again** (slot reuse) and the count returns to `(0/8)`; RSS 24,190,976 → 24,248,320 bytes across all four (allocator noise) and the thread count returns to 1, so no connection thread survives a cycle |
| **a full server** — `--max-players 2` | the extra client is refused and the dedi logs `refused ...: server is full (1/2)`; what the *client* is told is F3 |
| **a client killed mid-session** (`taskkill`) | the dedi frees the slot at once (`player 1 'Seat1' left (1/8)`) and the surviving client runs on untouched: `last_sim_frame=1111 lost=0 zero_byte=0` |
| **the dedi killed under live clients** | both clients lose it cleanly and identically — `[error] Lost the server 127.0.0.1:1409 (in the match)`, `returning to the frontend`, a HUD notice, **no crash and no hang**. Nothing reconnects: the restarted dedi logs no joins, so a restarted server is not picked back up |
| **a 150 s soak**, 2 clients | clean — exactly 2 joins and 2 leaves, no dumps, no stall, sims ~4,400–4,800 frames, the master listing live throughout (`players: 2, inProgress: 1`), unregistered on stop |

The master's own restart path (the dedi noticing a 404 and re-registering) belongs to the
Python layer and is covered there by `test_e2e.py`, not by hand here.

## Still to test

* **The in-game Join browser** — by hand (it has no headless lever).
* **The ladder** — this PC → LAN (second PC, `mp_agent`) → internet discovery (needs
  the C-side work) → playtesting.
* **The rejected-client reason**, once F3 is fixed: the same `--max-players 2` run
  should then show the client *why* it was refused.

## The ladder — where this is, and what advances it

The plan is deliberately staged, and two of the findings say plainly where the next
step's gate is:

| step | what it means | gate to enter it |
| --- | --- | --- |
| **0. This PC** *(done)* | master + dedi + real clients on loopback; the Python floor in CI | — |
| **1. LAN — the second PC** | a real remote client over the network, and the dedi in the in-game Join browser | **F1 and F2 fixed.** Without them a LAN run can only reproduce the same two failures: the client's level ending under it, and no client seeing any other. The rig for this step already exists — `mp_agent`/`mp_remote.py` drive the second PC, and `local_stack.py --external-dedi` points clients at one dedi |
| **2. Internet discovery** | hosts register with the master and clients list remote games **from inside the game** | the C-side work in [`docs/MASTER_SERVER.md`](../../docs/MASTER_SERVER.md) (`master_url`, register/heartbeat, a remote-row array, a non-blocking HTTP fetch). The **server** half is already proven (§Discovery): registration, live player counts, the beacon, and unregister-on-stop |
| **3. Playtesting** | real players, and then bugs and discovery issues | a session that survives step 1 and a listing that works at step 2 |

Two things carry forward unchanged at every step: the Python floor (it runs in CI and
needs no assets) and this rig's shape — one machine can always stand up the whole stack
and read every layer's log.

## The VPS footnote

Everything above is one PC. The intended deployment is a Linux VPS, likely Docker. What
is already true, and what will need re-checking there:

* **Ports to publish** — the dedi's TCP session port (`1318`) and the master's HTTP port
  (`8100`). UDP `1318` is only for LAN browser discovery and is pointless on an
  internet-facing server. Both servers already take a configurable bind address.
* **Two containers, no build step.** Each is one stdlib-only Python file with a JSON
  config beside it and no game assets: a base image, the file, a mounted config, a
  `CMD`. The only state is the master's `--persist` file and the logs.
* **Signals.** Unregister-on-stop is verified here under Ctrl-Break and SIGTERM; under
  Docker, re-check that `docker stop`'s SIGTERM actually reaches the process (it must be
  PID 1, or run under something that forwards it). Otherwise a stopped dedi leaves a
  stale entry until the TTL expires.
* **The public address — a known gap.** The master derives an entry's address from the
  **TCP peer**, which is right when the master sees the real client. It also accepts an
  explicit `advertiseHost` (`masterserver.py:180`, `:409`) — but **the dedi never sends
  one** (`grep advertiseHost dediserver/dediserver.py` → nothing). So a dedi behind
  NAT/reverse-proxy on the VPS will register whatever address the master saw, which is
  the wrong one if the master is on the same host. Either give the containers host
  networking, or have the dedi send `advertiseHost` — a small change, deliberately not
  made here.
* **What cannot be tested there.** The game half. F1 and F2 are client-side consequences
  of what the server sends, so they reproduce identically on a VPS, and the 1.6 GB of
  game data does not belong on one — the VPS proves the servers, this PC proves the
  interaction.
