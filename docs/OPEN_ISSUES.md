# Open issues — mp

## Late-join spawn and ownership: status (2026-10-21)

Fixed on `fix/mp-late-join-spawn` (commits `4d7e9ca1`, `9900e6aa`). The on-foot
ownership path is now exercised end to end; the remaining gap is a joiner that
arrives **after** the owner has already got out, which cannot see a body it never
held (follow-up below).

### 1. A join while another player is on foot next to their car

`MpSpawnLateJoiners` refuses every owned slot, including the parked body recorded
behind `MP_PLAYER.leftCarSlot1`, and logs:

    [mp] spawn: refused slot %d for player %d - car is owned (...)

**Verified 2026-10-21 on three seats.** The commandeer half was staged with the
owner getting out *after* the late joiner was in:

    --seat-env client1=MP_TEST_ONFOOT=25
    --seat-env host=MP_TEST_ONFOOT=35 --seat-env host=MP_TEST_ENTER_LEFT=45,1,10
    --seat-env client1=MP_TEST_PAUSECAR=65,3,3 --seat-env host=MP_TEST_PAUSECAR=75,3,2

Result: PASS, `dumps=0`; the host found and entered player 1's parked body
(`enter left car - player 1 slot 1 ... TannerCanEnterCar=1`), player 1's return was
given a different free slot, the host kept the commandeered body and walked back
into it (`change car: got back into slot 1`), and no seat ended with two bodies for
one player. The host also removed the car it had left behind when it commandeered
(`removed the car we left (slot 0) after getting into slot 1`).

The earlier `no left car for player 1 on this machine` report is the other case: a
machine that joined **after** the owner had already got out never held that body.
That is the follow-up below, not a claim or predicate failure -- a seat that was
present for the exit finds the body and passes every gate.

### Follow-up: materialize a left car on a joiner that missed the exit

A late joiner that arrives while a player is on foot has only that player's
`MP_PEDSTATE` stand-in; it never had the abandoned `CIV_AI` + quiet-pad body, and
`leftCarSlot1` is only recorded when the carstate's NO_CAR row follows a local slot
this machine assigned. Implement this by carrying the left car's model and pose (in
`MP_CARSTATE_NO_CAR` and/or `MP_PEDSTATE`), building the parked body on join, and
recording `leftCarSlot1` for it. Not implemented; it does not affect the same-body
invariant for machines that were present, and `MP_TEST_ENTER_LEFT`'s failure line
now names this case explicitly instead of saying only "no left car".

### 2. A dedicated-server city-change rejoin

`MpSendOwnCarState` now returns while `!gMp.gameStarted`, which
`JER_EVENT_GAME_START` (`MpOnGameStart`) sets. A rejoin after a `!vote` city change
relaunches the level, so the hook should fire again while the session is active.

**Not yet run:** a city vote, then the client's auto-reconnect.

Pass criteria:

- after the relaunch the client sends carstates again (peer poses keep updating);
- `gMp.gameStarted` is set; if it is not, the client is visible but frozen.

## Ownership fixes made during this verification (2026-10-21)

- `MpCarSlotTakenByOther` now counts another row's `leftCarSlot1` as ownership, so a
  commandeered-then-parked body is not deleted by the original owner's spawn.
- The `MP_CARSTATE_SAME_BODY` reuse gate rejects a slot another row claims through
  `leftCarSlot1`; without that the original owner reclaimed the commando's parked car.
- `MpFollowLocalCar` removes the body the local player left when they get into a
  *different* car. That was the extra car the peers kept after a commandeer.
- `MP_TEST_ENTER_LEFT` now calls `RemovePlayerPedestrian` after
  `ChangePedPlayerToCar`, as the engine's get-in path and `MP_TEST_PAUSECAR` do. The
  leaked local Tanner was the source of the `intrThreadMain+0x7E` crash: the exit
  wrote a world coordinate over `vsync_callback`.

## Build caveat found while fixing this

Changing `mp.h` did not recompile every mp source on an incremental build — only
`mp_session.c` was rebuilt, leaving `MP_PLAYER`/`MP_STATE` layouts different across
objects (symptoms were partial struct writes: a join pose's x/z zeroed while its
heading survived). Clear `JERICHO/MODS/mp/obj/<config>/x64` after a header change,
or make the project track `mp.h`.
