# What is mission-critical for mp — a critique

An assessment of what must be true for the multiplayer mod to **work in its current
scope**, and where it is not. Written 2026-10-07 against a measured baseline; the
numbers, the commands and the per-domain acceptance view live in
[`SYNC_CHECKLIST.md`](SYNC_CHECKLIST.md) (its *Measured baseline* and *Triage*
sections), and the internals in [`ARCHITECTURE.md`](ARCHITECTURE.md).

**The scope is the mod's own definition of done**, not a maximal one: a small LAN group
**takes a ride together** (the stock *Take a Ride* free-roam mode) and **swaps cars
mid-match**, for up to eight players (`SYNC_CHECKLIST.md` domains 1–13).

---

## The headline

The engineering is not the problem. Owner-authoritative car sync, an engine-contact
-driven collision handoff, replicated traffic on disjoint `car_data` bands, a per-domain
acceptance checklist — the mod is built to a much higher standard than it is **proven**
to be.

What is mission-critical here is almost entirely **evidence**, plus two real defects the
evidence turned up. Before this pass the two-machine sign-off table was empty, no CI job
ran any mp rig, and the instruments that were supposed to catch a bad run could not
(one of them counts a frozen game as clean). The mod is one honest measurement away from
being knowable, and that measurement now exists.

---

## Tier A — release-critical

Things that must be true before "it works" can be claimed. Each is measured, and each is
recorded as a blocker in `SYNC_CHECKLIST.md`'s triage.

**A1. A match does not survive.** The joiner's simulation **freezes mid-match in 2 of 3
sixty-second runs**, always on the joiner, at varying points. Its lockstep heartbeat
stops while its socket keeps polling: there is **no `LEAVE`, no `timeout`, no drop**
(`lost=0`) and **no crash dump**, so this is a hang, not a transport failure and not the
"late joiner dropped for `timeout`" the docs used to blame. Root cause unidentified. A
LAN group that "takes a ride" ends after a minute.

**A2. A session can crash the joining machine.** A match faults the joiner at
`crumpleDeformInternal+0x2A8` — 4 of 4 when first measured with `--level chicago`,
but **not Chicago-specific**: it reproduced on rio on a single-player level too, and
another such run passed, so it is **intermittent** and the city is not the trigger. It
is the only crash the entire session produced. The fault lands right after mirrored
traffic (`recv JPTF` / `traffic mirror slot N re-dented`), so an `mp`↔`crumple`
interaction is the lead.

**A3. Car swap — one of v1's two headline features — exhausts the car slots.** Under
fast cycling (3 seats, 249 changes in 60 s) no seat covered its list (30/48, 32/48,
35/48) and the host logged `no spare resident slot` **40 times**, plus `keeping slot`,
`not loaded here` and refusals. Taken on a 6-module modlist, so it must be re-measured
clean before it is fixed — but it cannot be signed off as is.

**A4. There is no automated gate.** Until this pass, CI built and packaged and ran no mp
check at all, so every fix was verified by hand and could silently regress — which it has
(a crash fix "came back" once already, in the mod's own written history). Partly closed
now: CI runs the docs and debug-independence checks, and `tools/mp_smoke.py` is a real
gate wherever the game is installed — but the two-instance smoke still cannot run on a
hosted runner (no 1.6 GB of assets, no window), so it stays a local/pre-release gate.

**A5. The two-machine LAN pass has never been run.** It is the only artifact that proves
the scope, and headless is not LAN. `SYNC_CHECKLIST.md`'s sign-off table is deliberately
still blank.

---

## Tier B — usability and trust

Things that decide whether it is usable beyond a LAN of friends. Not all are blockers;
each is a decision someone should make on purpose.

**B1. No latency smoothing, but internet play is advertised.** Roadmap G is open and
`JERICHO-MP.md` sells direct-connect internet play, while the wiki-accurate description
is *LAN-tuned*: at 100–200 ms ping the world moves in visible steps. Either the
advertisement or the expectation should change until smoothing lands.

**B2. No authentication, no encryption, and no validation of what a peer claims.**
Anyone who can reach the port can join, the traffic is plain text, and a peer's carstate
is adopted **verbatim** — the owner is trusted for its own car. Correct for a LAN; not
for a forwarded port, and the docs should say so where a player will read it.

**B3. Cross-city is the flagship car feature and is exactly where the open defects
live.** Two guest cities collide in the shared palette/page pool ("v1 is one guest city
per session"), and a cross-city live join can mis-slot because the host's mapping is not
yet authoritative. Same-city is healthy — measured: cross-city picks agree on both seats
and release their old slots cleanly.

**B4. Player-car damage is not synced.** `totalDamage`/`ap.damage[]` never leave the
machine, so a wrecked player car looks straight on a peer. The collision handoff is one
of the mod's best pieces of engineering, and this undercuts the payoff of every hit.
(Traffic damage *is* synced — the mechanism exists.)

---

## Tier C — accepted limits that must be *communicated*

These are declared scope. They are fine — **provided players are told**, and today they
are mostly buried in internals docs:

- **Eight players**, and effectively fewer: five domestic models resident, two spare
  slots and the city special per level, so eight ids get eight distinct models and no
  ninth player can be seated (`MP_MAX_PLAYERS`, `MpAssignedCarModel`).
- **One gamemode** (Take a Ride).
- **No host migration** — the host's exit ends the match for everyone.
- **One guest city per session** (the palette/page collision above).
- **Traffic motion** is replicated as *state*, not matched; ambient pedestrians are each
  machine's own.
- **A TCP session** for a real-time sim, and a mod list that must be clean when testing.

And one meta-limit worth naming: **doc drift is a mission-critical hazard on this
codebase.** The docs themselves record a wrong fix committed from stale reading; this
pass corrected a live example (traffic described as "not started" and a "drop" that was
really a freeze).

---

## The one-sentence version

The mod is built to a far higher standard than it is proven to; Tier A is now measured
rather than assumed, and it is two defects (a mid-match freeze and a joiner crash) plus
the evidence and the gate that were missing — not a shortage of synchronization code.
