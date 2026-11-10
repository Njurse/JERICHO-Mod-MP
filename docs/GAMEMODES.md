# Game modes

Status: **Phase POC, two timer modes.** There are three modes: Take a Ride (id 0),
which plays exactly as it did before the framework existed; Fox N Hounds (id 1);
and Car Bomb Tag (id 2). Phase 1 added the round state every mode reads, on the
wire as `JPRD` (protocol 22). Phase POC adds the marked player and the clocks, on
the wire as `JPMK` (protocol 23, mp module 0.20.0), and uses two core APIs from
JERICHO 0.13.11+: `jer_arrow_on_car` and `jer_music_force` (core PR #94). Everything
below "Not yet" is design, not code.

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
| `countdown` | a 3-2-1 before each round | 0 |
| `carChangeLocked` | no car change while a round runs | 0 |
| `carPool` | car-pool filter id, `MP_MODE_CARPOOL_*` | `ALL` |
| `spawn` | spawn rule, `MP_MODE_SPAWN_*` | `SESSION` |
| `mapCycle` | the lobby may cycle maps | 0 |
| `timer` | `MP_MODE_TIMER_NONE` / `_PERSONAL` (a clock per player) / `_SHARED` (one clock) | none |
| `music` | `MP_MODE_MUSIC_AUTO` / `_CALM` / `_PURSUIT` while a round is live (= `JER_MUSIC_*`) | auto |

The rows today:

| id | name | timer | music |
| --- | --- | --- | --- |
| 0 | `TAKE A RIDE` | none | auto |
| 1 | `FOX N HOUNDS` | personal | pursuit |
| 2 | `CAR BOMB TAG` | shared | pursuit |

All three are selectable. Apart from `timer` and `music`, modes 1 and 2 take
every Take a Ride answer: instant respawn, live late join, no teams, no car
pool, no countdown.

The session never reads a field. It asks a question at a fixed **decision point**
and passes `gMp.gamemode`:

| decision point | question | mode 0 answer |
| --- | --- | --- |
| `MpRespawnAfterDeath` (mp_session.c) | `MpModeRespawnPolicy` | instant: the existing respawn |
| `MpChangeCar` | `MpModeCarChangeRefusal` | NULL: never refused |
| `MpCarListForCity` | `MpModeFilterCarPool` | the list, untouched |
| `MpNameTagColour` (mp_ui.c), `MpOnPedDraw` (mp.c), chat ink (`MpSendChat`, `MpHandleChat`) | `MpModePlayerColour` | the player's own colour and on/off |
| `MpSpawnLateJoiners`, `MpOnNetSpawn` | `MpModeLateJoinPolicy`, `MpModeSpawnRule` | live, the session's own placement |

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
- A round needs two present players. With fewer, the round stays `WAITING`:
  round 0 idle, nobody marked, no clock running. When the second player is in,
  round 1 begins with a random marked player, picked by the referee only.
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
- `ENDED` lasts 10 s (`MP_MARK_INTERMISSION_MS`). The next round then starts with
  fresh clocks and a new random marked player, or the round goes back to
  `WAITING` if there are fewer than two players. The round state follows:
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
| `phase` u8 | `MP_MARK_PHASE_WAITING` 0, `_LIVE` 1, `_ENDED` 2 |
| `marked` u8 | the fox / bomb holder, `MP_MARK_NONE` (0xFF) = nobody |
| `winner` u8 | Fox N Hounds' winner, else `MP_MARK_NONE` |
| `minutes` u8 | the round's time |
| `count` u8 | rows that follow |
| `seq` u16 | +1 per change of marked player; the notices key on it |
| `sharedDs` u16 | Car Bomb Tag's clock, deciseconds (rounded up) |
| `reserved` u16 | 0 |

| `MP_MARK_ENTRY` (4 bytes) | |
| --- | --- |
| `playerId` u8 | the roster's id |
| `reserved` u8 | 0 |
| `remainDs` u16 | Fox N Hounds: this player's clock, deciseconds (rounded up) |

`JPMK` is sent only in a timer mode, so a Take a Ride session's traffic, and its
`JPRD`, are byte for byte what they were. No text rides it: each machine builds
the notices from the snapshot and the roster's names. The roster, welcome,
session, beacon and rules layouts are unchanged. Protocol 23 refuses a
protocol-22 HELLO, as every bump does.

**Who writes it.** Only the host, the same way as `JPRD`. Clients only report
contacts, and the host originates every state.

- **Listen host** (`mp_session.c`): it ticks the referee on the 30 Hz sim frame
  (1000 ms per 30 frames, with no drift). A contact it sees itself
  (`MpContactPush`) is a tag. A client's `MP_HIT` report is a tag only if the
  host's own view has the two cars within 1800 units (twice `MP_HIT_RADIUS`, for
  the lag on two links). It sends `JPMK` after a WELCOME, at START, on every
  change, and every 30 frames while live (120 otherwise). A `JPMK` a client sends
  is dropped.
- **Dedicated server** (`dedi/core.py`): it steps `MarkReferee` every 0.1 s on
  wall time. It decides tags from the carstate rows it already keeps; this is the
  **position path**. When a row arrives, the sender's car is compared with the
  marked car (or, if the sender is marked, with every other car). It is a tag if
  the cars are within 900 units (`MP_HIT_RADIUS`), closing at 4 units/frame or
  more (`MP_HIT_MIN_CLOSING`), and the other row is under 0.5 s old. Both cars
  must be past the 4 s arm, and the referee's 2 s cooldown also applies. A
  client's `JPHI` is the **fallback**: it counts only if the kept rows put the
  two cars within 1800 units. The server sends `JPMK` after each WELCOME (after
  `JPRD`), on every change, and every 1 s while live (4 s otherwise). A
  client's `JPMK` is never forwarded, and the sender gets the server's own back.

**What every machine shows** (`MpMarkPresentTick`):

- A notice through `jer_notify`, split by who it is about: `You are now the fox!` /
  `<name> is now the fox!`, and `You have the bomb! Get rid of it!` /
  `<name> has the bomb! Stay away!` (priority 3, 3 s). At the end:
  `You won the match!` / `<name> won the match!` (Fox N Hounds), or
  `BOOM! You lost the match!` / `<name> blew up! You won the match!` (Car Bomb
  Tag) (priority 4, 5 s).
- The clock as a `jer_hud_panel` at the top centre: `WAITING FOR PLAYERS`,
  `FOX m:ss` / `HOUND m:ss`, or `BOMB m:ss` / `TIMER m:ss`. It is orange while you
  are marked. A client runs the clock on between snapshots, for at most 3 s.
- `jer_arrow_on_car` over the marked car while live. The arrow is never drawn
  over your own car. It is cleared at the end, when the marked player leaves, and
  on session reset.
- The mode's `music` (pursuit) through `jer_music_force` while live, and
  `JER_MUSIC_AUTO` otherwise. Take a Ride never forces music. The engine also
  releases the force and the arrows when it leaves a level, and MP asserts them
  again every frame.

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
  intermission, and the clamp. It then runs 44 scripted games (about 4,900 steps)
  through the C referee and `dedi/markref.py` together, and checks that the
  `JPMK` bytes match after every step.
- `python tools/servers/dediserver/test_modes.py` checks the server side.
- `python tools/servers/dediserver/test_marks.py` checks the dedicated server's
  referee over real sockets:
  - one player waits and two start;
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

- Phase 2: the spectator camera, late-join wait (a joiner is still spawned live),
  the respawn delay and no-respawn policies, and the 3-2-1 countdown and its
  input freeze.
- Phase 3: team assignment (every team is none), forced team colours, and
  scoreboard columns. Score is carried and stays 0, and nothing draws it.
- Later: arenas and mode spawn points, team radio, a mode options menu, Capture
  the Flag, Cops n Robbers, map cycling, and the admin pause page.
