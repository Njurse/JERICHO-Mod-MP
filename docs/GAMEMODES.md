# Game modes

Status: **Phase POC, two timer modes.** There are three modes: Take a Ride (id 0),
which plays exactly as it did before the framework existed; Fox N Hounds (id 1);
and Car Bomb Tag (id 2). Phase 1 added the round state every mode reads, on the
wire as `JPRD` (protocol 22). Phase POC adds the marked player and the clocks, on
the wire as `JPMK` (protocol 23, mp module 0.20.0), and uses two core APIs from
JERICHO 0.13.11+: `jer_arrow_on_car` and `jer_music_force` (core PR #94). mp module
0.21.0 adds two countdowns to both timer modes (still protocol 23): a pre-game
lobby count, 5, 4, 3, 2, 1, once after each map load; then, before every round,
the round start: wait for every car, a start cluster on a road, then GET READY,
3, 2, 1, GO. 0.22.0 is presentation only (still protocol 23): a 3D bomb tick
that speeds up as the clock runs down, a wreck on the bomb car when the clock
ends, a cream/grey/flashing-red clock at the top right, a persistent "You're
the fox!" centre line, and a glowing body shell on the fox (player colour, or
cyan) and on the bomb car (always red). 0.22.1 (still protocol 23) folds the
pre-game wait and countdown into the framework: a mode opts in with four fields
on its `MP_MODE_DESC` row; Fox N Hounds and Car Bomb Tag opt in, Take a Ride
stays off; waiting-for-players and the old pre-game lobby are one WAITING
stage, then one COUNTDOWN; a join before GO is not a mid-round join. 0.22.2
(still protocol 23) fires a real respawn the moment WAITING is satisfied and
COUNTDOWN begins: every player is repaired and unlocked through the round-reset
path, the bomb wreck latch is cleared, and the start cluster is placed then,
not during waiting and not mid-count. 0.22.3 (still protocol 23) plays the
bomb beep without doppler when you hold it (`StartSound`, no 3D velocity)
and halves the beep pace (1 s to 400 ms approaching 10 s, then 200 ms, then
every 4 frames under 5 s). The fox/bomb holo is a slightly scaled low-LOD
body (NewLowCarModel vlist + those polys), not the hi-LOD dent dump fed
to low-LOD indices. Everything below "Not yet" is design, not code.

## The model

A game mode is a row in one table, `kModes[]` in `mp_mode.c`. The row is an
`MP_MODE_DESC` (`mp_mode.h`):

| field | meaning | Take a Ride |
| --- | --- | --- |
| `id` | the session's `gamemode` byte, `MP_GAMEMODE_*` (`mp_proto.h`) | 0 |
| `name` | what the lobby and every browser show | `TAKE A RIDE` |
| `selectable` | a host may pick it on the Host page | 1 |
| `teamCount`, `teams[4]` | 0 = free for all, else 1..4 teams, each with a name and an r,g,b | 0 |
| `teamColours` | name tag, suit and chat ink forced to the team colour | 0 |
| `respawn`, `respawnDelayFrames` | `MP_MODE_RESPAWN_INSTANT` / `_DELAY` / `_NONE` | instant |
| `lateJoin` | `MP_MODE_LATEJOIN_LIVE` / `_SPECTATE` | live |
| `waitMinPlayers` | sit in WAITING until this many players are present (0 = off) | 0 |
| `waitForCars` | 1 = also wait until every present player has picked a car | 0 |
| `firstCountdownSec` | once per map load, after the wait, an N..1 count then GO (0 = skip) | 0 |
| `roundCountdownSec` | later rounds: a 1 s GET READY then an N..1 count then GO (0 = skip) | 0 |
| `carChangeLocked` | no car change while a round runs | 0 |
| `carPool` | car-pool filter id, `MP_MODE_CARPOOL_*` | `ALL` |
| `spawn` | spawn rule, `MP_MODE_SPAWN_SESSION` / `_CLUSTER` (seated as SESSION, then a start cluster each round) | `SESSION` |
| `mapCycle` | the lobby may cycle maps | 0 |
| `timer` | `MP_MODE_TIMER_NONE` / `_PERSONAL` (a clock per player) / `_SHARED` (one clock) | none |
| `music` | `MP_MODE_MUSIC_AUTO` / `_CALM` / `_PURSUIT` while a round is live (= `JER_MUSIC_*`) | auto |

The rows today:

| id | name | timer | music | wait / first / later | spawn |
| --- | --- | --- | --- | --- | --- |
| 0 | `TAKE A RIDE` | none | auto | off (`0, 0, 0, 0`) | `SESSION` |
| 1 | `FOX N HOUNDS` | personal | pursuit | `2, 1, 5, 3` | `CLUSTER` |
| 2 | `CAR BOMB TAG` | shared | pursuit | `2, 1, 5, 3` | `CLUSTER` |

All three are selectable. Apart from `timer`, `music`, the four pre-game fields
and `spawn`, modes 1 and 2 take every Take a Ride answer: instant respawn, live
late join, no teams, no car pool.

To give a new mode the same wait-and-countdown, copy those four numbers onto its
row in `kModes[]` (`mp_mode.c`). Leave them `0, 0, 0, 0` for Take-a-Ride
behaviour (no wait, no count). The accessors are `MpModeWaitMinPlayers`,
`MpModeWaitForCars`, `MpModeFirstCountdownSec`, `MpModeRoundCountdownSec`,
`MpModeHasPreGame` and `MpModeWantsCountdown` (the last is 1 when either
countdown field is set). The referee (`mp_mark.c`, and `dedi/markref.py` on a
dedicated server) reads them; nothing else has to be wired.

The session never reads a field. It asks a question at a fixed **decision point**
and passes `gMp.gamemode`:

| decision point | question | mode 0 answer |
| --- | --- | --- |
| `MpRespawnAfterDeath` (mp_session.c) | `MpModeRespawnPolicy` | instant: the existing respawn |
| `MpChangeCar` | `MpModeCarChangeRefusal` | NULL: never refused |
| `MpCarListForCity` | `MpModeFilterCarPool` | the list, untouched |
| `MpNameTagColour` (mp_ui.c), `MpOnPedDraw` (mp.c), chat ink (`MpSendChat`, `MpHandleChat`) | `MpModePlayerColour` | the player's own colour and on/off |
| `MpSpawnLateJoiners`, `MpOnNetSpawn` | `MpModeLateJoinPolicy`, `MpModeSpawnRule` | live, the session's own placement (`CLUSTER` seats the same way) |
| `MpMarkPresentTick` -> `MpMarkClusterTick` | `MpModeSpawnRule` == `CLUSTER` | never reached (no `JPMK` in mode 0) |
| referee gate (`mp_mark.c`, `markref.py`), `MpRoundStartHoldsCar` | `MpModeHasPreGame` / `MpModeWantsCountdown` and the four wait/countdown fields | 0: no wait, no countdown, no pad hold |

Rules every mode keeps:

- **No game over in a session, ever.** The respawn policy picks *how* a death is
  answered. `MpRespawnAfterDeath` still refuses the game over whatever it says.
- **Placement is x/z only.** The engine resolves the ground height. A mode that
  picks spawn points picks x/z, never y.
- **An unknown id is named `-` and plays as Take a Ride.** A newer server's id did
  exactly that before this framework existed, because nothing read the byte.
- **The wire does not change for a mode.** The id already rides `MP_WELCOME`,
  `MP_SESSION` (in `JPST`), `MP_BEACON` and the master advert, the round
  state rides `JPRD`, and a timer mode's marked player rides `JPMK` (both below).
  Anything else a mode needs on the wire is a protocol change. It bumps `MP_PROTO_VERSION` and updates `tools/mp_test.py`, checked by
  `tools/servers/proto_check.py`.

A decision point that is asked for something this build cannot do yet (a delayed
respawn, spectating, a mode's own spawn points) logs that once per session and
uses the session's default behaviour. Mode 0 never asks, so it never logs.

## The round state (`JPRD`, protocol 22)

One message carries what a player is doing in the round. Identity stays on the
roster: `MP_ROSTER_ENTRY` (32 bytes, no spare), `MP_WELCOME`, `MP_SESSION`,
`MP_BEACON` and the rules byte are unchanged.

`MP_TAG_ROUND` (`"JPRD"`) is an `MP_ROUND` header followed by `count`
`MP_ROUND_ENTRY` rows, one per present player in ascending id (`mp_proto.h`):

| `MP_ROUND` (8 bytes) | |
| --- | --- |
| `gamemode` u8 | the mode the host is refereeing |
| `phase` u8 | `MP_ROUND_PHASE_IDLE` 0 (no round yet), `_LIVE` 1, `_ENDED` 2 |
| `round` u16 | 1 = the match's first round, 0 = none yet |
| `count` u8, `reserved[3]` | rows that follow, at most `MP_MAX_PLAYERS` (16) |

| `MP_ROUND_ENTRY` (8 bytes) | |
| --- | --- |
| `playerId` u8 | the roster's id |
| `state` u8 | `MP_ROUND_PLAYER_PLAYING` 0, `_SPECTATING` 1, `_WAITING` 2 |
| `team` u8 | 0..3, `MP_ROUND_TEAM_NONE` (0xFF) = none |
| `reserved` u8 | 0 |
| `score` i32 | this round's score, starts at 0 |

**Who writes it.** Only the host: the listen host (`mp_round.c`, driven from
`mp_session.c`) or the dedicated server (`dedi/roundstate.py`). The host keeps
the round's players equal to the roster's rows and sends the snapshot:

- after each WELCOME, so a joiner has it before it plays;
- at START (`MpStartMatch` → `MpHostRoundBegin`), after the roster and before `JPST`;
- whenever it changes (a join or a leave), checked every frame;
- every 120 frames with the roster (every 2 s on the dedicated server), so a
  lost one is repaired.

A client replaces its whole copy with each snapshot and never sends one. A host
drops a `JPRD` a client sends and does not relay it, so a peer cannot write a
score or a team. The dedicated server answers it with its own snapshot.

**What a reader gets.** `MpRoundNumber`, `MpRoundPhase`, `MpRoundIsLive`,
`MpRoundPlayerState`, `MpRoundPlayerScore` and `MpRoundPlayerTeam` (`mp_round.h`).
`MpModePlayerTeam` reads the team from here, and is still -1 for any mode with no
teams. A player with no row, or a client with no snapshot yet, reads as Take a
Ride: playing, team -1, score 0. A client in a match that has had no snapshot
after about 10 s logs that once.

**Take a Ride.**

- `MpStartMatch` begins round 1, live. On the dedicated server, round 1 is live
  from server start, because every joiner is sent a START.
- Every present player is playing, on no team, with score 0. A late joiner is
  playing too, and is still spawned live.
- The round never ends on its own, and a real game over is still always refused.
- `MpSoftRestart` keeps the same round. It puts one player back at the level start
  and any player can use it, client or host, so it is not a match restart and
  does not change the round or anyone's score.
- In the lobby, before START, present players are waiting in round 0 (idle).

## Fox N Hounds and Car Bomb Tag (`JPMK`, protocol 23)

Both modes mark one player. In **Fox N Hounds** the marked player is the fox, and
every player has a clock of their own that runs only while they are the fox. The
first player whose clock reaches 0:00 wins. In **Car Bomb Tag** the marked player
holds the bomb, and there is one shared clock. Whoever holds the bomb when it
reaches 0:00 loses, and everyone else wins.

**Rules (the referee, `mp_mark.c`, and its Python twin `dedi/markref.py`).**

- The time is 1 to 5 minutes and defaults to 3 (`MP_MARK_MINUTES_*`). Every clock
  starts full.
- A round needs the mode's wait: at least `waitMinPlayers` present (2 for Fox
  and Bomb) **and**, if `waitForCars` is set, every present player must have
  picked a car (`MpPlayerCarReady` on a listen host, a received `JPCC` on a
  dedicated server). Until then the referee stays `WAITING`: round 0 idle, nobody
  marked, no clock running. The panel says `WAITING FOR PLAYERS`, or
  `WAITING FOR CARS r/n` when enough people are in but someone is still in the
  car select.
- **One countdown, after the wait.** A map load arms the first-load count: on a
  listen host the match START (`MpHostMarkBegin` -> `MpMarkReset`); on a
  dedicated server the boot, and the present set emptying -- a map vote kicks
  everyone to rejoin on the new map, so the next startable group gets the
  first-load count again. When the wait is satisfied the referee goes
  `COUNTDOWN` once. On a fresh map that is `firstCountdownSec` (5 for Fox and
  Bomb) as `5`, `4`, `3`, `2`, `1` (golden `jer_notify`, priority 4, 1 s each);
  the panel says `PRE-GAME n`; the pads are held; every player is respawned as
  this count begins (see the start cluster below). Later rounds on the same map
  skip the first-load count and use a 1 s `GET READY` plus `roundCountdownSec`
  (3) as `3`, `2`, `1`, with the same countdown-start respawn. Nobody is marked,
  clocks show their full time and do not tick,
  no tag, no arrow, no forced music. The first-load flag is only cleared at GO,
  so a countdown aborted by a joiner without a car is retried as the same kind
  (first-load if GO has not happened yet). `MP_MARK_PHASE_LOBBY` (4) is retired
  and unused; proto 23 keeps the value. **GO** is `LIVE`: a random marked
  player, picked by the referee only, the clocks start, the 2 s tag cooldown
  starts, and the notice is `GO! <name> is now the fox!` / `GO! <name> has the
  bomb! Stay away!`.
- A joiner who arrives during `WAITING` or `COUNTDOWN` (or `ENDED`, before the
  next GO) is not a mid-round join: they go through car selection and are
  counted in the wait. Only a join after GO uses mid-round placement.
- **The start cluster** (`MpMarkRoundReset`, `MpMarkClusterTick`,
  `MP_MODE_SPAWN_CLUSTER`). When WAITING is satisfied and COUNTDOWN begins,
  every machine runs a real respawn on its own car at that moment, not during
  waiting and not mid-count: the wreck latch is cleared on every slot
  (`jer_carexplode_reset`), the local car is repaired (damage, dents, hubcaps),
  handbrake / thrust / wheelspin and death state are dropped, the wanted level
  is cleared, and `JER_EVENT_RESET_CAR` is fired. Cars are owner-authoritative,
  so each machine places only its own car; a dedicated server has no world and
  places nothing. The same tick, the anchor (the lowest present id in the
  `JPMK` rows whose car this machine can see) moves to the nearest road
  (`MpFindRoadSpotNear`). Everyone else searches from a slot on a ring around
  the anchor's current pose: one slot per player, 8 per ring, rings 3200 units
  apart (`MP_JOIN_CLEAR` + 1200, the join spawn's retry step). A spot must be
  on a road, within 9600 units of the anchor, and `MP_JOIN_CLEAR` (2000) from
  every other player's car; it is streamed (`MpWarpStreamTo`) and landed on
  (`MpPlaceOnGround`). Each car faces its own random heading (0..4095), stopped
  and repaired. If the ground is still streaming, placement retries for up to
  2 s, then the pause-menu warp beside the anchor (`MpWarpToPlayer`) is the
  fallback; failing that the already-reset car starts where it is, and the log
  says why. A player on foot is not placed. The wreck latch is not left until
  session teardown: that used to keep a bomb holder locked after the next GO
  (`InitCarPhysics` does not clear handbrake, and `jer_carexplode_hold` would
  re-apply the lock if they were marked again).
- A tag is a contact between two player cars, one of them marked; traffic and
  on-foot players never count. The mark moves to the other car, and no further
  tag is accepted for 2 s (`MP_MARK_TAG_COOLDOWN_MS`), so one bump cannot bounce
  it straight back. The spawn arm applies too: a car that spawned less than 4 s
  ago (`MP_HIT_ARM_FRAMES`) cannot tag or be tagged.
- Fox N Hounds: the fox reaching 0:00 is the winner. A hound's clock is frozen.
  Car Bomb Tag: the shared clock does not reset on a pass, and the holder at
  0:00 loses (`winner` none, `marked` stays the loser).
- A round that drops below two players ends with no winner. If the marked
  player leaves and two or more remain, a new marked player is picked at random.
- A late joiner is a hound with a full, frozen clock (Fox N Hounds), or a player
  without the bomb (Car Bomb Tag).
- `ENDED` lasts 10 s (`MP_MARK_INTERMISSION_MS`). Then the same wait-then-countdown
  gate, with the first-load count only if everyone left meanwhile (a new map).
  Otherwise `COUNTDOWN` is the per-round count (a new start cluster, 3-2-1, GO
  with fresh clocks and a new random marked player) when the wait is satisfied,
  else `WAITING`. The round state follows:
  `MpRoundBegin` / `MpRoundEnd` (or `RoundState.begin` / `.end`), so `JPRD`
  counts the rounds.
- None of the stock session rules change. A game over is refused, the respawn is
  instant, and `MpSoftRestart` puts one player back and starts no new round. No
  stock game-over screen is shown at the end: the round just goes to `ENDED`.

**On the wire.** `MP_TAG_MARK` (`"JPMK"`) is an `MP_MARK` header followed by
`count` `MP_MARK_ENTRY` rows, one per present player in ascending id
(`mp_proto.h`):

| `MP_MARK` (12 bytes) | |
| --- | --- |
| `gamemode` u8 | the mode being refereed |
| `phase` u8 | `MP_MARK_PHASE_WAITING` 0, `_LIVE` 1, `_ENDED` 2, `_COUNTDOWN` 3, `_LOBBY` 4 (retired, unused) |
| `marked` u8 | the fox / bomb holder, `MP_MARK_NONE` (0xFF) = nobody |
| `winner` u8 | Fox N Hounds' winner, else `MP_MARK_NONE` |
| `minutes` u8 | the round's time |
| `count` u8 | rows that follow |
| `seq` u16 | +1 per change of marked player; the notices key on it |
| `sharedDs` u16 | Car Bomb Tag's clock, deciseconds (rounded up) |
| `countdownDs` u16 | `COUNTDOWN`: time to GO, deciseconds (rounded up); else 0. Was `reserved` |

| `MP_MARK_ENTRY` (4 bytes) | |
| --- | --- |
| `playerId` u8 | the roster's id |
| `flags` u8 | `MP_MARK_ENTRY_F_READY` 0x01 = has picked a car. Was `reserved` |
| `remainDs` u16 | Fox N Hounds: this player's clock, deciseconds (rounded up) |

`JPMK` is sent only in a timer mode, so a Take a Ride session's traffic, and its
`JPRD`, are byte for byte what they were. No text rides it: each machine builds
the notices from the snapshot and the roster's names. The roster, welcome,
session, beacon and rules layouts are unchanged. Protocol 23 refuses a
protocol-22 HELLO, as every bump does. The round start (0.21.0) uses only bytes
that were reserved and two new phase values in the existing byte, so it is still
protocol 23: an mp 0.20.0 peer on a 0.21.0 host shows a frozen clock during the
lobby and the countdown, is not placed in the cluster and does not hold its pad, then plays
the round normally from GO.

**Who writes it.** Only the host, the same way as `JPRD`. Clients only report
contacts, and the host originates every state.

- **Listen host** (`mp_session.c`): it ticks the referee on the 30 Hz sim frame
  (1000 ms per 30 frames, with no drift). A contact it sees itself
  (`MpContactPush`) is a tag. A client's `MP_HIT` report is a tag only if the
  host's own view has the two cars within 1800 units (twice `MP_HIT_RADIUS`, for
  the lag on two links). It sends `JPMK` after a WELCOME, at START, on every
  change, and every 30 frames while live, counting down or in the lobby (120
  otherwise). A
  `JPMK` a client sends is dropped. The ready flags come from `MpPlayerCarReady`
  (the host is always ready).
- **Dedicated server** (`dedi/core.py`): it steps `MarkReferee` every 0.1 s on
  wall time. It decides tags from the carstate rows it already keeps; this is the
  **position path**. When a row arrives, the sender's car is compared with the
  marked car (or, if the sender is marked, with every other car). It is a tag if
  the cars are within 900 units (`MP_HIT_RADIUS`), closing at 4 units/frame or
  more (`MP_HIT_MIN_CLOSING`), and the other row is under 0.5 s old. Both cars
  must be past the 4 s arm, and the referee's 2 s cooldown also applies. A
  client's `JPHI` is the **fallback**: it counts only if the kept rows put the
  two cars within 1800 units. The server sends `JPMK` after each WELCOME (after
  `JPRD`), on every change, and every 1 s while live, counting down or in the
  lobby (4 s otherwise). A client's `JPMK` is never forwarded, and the sender gets the
  server's own back. A player is ready once their `JPCC` (car pick) has arrived.
  The server referees the wait and the countdown only; it has no road data, so
  it sends no positions -- each client places its own car in the cluster when it
  sees `COUNTDOWN`, exactly as on a listen host.

**What every machine shows** (`MpMarkPresentTick`):

- A notice through `jer_notify`, split by who it is about: `You are now the fox!` /
  `<name> is now the fox!`, and `You have the bomb! Get rid of it!` /
  `<name> has the bomb! Stay away!` (priority 3, 3 s). At the end:
  `You won the match!` / `<name> won the match!` (Fox N Hounds), or
  `BOOM! You lost the match!` / `<name> blew up! You won the match!` (Car Bomb
  Tag) (priority 4, 5 s).
- The pre-game lobby: `5`, `4`, `3`, `2`, `1` (priority 4, 1 s each).
- The countdown: `GET READY`, `3`, `2`, `1` (priority 4, 1 s), then the GO line
  above (priority 4, 3 s). Each count is shown once and only ever downward.
- The clock as a `jer_hud_panel` at the top right: `PRE-GAME n`,
  `WAITING FOR PLAYERS`, `WAITING FOR CARS r/n`,
  `FOX m:ss` / `HOUND m:ss`, or `BOMB m:ss` / `TIMER m:ss`. It fades to cream
  `(128, 128, 64)` while the viewer's clock is running, and to mid grey
  `(128, 128, 128)` while they are a frozen hound. While they hold the bomb the
  text fades to red and flashes at 4 Hz, or 8 Hz under 10 seconds. A client
  runs the clock on between snapshots, for at most 3 s.
- While Fox N Hounds is live, a centre line: `You're the fox!` if you are
  marked, or `<name> is the fox!` with the fox's clock on the next line
  (`SetTextColour` + `PrintStringCentred`, cream). The GO / countdown /
  match-end notices stay on `jer_notify`.
- `jer_arrow_on_car` over the marked car while live. The arrow is never drawn
  over your own car. It is cleared at the end, when the marked player leaves, and
  on session reset.
- The mode's `music` (pursuit) through `jer_music_force` while live, and
  `JER_MUSIC_AUTO` otherwise. Take a Ride never forces music. The engine also
  releases the force and the arrows when it leaves a level, and MP asserts them
  again every frame.
- Car Bomb Tag, while live: a beep from the holder's car (`SBK_ID_COUNTDOWN`
  sample 0, otherwise an always-loaded SFX fallback). The interval shrinks
  from 1 s to 400 ms approaching 10 s, then 200 ms at or under 10 s, and
  every 4 frames under 5 s. A local holder plays it through `StartSound`
  (no position, so `ComputeDoppler` never runs and the `DoDopplerSFX`
  update loop skips it). A remote holder stays positional:
  `Start3DSoundVolPitch` plus `SetChannelPosition3` on the live car pose
  and `linearVelocity`, so source velocity is not left at `dummylong`.
- When the bomb clock ends: `jer_carexplode` on the bomb car — a wrecked dent,
  a tumble, locked controls, three visual-only big bangs (no blast damage),
  and bank 54 sample 1 (or SFX 6). Fired once; a dedicated server never runs
  this C. The files have no link to Caine's Crossfire. The latch and lock stay
  through `ENDED` and any following `WAITING` (that is the wreck). They are
  cleared by the countdown-start respawn above, not only at session teardown.
- A glowing body shell (`jer_carglow`) on the marked car while live: a slightly
  scaled low-LOD copy of the car body (the same `NewLowCarModel` vlist
  `plotNewCarModel` uses, scaled about model-space `-cog`, with the same
  DrawCarObject trans path), drawn semi-transparent with the explosion smoke
  texture and pulsed. It does not use `gTempCarVertDump` — that dump is the
  hi-LOD dented mesh, and pairing it with low-LOD polys mangled the holo.
  The fox uses the player's colour if they have one set, otherwise cyan; the
  bomb car is always red.

## Choosing a mode

- **Listen host:** the Host page has a `Game mode: < ... >` row. It cycles the
  selectable rows only (`MpModeNextSelectable`): Take a Ride, Fox N Hounds, Car
  Bomb Tag. In a timer mode a `Time: < N MIN >` row (1 to 5, wrapping) follows
  it. Neither choice is saved: `MpSessionReset` puts them back to 0 and 3 minutes.
  `MpSetSessionGamemode` and `MpSetSessionModeMinutes` take a change only while
  the match is not running.
- **Dedicated server:** the existing `"gamemode"` key in `dediserver.json`, or
  `--gamemode N`. It is optional and defaults to 0. The time is the optional
  `"mode_time"` key (minutes), or `--mode-time N`. If it is missing or not a
  number, the time is 3, and any other value is clamped to 1..5. It is not in
  `DEFAULTS`, so `upgrade_config` never adds it or rewrites a file because of it.
- **Clients** adopt the host's id from the welcome and the start. They log a line
  when the id is unknown to their build.

## Adding a mode (later phases)

1. Add `MP_GAMEMODE_<NAME>` to the enum in `mp_proto.h`, before `MP_GAMEMODE_COUNT`.
2. Add its row to `kModes[]` in `mp_mode.c`. Row 0 stays Take a Ride.
3. Add the name to `GAMEMODE_NAMES` in `tools/mp_test.py`.
   `tools/mode_check/run_mode_check.py` fails when the two tables disagree.
4. Implement any policy it needs that is still a fallback (see "Not yet").
5. Set `selectable` only when it is playable end to end.

## Dedicated server plugin

`tools/servers/dediserver/dedi/builtin/modes.py` is the skeleton for a mode's
server-side rules. It has `ENABLED_BY_DEFAULT = False`. When it is named in
`"plugins"`, it logs the session's mode and adds a read-only `!mode` command
(chat and console). It sets nothing.

## Tests

- `python tools/mode_check/run_mode_check.py` builds `test_mp_mode.cpp` with
  `mp_mode.c` and `mp_round.c` alone and asserts that every mode-0 answer is today's behaviour,
  including for unknown ids. It also checks that the C names match
  `mp_test.GAMEMODE_NAMES`. The compiler is `--cxx`, `$CXX`, `c++`/`clang++`/`g++`,
  or `python -m ziglang c++`. The test file is `.cpp` because the mod project
  globs `**.c` into the DLL, and the test has a `main()`.
- The same runner builds `test_mp_round.cpp` with `mp_round.c` and `mp_mode.c`. It
  checks the round lifecycle (idle, round 1 live, late join, leave, end and the
  next round), the snapshot bytes, the client apply and its refusals, and the
  Take a Ride defaults. It then compares the C snapshot with
  `mp_test.build_round` byte for byte.
- The same runner builds `test_mp_mark.cpp` with `mp_mark.c`, and checks every
  rule above: the two-player start, the random pick, tags, the cooldown, the
  frozen and running clocks, the win and the loss, late joiners, leavers, the
  intermission, and the clamp; and the round start (`CheckRoundStart`): no start
  while a car is missing, a countdown that touches no clock and allows no tag, a
  car-less joiner aborting it, GO, and the intermission back into the same gate;
  and the pre-game lobby (`CheckLobby`): it holds while nobody is in, runs no
  clock and allows no tag, goes straight to the countdown when it ends, is not
  repeated after the intermission on the same map, comes back when everyone has
  left, and a client applies it.
  It then runs 48 scripted games (about 5,000 steps, with ready-set changes)
  through the C referee and `dedi/markref.py` together, and checks that the
  `JPMK` bytes match after every step, plus the referee's timing constants and
  each mode's countdown flag against the Python mirror.
- `python tools/servers/dediserver/test_modes.py` checks the server side.
- `python tools/servers/dediserver/test_marks.py` checks the dedicated server's
  referee over real sockets:
  - a new map: the first player in gets the 5 s pre-game lobby (and hears it);
    the next round on the same map skips it; everyone leaving re-arms it;
  - one player waits; two without cars wait; every car in -> countdown (no round,
    no clock, no tag) -> GO; a car-less joiner holds the next round after the
    intermission;
  - position-path tags, the arm, the radius, closing speed, and no bounce;
  - the `JPHI` fallback;
  - a client's `JPMK` is answered and not forwarded;
  - the Fox N Hounds win and the Car Bomb Tag loss;
  - a missing `mode_time` means 3 and an operator's file is not rewritten;
  - Take a Ride sends no `JPMK` and its snapshot is unchanged.
- `python tools/servers/dediserver/test_round.py` checks the dedicated server's
  round state over a real socket: the order ROSTER, WELCOME, START, ROUND; round 1
  live with everyone playing; proto 23 on every envelope; proto-21 and proto-22
  HELLOs refused; a client's `JPRD` not forwarded.
- `python tools/servers/proto_check.py` checks the wire mirror: struct sizes
  (now with `MP_ROUND`, `MP_ROUND_ENTRY`, `MP_MARK` and `MP_MARK_ENTRY`), shared
  constants, the round, mark and mode enum values, and every `MP_TAG_*` against
  `mp_test.TAG` in both directions.

## Not yet

Apart from the two timer modes, nothing acts on the round state yet. The timer
modes are a proof of concept: they have been checked headless, as above, but not
yet played in game. These are later phases, and none of them is wired:

- After the POC:
  - an in-game pass of both modes on a listen host and a dedicated server;
  - a scoreboard of wins;
  - an option for the intermission length;
  - a cooldown on the HUD;
  - the arrow from modules loaded as DLLs (the host API table is not extended yet);
  - round-end screens beyond the notice.

- Round start, not yet: an in-game pass of the cluster on a listen host and on a
  dedicated server; a server-side cluster (the server has no road data and no
  host-pose packet exists, so each client places itself).
- Phase 2: the spectator camera, late-join wait (a joiner is still spawned live),
  and the respawn delay and no-respawn policies. (The 3-2-1 countdown and its
  input hold landed with the timer modes' round start, 0.21.0.)
- Phase 3: team assignment (every team is none), forced team colours, and
  scoreboard columns. Score is carried and stays 0, and nothing draws it.
- Later: arenas and mode spawn points, team radio, a mode options menu, Capture
  the Flag, Cops n Robbers, map cycling, and the admin pause page.
