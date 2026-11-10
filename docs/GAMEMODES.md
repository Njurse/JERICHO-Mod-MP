# Game modes

Status: **Phase 0, scaffold only.** There is one mode, Take a Ride (id 0), and it
plays exactly as it did before the framework existed. Everything below "Not yet"
is design, not code.

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
  `MP_SESSION` (in `JPST`), `MP_BEACON` and the master advert. Anything a mode
  needs on the wire later (teams, round state) is a protocol change. It bumps
  `MP_PROTO_VERSION` and updates `tools/mp_test.py`, checked by
  `tools/servers/proto_check.py`.

A decision point that is asked for something this build cannot do yet (a delayed
respawn, spectating, a mode's own spawn points) logs that once per session and
uses the session's default behaviour. Mode 0 never asks, so it never logs.

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
  `mp_mode.c` alone and asserts that every mode-0 answer is today's behaviour,
  including for unknown ids. It also checks that the C names match
  `mp_test.GAMEMODE_NAMES`. The compiler is `--cxx`, `$CXX`, `c++`/`clang++`/`g++`,
  or `python -m ziglang c++`. The test file is `.cpp` because the mod project
  globs `**.c` into the DLL, and the test has a `main()`.
- `python tools/servers/dediserver/test_modes.py` checks the server side.
- `python tools/servers/proto_check.py` checks the wire mirror. It is unchanged
  because no struct changed.

## Not yet

None of these is wired. Each one is a later phase: teams on the wire, spectating,
the countdown runtime, arenas and mode spawn points, team radio, a mode options
menu, Capture the Flag, Cops n Robbers, map cycling, and the admin pause page.
