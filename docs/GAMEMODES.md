# Game modes

Status: **Phase 1, round lifecycle and state carrier.** There is one mode, Take a
Ride (id 0), and it plays exactly as it did before the framework existed. Phase 1
adds the round state every later mode will read, on the wire as `JPRD` (protocol
22, mp module 0.19.0). Nothing displays or acts on it yet. Everything below "Not
yet" is design, not code.

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
  `MP_SESSION` (in `JPST`), `MP_BEACON` and the master advert, and the round
  state rides `JPRD` (below). Anything else a mode needs on the wire is a protocol
  change. It bumps `MP_PROTO_VERSION` and updates `tools/mp_test.py`, checked by
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

## Choosing a mode

- **Listen host:** the Host page has a `Game mode: < ... >` row. It cycles the
  selectable rows only (`MpModeNextSelectable`), so today it stays on Take a Ride.
  The choice is per session and is not saved: `MpSessionReset` puts it back to 0.
  `MpSetSessionGamemode` takes it only while the match is not running.
- **Dedicated server:** the existing `"gamemode"` key in `dediserver.json`, or
  `--gamemode N`. It is optional and defaults to 0. No new config keys came with
  modes, so `upgrade_config` never rewrites a config file because of them.
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
- `python tools/servers/dediserver/test_modes.py` checks the server side.
- `python tools/servers/dediserver/test_round.py` checks the dedicated server's
  round state over a real socket: the order ROSTER, WELCOME, START, ROUND; round 1
  live with everyone playing; proto 22 on every envelope; a proto-21 HELLO
  refused; a client's `JPRD` not forwarded.
- `python tools/servers/proto_check.py` checks the wire mirror: struct sizes
  (now with `MP_ROUND` and `MP_ROUND_ENTRY`), shared constants, the round enum
  values, and every `MP_TAG_*` against `mp_test.TAG` in both directions.

## Not yet

The round state is carried but nothing acts on it yet. These are later phases,
and none of them is wired:

- Phase 2: the spectator camera, late-join wait (a joiner is still spawned live),
  the respawn delay and no-respawn policies, and the 3-2-1 countdown and its
  input freeze.
- Phase 3: team assignment (every team is none), forced team colours, and
  scoreboard columns. Score is carried and stays 0, and nothing draws it.
- Later: arenas and mode spawn points, team radio, a mode options menu, Capture
  the Flag, Cops n Robbers, map cycling, and the admin pause page.
