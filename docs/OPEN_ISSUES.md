# Open issues — mp

## Late-join spawn: two verification gaps (2026-10-20)

Fixed on `fix/mp-late-join-spawn` (commits `4d7e9ca1`, `9900e6aa`). Both gaps below
are **guarded in code** and the full-city and arena 3-seat rigs pass, but neither
scenario has been exercised end-to-end.

### 1. A join while another player is on foot next to their car

`MpSpawnLateJoiners` refuses every owned slot, including the parked body recorded
behind `MP_PLAYER.leftCarSlot1`, and logs:

    [mp] spawn: refused slot %d for player %d - car is owned (...)

**Not yet run:** three seats; the first joiner gets out with
`MP_TEST_CARCHANGE=<secs>,<exitSecs>` **before** the second joiner arrives, so the
join pass sees a `CONTROL_TYPE_CIV_AI` + `gMpQuietPad` body that is still that
player's parked car.

Pass criteria:

- the parked body keeps its slot, damage and felony;
- the joiner is seated on a different slot (or the pass logs `refused slot`);
- when the on-foot player walks back in they re-seat on their own body
  (`MP_CARSTATE_SAME_BODY`), not a fresh spawn.

### 2. A dedicated-server city-change rejoin

`MpSendOwnCarState` now returns while `!gMp.gameStarted`, which
`JER_EVENT_GAME_START` (`MpOnGameStart`) sets. A rejoin after a `!vote` city change
relaunches the level, so the hook should fire again while the session is active.

**Not yet run:** a city vote, then the client's auto-reconnect.

Pass criteria:

- after the relaunch the client sends carstates again (peer poses keep updating);
- `gMp.gameStarted` is set; if it is not, the client is visible but frozen.

## Build caveat found while fixing this

Changing `mp.h` did not recompile every mp source on an incremental build — only
`mp_session.c` was rebuilt, leaving `MP_PLAYER`/`MP_STATE` layouts different across
objects (symptoms were partial struct writes: a join pose's x/z zeroed while its
heading survived). Clear `JERICHO/MODS/mp/obj/<config>/x64` after a header change,
or make the project track `mp.h`.
