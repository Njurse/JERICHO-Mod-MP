/*
 * mp_session.c -- JERICHO Multiplayer session: bring-up, the join handshake
 * (with the mod-match lobby policy), the per-frame owner-authoritative car
 * sync and input replication, the roster, the on-foot stand-ins, chat, and the
 * send/recv dispatch that the transport (mp_net.c) calls into.
 */
/* The session's engine surface lives in mp_session_int.h, so every split
 * module (mp_round_net.c, mp_lobby.c, ...) compiles against exactly the
 * same headers this file always did. */
#include "mp_session_int.h"

/* WHICH CAR SLOTS ARE OURS.
 *
 * Sticky: once the module has put a player in a car, that slot is a module car for
 * the rest of the session, even after the player gets out of it. It matters
 * because a car you have left is still in the world with nobody driving it, and
 * the engine's traffic AI treats such a car as fair game.
 *
 * Cleared when the session ends: a stale mark would make us take a STOCK traffic
 * car for one of ours on the next match, which would break the traffic instead. */
unsigned char gMpOurCars[MAX_CARS];

/* A pad nobody writes, for cars that must sit still. Pad 1 is free because a match
 * keeps NumPlayers at 1 -- each machine drives the whole screen. Not -1 and not
 * NULL: the engine indexes Pads[] with it and some sites do not check. */
char gMpQuietPad = 1;

/* The remote snapshot ring lives here, with the other session state, because
 * both the contact sender (MpContactPush) and the contact receiver
 * (MpHandleHit/MpHitFrame above the playout code in file order) read it. */

extern int MapHeight(VECTOR* pos);	/* the engine's ground height at an x/z */

/* How long a level load may block our main loop before we assume the peer is
 * gone rather than merely loading. Both sides load at the same time, so both
 * go quiet for the whole load. */
#define MP_BUSY_LAUNCH_MS 60000

/* ------------------------------------------------------------------ */
/* Session lifecycle                                                   */
/* ------------------------------------------------------------------ */
/* One launch per session. The client asks to launch twice -- once from the
 * live-match WELCOME (gMp.pendingLaunch) and once when the car select's START is
 * claimed -- and two SetState(STATE_GAMESTART) calls load the level twice, i.e.
 * the match appears to restart the moment the joiner arrives. Cleared by
 * MpSessionReset, so a new session can launch again. */
int gMpLaunched;

/* A LIVE join: the engine builds OUR car on the level's own start point, which is
 * where the host (and everyone before us) came in. MpPlaceSelfTick moves it to a
 * random road spot away from the other players once the level is up. */
/* Placement is tried from the FIRST frame our car exists, and retried every frame
 * (the road data around the probe may still be streaming in) for up to this many
 * frames before giving up on the level start. It used to wait a fixed 15 frames
 * and then try once, which is the visible "sits on the host, then pops away". */

int gMpPlaceSelfFrames;
int gMpPlaceSelfTry;	/* frames spent trying to place (MpPlaceSelfTick) */
/* Map-wide join placement (full-city Take a Ride only): the ONE region streamed
 * for this join, and how many centers have been probed inside it. -1 = none
 * chosen yet. Kept across ticks so a join never thrashes the streamer. */
int gMpWideRegion = -1;
int gMpWideTries;

int gMpLocalAppliedPad = -1;	/* the pad that drove OUR car, -1 = none seen yet */

void MpFollowLocalCar(void);

/* The last message received and from which connection: logged with every respawn,
 * re-place and refused write to our own car, so a log says what set it off. */
char gMpLastRxTag[5] = "none";
int gMpLastRxConn = -1;
unsigned long gMpLastRxFrame;

/* Is car slot `slot` OUR car (the one the local player drives on this machine)? A
 * received message about ANOTHER player may never rebuild, re-place, adopt or remove
 * this car. Logs the refusal with the message that asked, so a future log is conclusive. */
int MpRemoteTargetsLocalCar(int slot, const char* what, int otherId)
{
	MP_PLAYER* me = MpLocalPlayer();
	int mine = (slot >= 0 && slot < MAX_CARS) &&
		(slot == player[0].playerCarId || (me != NULL && slot == me->carId));

	if (mine && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] ignored remote %s targeting local car (slot %d, other player %d, our player %d, last rx %s conn %d frame %lu)\n",
			what, slot, otherId, me != NULL ? me->id : -1, gMpLastRxTag, gMpLastRxConn, gMpLastRxFrame);
	return mine;
}

/* mp.c calls this from the NET_INPUT hook for our own car, so we replicate the
 * pad the engine is actually driving us with. */
void MpNoteLocalPad(int pad)
{
	gMpLocalAppliedPad = pad;
}

static int gMpModeFallbackLogged[MP_MODE_FALLBACK_KINDS];

void MpModeFallbackOnce(int kind, const char* what, int asked)
{
	if (kind < 0 || kind >= MP_MODE_FALLBACK_KINDS || gMpModeFallbackLogged[kind])
		return;

	gMpModeFallbackLogged[kind] = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] mode %d (%s): %s %d is not implemented in this build - using the session default\n",
			gMp.gamemode, MpModeName(gMp.gamemode), what, asked);
}

/* An id adopted from the host (WELCOME / START): kept as-is for naming, played as
 * Take a Ride when this build does not know it (MpModeBehaviour). */
void MpModeNoteAdopted(int gamemode, const char* how)
{
	if (MpModeFind(gamemode) == NULL && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] gamemode %d from %s is not known to this build - playing it as %s\n",
			gamemode, how, MpModeName(MP_GAMEMODE_TAKEADRIDE));
}

int MpSetSessionGamemode(int gamemode)
{
	if (MpIsHost() && !gMp.running && MpModeIsSelectable(gamemode) && gMp.gamemode != gamemode)
	{
		gMp.gamemode = gamemode;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] lobby game mode: %d (%s)\n", gamemode, MpModeName(gamemode));
	}

	return gMp.gamemode;
}

/* mp 0.25: the round time is SECONDS and belongs to the game mode: the host's
 * saved choice for the mode now selected ([mp] round_seconds_<mode>), so switching
 * the lobby's mode shows that mode's own time. */
int MpSessionModeSeconds(void)
{
	int gm = gMp.gamemode;

	if (gm < 0 || gm >= MP_GAMEMODE_COUNT)
		return MpMarkDefaultSeconds(gm);
	return MpMarkClampSeconds(gMp.config.modeSeconds[gm] > 0 ? gMp.config.modeSeconds[gm] : MpMarkDefaultSeconds(gm));
}

int MpSetSessionModeSeconds(int seconds)
{
	int gm = gMp.gamemode;

	seconds = MpMarkClampSeconds(seconds);

	if (MpIsHost() && !gMp.running && gm >= 0 && gm < MP_GAMEMODE_COUNT &&
		gMp.config.modeSeconds[gm] != seconds)
	{
		gMp.config.modeSeconds[gm] = seconds;	/* the host's default for this mode */
		MpConfigSave();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] lobby round time (%s): %d s\n", MpModeName(gm), seconds);
	}

	return MpSessionModeSeconds();
}

int MpSessionModeMinutes(void)
{
	return (MpSessionModeSeconds() + 59) / 60;
}

int MpSetSessionModeMinutes(int minutes)
{
	MpSetSessionModeSeconds(MpMarkClampMinutes(minutes) * 60);
	return MpSessionModeMinutes();
}

void MpClearFelony(int notify);

/* ------------------------------------------------------------------ */
/* COPS N ROBBERS: THE POLICE RADIO                                     */
/*                                                                      */
/* The engine's own cop lines (SOUND_BANK_VOICES: the host city's phrase */
/* bank, loaded by LoadLevelSFX, gamesnd.c), said from the cop PLAYERS' */
/* cars, on the engine's own cues (cop_ai.c ControlCops, felony.c):     */
/*   GO             10 or 11, "suspect picked up", + the heading clip;   */
/*   a scored hit   6 (the heat meter went up: the damage line);        */
/*   chatter        15..17 + heading, no repeat, no sooner than the      */
/*                  engine's own TimeSinceLastSpeech > 720 frames.       */
/* The heading clip (sample direction + 1, GetCarDirectionOfTravel of   */
/* the getaway) follows where the speaking car's lines want one         */
/* (CopPhraseWantsHeading, gamesnd.c): stock D2 lines as CopSay -- 10   */
/* for cop banks 2/3, 15 for 1/4; a baked D1/bank set after 10 and 15.  */
/* The speaker ROTATES among the cop players within MP_RADIO_RANGE of  */
/* the getaway (round-robin by player id after the last speaker), so    */
/* the chatter stays on the cars in the action; no cop in range = no    */
/* line. The damage line is said by the cop who scored it (the last     */
/* cop whose round score went up -- the host's MpRoundAddScore, seen    */
/* by clients through JPRD). 3D from that car (non-positional when it   */
/* is our own car). (mp 0.23.4)                                         */
/* NOBODY TALKS OVER ANYBODY: one line at a time per listener -- a new  */
/* line waits for the last (and its heading clip) to finish, and for    */
/* the engine's own speech queue (an AI cop's CopSay) to be quiet.      */
/* Presentation only: nothing on the wire, each machine its own.        */
/* Which lines: the SPEAKING CAR's city. A cop car imported from another */
/* city speaks that city's radio lines (GetCopPhraseSample, gamesnd.c: */
/* a D2 city's stock phrase banks; a Driver 1 cop its bank's baked     */
/* LUMP_POLICE_VOICES, else Driver 1 Miami's), loaded beside the host's */
/* when the car arrives; with none resident, the host city's. (0.24.0)  */
/* ------------------------------------------------------------------ */

/* x/z units from the getaway: about one draw distance -- a cop that can SEE the
 * chase (two thirds of MP_MARK_CNR_ESCAPE_DIST, 24000, since 0.24.0). Farther
 * cops stay quiet. */

/* The longest a line (or its heading clip) holds the radio. The wait is on the
 * voice's own play time (jer_sound_channel_busy: channels[].time), but an UNLOCKED
 * voice (jer_sound_ensure gives -1 once the reserve is reached, or after a level
 * change dropped the lock) can be taken by a LOOPING sample -- a siren, an engine --
 * and sound.c never counts a looping voice's time down, so the radio waited on it
 * forever: the "lines stop switching" report. 5 s is longer than any phrase. */

/* NO "HIT ANOTHER VEHICLE" FOR COP-VS-ROBBER. The engine says that line
 * (felony.c NoteFelony, CopSay 7/8 on a felony type 4 car hit) whenever the
 * local car collides with another car. In a live Cops n Robbers round a cop
 * ramming the getaway, or the getaway ramming a cop, is the game itself, not
 * "the suspect hit a vehicle": core's gJerCopHitLineVeto asks us, with the car
 * the local car collided with. Silenced: on a cop's machine, the getaway's
 * car; on the getaway's machine, any cop player's car. Every other hit
 * (traffic, parked cars) keeps the line. Presentation only. */
int gMpCnrHitMarked = -1;	/* the getaway's player id while the veto is set */


void MpSessionReset(void)
{
	/* a session that is over must not stay on the board: the unregister is what
	 * takes it off, and MpMasterAdvertiseStop sends it when the socket is free
	 * (otherwise the entry lapses on the master's ttl). Idempotent, so the several
	 * "this session is over" paths can all call it. */
	MpMasterAdvertiseStop();

	/* ...and the router mapping the host opened must go too (best effort; the
	 * worker deletes what it added and exits on its own thread). */
	MpUpnpStop();

	/* THE OLD SESSION'S LISTENER GOES WITH IT, and this is where, because this is the one
	 * primitive EVERY "the session is over" path goes through - MpReturnToFrontend and both
	 * Begin* calls (which reset before they establish anything new).
	 *
	 * It used to be closed only from MpLeaveSession, so a HOST that ended its match any other
	 * way (a lost connection, a refusal, the host ending the game) came back to the frontend
	 * with gMp.listenersUp still set. That is not just a bound port: the presim hook runs
	 * MpNetPoll whenever listenersUp is set, so an abandoned host kept ACCEPTING peers into a
	 * session that no longer existed, and the next session inherited a stale listener. The
	 * "first join worked but I could not reconnect" report was a symptom, and a join being a
	 * handover only papered over it. Guarded, so a fresh Begin (listener already down, and
	 * this is a no-op) does not log a teardown of nothing. Discovery advertises this host, so
	 * it stops here too. */
	if (gMp.listenersUp)
	{
		MpHostEnd();
		MpDiscoveryStop();
	}

	gMpLaunched = 0;
	gMpLocalAppliedPad = -1;
	MpFailsafeReset();
	gMp.gamemode = MP_GAMEMODE_TAKEADRIDE;
	memset(gMpModeFallbackLogged, 0, sizeof(gMpModeFallbackLogged));
	MpRoundReset();		/* no round, nobody in it: the next START begins round 1 */
	gMp.modeMinutes = gMp.config.modeMinutes > 0 ? gMp.config.modeMinutes : MP_MARK_MINUTES_DEFAULT;	/* the host's saved default */
	MpMarkPresentClear();	/* no clock, arrow or forced music outlives a session */
	MpMarkReset(MP_GAMEMODE_TAKEADRIDE, MP_MARK_MINUTES_DEFAULT);
	MpMarkPresentReset();	/* this module's own state */

	MpRoundNetReset();	/* this module's own state */

	gMp.city = 0;
	gMp.timeOfDay = -1;
	gMp.weather = -1;
	gMp.seed = 0;
	gMp.frame = 0;
	MpSyncReset();	/* this module's own state */

	MpPlayResetAll();	/* no snapshot history survives a match */
	gMp.running = 0;
	gMp.leaving = 0;
	gMp.modsMatched = 1;
	gMp.localPlaced = 0;
	gMp.liveJoin = 0;
	gMpSawPreLive = 0;
	gMp.gameStarted = 0;
	gMpPlaceSelfFrames = 0;
	gMpPlaceSelfTry = 0;

	MpSpawnReset();	/* this module's own state */

	/* ...and a "change car" still waiting for a spare slot is session state too: it must not
	 * outlive the session it was asked in. MpLeaveSession forgets it, but every OTHER way back
	 * to the frontend (MpReturnToFrontend) went through this function without doing so, so the
	 * wait kept counting down in the menus and fired a change against whatever car happened to
	 * be under the player there. */
	MpCarChangeWaitForget();

	/* ...and the role-car picks: they belong to this connection (ROLE CARS, below). A
	 * server city change's own leave keeps them - the rejoin is the same connection. */
	MpRolePicksForget("the session is over");

	/* ...and a death the mode's respawn hold kept: it belonged to that match */
	gMpRespawnDeferred = 0;

	/* the palette-correction log is throttled per reported value; clear that
	 * memory so a fresh session reports its first correction */

	/* A new session starts with a clean suit-colour cache, so a colour minted for
	 * a player in a previous match cannot be handed back for a different one
	 * (the level load clears it too; this makes the scope explicit and
	 * independent of a level reload). */
	jer_ped_palette_reset();
}

void MpLaunchLocal(void)
{
	/* Idempotent: see gMpLaunched. A second SetState(STATE_GAMESTART) would load
	 * the level again and restart the match on the joining machine. */
	if (gMpLaunched)
		return;

	gMpLaunched = 1;

	/* TEST LEVER (MP_TEST_CARSELECT=slotN): the stock car select is the one step a
	 * padless run cannot drive, so this writes exactly the state CarSelectScreen
	 * leaves behind when Select is pressed (FEmain.c): carSelection plus
	 * wantedCar[0] resolved against THIS machine's city, before the session city is
	 * applied below - the same order the real pick happens in. It is what makes
	 * "is the player's pick respected?" a line in a headless run instead of
	 * something only a human at a menu can check. */
	{
		const char* s = MpTestCarSelect();

		if (s != NULL)
		{
			extern int carSelection;
			extern char carNumLookup[CITY_COUNT][10];
			int slotNo = atoi(s);
			int idx = (slotNo >= 1 && slotNo <= 10) ? slotNo - 1 : 0;
			int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;

			carSelection = idx;
			wantedCar[0] = carNumLookup[lvl][idx];

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] test: car select honoured slot %d -> wantedCar[0]=%d (MP_TEST_CARSELECT)\n",
					slotNo, wantedCar[0]);
		}
	}

	GameLevel = gMp.city;
	GameType = GAME_TAKEADRIVE;
	NumPlayers = 1;
	/* The arena is part of the session now, not a local boot flag: gSubGameNumber
	 * (offset by 440 in glaunch to reach the M58/M498 multiplayer levels) is taken
	 * from the shape the host published, so the host and a client that booted with
	 * a different -mp still load the SAME map. It used to be hardcoded 0, which
	 * silently ignored the arena on every machine. */

	/* An extra arena is only loose mission files. Without them the BLK
	 * has no such mission and the load reads garbage, so fall back to MP 0 and
	 * say why. */
	if (gBootMpLevel && !MpArenaInstalled(gMp.city, gBootMpArena))
	{
		if (gMpCtx)
			gMpCtx->jer_log(gMpCtx, "[mp] arena %d of city %d is not installed here (run BAKE_D1_CARS to build Vegas MP 2/3) - loading MP 0 instead\n",
				gBootMpArena, gMp.city);
		jer_notify("This map is not installed - run Bake to add Vegas MP 2/3", 4, 4);
		gBootMpArena = 0;
	}

	gSubGameNumber = gBootMpLevel ? gBootMpArena : 0;

	if (gMp.timeOfDay >= 0)
	{
		wantedTimeOfDay = gMp.timeOfDay;

		/* want-night picks WHICH take-a-ride LEVEL VARIANT is loaded: glaunch
		 * offsets the mission number by it, so a machine that left it at 1 from an
		 * earlier night run loads the night map while the session says day. Take it
		 * from the session, the same way main.c's -time boot does. */
		gWantNight = (gMp.timeOfDay == TIME_DUSK || gMp.timeOfDay == TIME_NIGHT) ? 1 : 0;
	}
	if (gMp.weather >= 0)
		wantedWeather = gMp.weather;

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] launching: city %d mode %d (1=TAKEADRIVE, 0=MISSION!) subgame %d map %s arena %d time %d weather %d players %d\n",
			GameLevel, GameType, gSubGameNumber, gBootMpLevel ? "MULTIPLAYER" : "full-city", gBootMpArena,
			wantedTimeOfDay, wantedWeather, NumPlayers);

	MpConnMatchStarted();	/* the stage stops lying here: we are IN the match now */

	MpMarkBusy(MP_BUSY_LAUNCH_MS);	/* the load is about to block us */
	/* A chosen vehicle, applied here rather than as a boot argument. The engine
	 * re-applies wantedCar to PlayerStartInfo immediately before the level runs,
	 * and it is only cleared on the way back to the frontend, so setting it now
	 * survives the launch. Slot 0 is this machine's own player.
	 *
	 * The value is resolved against the SESSION's city: "slotN" is the frontend's
	 * per-city car slot, so resolving it HERE (GameLevel is the host's city by
	 * now) is what makes the client's pick come from the host's car list rather
	 * than from whatever city this machine happened to boot with. A raw number
	 * the level cannot load is left alone -- InitPlayer clamps it to a resident
	 * car, so it is no longer a crash. */
	/* A car the player PICKED in the stock car select wins over everything below.
	 * The engine writes that pick to wantedCar[0] the moment they press Select
	 * (CarSelectScreen, FEmain.c), and mp used to overwrite it a moment later from
	 * its own config - which is exactly why "car selections aren't respected when
	 * joining": the choice was thrown away microseconds after it was made. A -car
	 * boot argument lands in the same variable, so that is respected too.
	 * -mpcar is now only the DEFAULT, for a machine where nobody picks: the
	 * launchers and the headless harness. */
	if (wantedCar[0] >= 0)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] using the car-select pick: model %d (city %d)\n",
				wantedCar[0], GameLevel);
	}
	else if (gMp.config.car >= 0)
	{
		int car = gMp.config.car;

		if (gMp.config.carIsSlot)
		{
			extern char carNumLookup[CITY_COUNT][10];
			int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
			int slotNo = (car >= 1 && car <= 10) ? car : 1;

			car = carNumLookup[lvl][slotNo - 1];

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] car slot %d -> model %d (city %d)\n",
					slotNo, car, GameLevel);
		}

		wantedCar[0] = car;
	}
	else
	{
		/* No pick (no -mpcar): take the SAME car the other machines will assign
		 * this player, so every machine agrees on who drives what. Leaving this to
		 * the engine's level default is what made the machines disagree (the
		 * client saw the host as slot 0, with a palette that matched a different
		 * model) and could make both cars identical.
		 *
		 * BUT ONLY IF THE LEVEL ACTUALLY HAS IT. wantedCar is the engine's LAST word
		 * before the level runs, so a number this level's pool does not carry does not
		 * stay a number: it becomes the level's own car of that number - the "it only
		 * did chicago 2 (domestic)" report - or a fallback to resident slot 0. The
		 * assignment is a position in the session (player 1 -> model 1), not a claim
		 * about this level, so leave wantedCar alone when the level cannot build it and
		 * let the level's own car stand in until the player picks. */
		int me = (gMp.localPlayerId >= 0) ? gMp.localPlayerId : 0;
		int assigned = MpAssignedCarModel(me);

		if (JerCarSlotUsable(assigned))
		{
			wantedCar[0] = assigned;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] no car chosen -> assigned model %d (player %d, city %d)\n",
					wantedCar[0], me, GameLevel);
		}
		else if (gMpCtx != NULL)
		{
			gMpCtx->jer_log(gMpCtx,
				"[mp] no car chosen and the session's model %d is not in this level - the level's own car stands in until they pick\n",
				assigned);
		}
	}

	/* The pick is only known NOW, so this is the first moment it can be sent.
	 * Refresh our own row too: the roster every machine reads should name the car
	 * we are actually driving. */
	{
		MP_PLAYER* me = MpLocalPlayer();

		if (me != NULL)
		{
			me->car = wantedCar[0];
			me->carIsSlot = 0;
			me->carCity = -1;	/* the pick is resolved against the session's city */
			me->carConfirmed = 1;
		}

		MpSendPickedCar(wantedCar[0], -1);
	}

	/* Log the host's own car sources. WITH -mpcar wantedCar[0] is the pick; without
	 * one it is the assigned model above -- and if NEITHER ran, the engine's level
	 * default decides, which is the case where the machines used to disagree. */
	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] car sources: config.car=%d isSlot=%d wantedCar[0]=%d startinfo[0]=%d\n",
			gMp.config.car, gMp.config.carIsSlot, wantedCar[0],
			(PlayerStartInfo[0] != NULL) ? PlayerStartInfo[0]->model : -9);

	/* ...and the whole assignment table for every player id, once per level. This
	 * is what makes "every player has a DISTINCT car, and every machine agrees on
	 * which" a line to read instead of something to infer from an N-seat run --
	 * and it is unconditional, because the N-seat harness runs --no-debug. */
	if (gMpCtx != NULL)
	{
		char line[256];
		size_t n;
		int id;

		n = (size_t)snprintf(line, sizeof(line), "[mp] assigned cars (city %d):", GameLevel);

		for (id = 0; id < MP_MAX_PLAYERS && n + 12 < sizeof(line); id++)
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %d:%d",
				id, MpAssignedCarModel(id));

		gMpCtx->jer_log(gMpCtx, "%s\n", line);
	}

	SetState(STATE_GAMESTART);
}

/* Host: broadcast the agreed config, then launch locally for everyone. */
/* ------------------------------------------------------------------ */
/* The roster                                                          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Seating every player: the level's cars, and its two spare slots      */
/* ------------------------------------------------------------------ */

/* The clearance a join must leave around every other player's car. A car is
 * about 1000 units long, so two cars this far apart cannot overlap, and a
 * joiner cannot materialise in somebody's braking distance. */

/* The map SHAPE of the session being set up: 0 = the city's large main map
 * (Single Player), 1 = its small multiplayer map. These are the engine's own
 * boot flags -- the launcher reads them (main.c -mp sets them; glaunch offsets
 * the mission number by gSubGameNumber, which MpLaunchLocal derives from them)
 * -- and the session publishes them to clients (MP_WELCOME.mpLevel/arena and
 * MP_SESSION.spare), so the HOST's menu choice reaches every machine's level
 * load. Kept behind a setter because the UI file must not poke boot flags.
 *
 * Without this the flags were only ever set by -mp on the command line, which is
 * why the Single Player / Multiplayer menu changed NOTHING: MpLaunchLocal
 * rewrote gSubGameNumber from gBootMpLevel either way, so both answers loaded
 * the same level. */
void MpSetMapShape(int mpLevel, int arena)
{
	gBootMpLevel = mpLevel ? 1 : 0;
	gBootMpArena = arena & MP_SESSION_ARENA_MASK;
}

/* ---- EXTRA ARENAS -----------------------------------------------------------
 * Driver 2 ships two multiplayer maps per city (arena 0 and 1, in MISSIONS.BLK).
 * Vegas has two more regions in its multiplayer level that only the 2-player
 * Checkpoint races reach; tools/make_vegas_arenas.py (run by BAKE_D1_CARS) writes
 * them as loose MISSIONS\M<n>.D2MS files, which LoadMission reads before the BLK.
 * Arena n of city c is mission 58 + c*2 + night + n*440 (glaunch.c).
 *
 * WITHOUT the files the BLK has no such entry and LoadMission reads garbage, so
 * every path that could load one (the menus, the votes, the rotation and the
 * launch itself) asks here first. */
#define MP_ARENA_CITY_VEGAS	2

static int MpArenaMission(int city, int night, int arena)
{
	return 58 + city * 2 + (night ? 1 : 0) + arena * 440;
}

int MpArenaCount(int city)
{
	return (city == MP_ARENA_CITY_VEGAS) ? 4 : 2;
}

int MpArenaInstalled(int city, int arena)
{
	static unsigned long checkedAt;
	static int have = -1;

	if (arena >= 0 && arena < 2)
		return 1;

	if (city != MP_ARENA_CITY_VEGAS || arena < 0 || arena >= MpArenaCount(city))
		return 0;

	/* the menus ask every frame: look at the disk at most once a second */
	if (have < 0 || MpNowMs() - checkedAt > 1000)
	{
		int a, night;
		char name[40];

		have = 1;
		for (a = 2; a < 4 && have; a++)
		{
			for (night = 0; night < 2 && have; night++)
			{
				snprintf(name, sizeof(name), "MISSIONS\\M%d.D2MS", MpArenaMission(city, night, a));
				if (!FileExists(name))
					have = 0;
			}
		}
		checkedAt = MpNowMs();
	}

	return have;
}

int MpArenaEveryoneHas(int city, int arena)
{
	/* Peer bits need the JPAR hello block (held: new optional wire field).
	 * Until that is approved, only this machine's files are checked; a peer
	 * without them falls back to MP 0 at launch. */
	return MpArenaInstalled(city, arena);
}

/* The name the frontend already printed for this city's multiplayer map.
 * Arenas 0 and 1 are the stock per-city list (FEscreens.inc screens 35-38,
 * the buttons Host Game still shows). Vegas 2 and 3 are the cut Ghost Town
 * / North Vegas regions only the 2-player Checkpoint races reached
 * (FEmain.c gameAreas[56-62] + EN_MISSION.LTXT; make_vegas_arenas.py).
 * NULL means no name -- the caller should print "mp N". */
const char* MpArenaTitle(int city, int arena)
{
	static const char* const names[4][4] = {
		{ "River North", "Cabrini Green", NULL, NULL },
		{ "Plaza de Revolucion", "Old Havana", NULL, NULL },
		{ "Downtown", "The Lakes", "Ghost Town", "North Vegas" },
		{ "Centro", "Flamengo", NULL, NULL },
	};

	if (city < 0 || city >= 4 || arena < 0 || arena >= 4)
		return NULL;
	return names[city][arena];
}

/* JOIN / LEAVE SOUNDS (0.23.3). The frontend's own menu samples, which mp asks
 * the core to keep resident in game (jer_sound_request_menu_bank, mp.c):
 * JER_MENU_SFX_ACCEPT (the frontend's Cross, FEmain.c FESound(2)) when a player
 * joins, JER_MENU_SFX_BACK (its Triangle, FESound(0)) when one leaves.
 *
 * Quiet for a server CITY CHANGE: the dedicated server kicks everyone with
 * MP_KICK_FLAG_REJOIN and every client comes back by itself. So:
 *   - our own rejoin (MpRejoinActive, then settling) and anything before the
 *     match is running on this machine (the roster batch a joiner receives is
 *     the players already there) never sound;
 *   - the names that were in the match when the REJOIN kick came are kept
 *     (gMpRejoinQuiet) and each one's first return within MP_REJOIN_QUIET_MS
 *     is silent - they reach the new city at different times;
 *   - a leave waits MP_JOINLEAVE_LEAVE_DELAY_MS: the server kicks the players
 *     one after another, so the ones it kicked first can drop off this
 *     machine's roster just before our own kick arrives, which cancels it. */

/* The PAUSE MENU's "Player options" act on the LOCAL player's own car and wanted
 * level, and only on this machine: they are the same per-machine switches the
 * engine's own cheats drive (gInvincibleCar / gPlayerImmune), applied to whoever
 * is sitting here. Nothing about them goes on the wire. */

/* Repair OUR car. The same field set the soft restart and sandbox's repair use,
 * plus CreateDentableCar -- totalDamage alone leaves the DRAWN mesh caved in. */
void MpSelfRepairCar(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
	{
		MpNotify("Repair: you are on foot with no car");
		return;
	}

	cp = &car_data[me->carId];

	/* The engine's own clean-car recipe (mission.c's respawn path): zero the
	 * damage rating, rebuild the dentable mesh from the clean model, then run one
	 * dent pass over the now-zero damage so the geometry and the damage-texture
	 * UVs settle back to pristine together. CreateDentableCar fires
	 * JER_EVENT_RESET_CAR itself (zeroing the crumple state before the clean
	 * copy), so there is no separate reset here. */
	cp->totalDamage = 0;
	memset(cp->ap.damage, 0, sizeof(cp->ap.damage));
	CreateDentableCar(cp);
	DentCar(cp);
	CarReapplyHubcapState(cp->id);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] player option: car repaired (slot %d)\n", me->carId);

	MpNotify("Car repaired");
}

/* Clear OUR wanted level. GetPlayerFelony picks the car's rating OR the
 * pedestrian's, so clear both -- leaving the other set puts the cops straight
 * back on us the moment we step out. */
void MpSelfClearFelony(void)
{
	MpClearFelony(1);
}

void MpClearFelony(int notify)
{
	MP_PLAYER* me = MpLocalPlayer();
	int i, released = 0;

	if (me != NULL && me->carId >= 0 && me->carId < MAX_CARS)
		car_data[me->carId].felonyRating = 0;

	pedestrianFelony = 0;

	/* AND THE PURSUIT ITSELF. Zeroing the rating stops NEW cars being sent, but the
	 * ones already on our bumper are CONTROL_TYPE_PURSUER_AI and would keep chasing
	 * a crime nobody remembers - "clear felony" that leaves three cop cars ramming
	 * you is not a clear.
	 *
	 * A VANILLA cop goes back to being TRAFFIC, which is what the engine does when
	 * a pursuit ends. InitCivState is the engine's own "this car is a civilian now":
	 * it flips controlType AND initialises the CIV_STATE fields CivControl reads. A
	 * bare controlType write is not enough - the AI state is a union, so a car
	 * switched that way runs civ AI on a cop's state.
	 *
	 * Left alone deliberately: anything without a siren (roadblocks, armoured vans,
	 * a mission car - not "just a vanilla cop"), and any car a PLAYER is driving,
	 * because that is somebody's car and not ours to convert. */
	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (cp->controlType != CONTROL_TYPE_PURSUER_AI)
			continue;

		if (!CarHasSiren(cp->ap.model))
			continue;

		if (MpGetPlayerByCar(i) != NULL)
			continue;

		InitCivState(cp, NULL);
		released++;
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] player option: wanted level cleared%s\n",
			(released > 0) ? " and the pursuit released back to traffic" : "");

	if (released > 0 && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] clear felony: %d cop car(s) back to civilian AI\n", released);

	if (notify)
		MpNotify("Wanted level cleared");
}

/* ------------------------------------------------------------------ */
/* Input lockstep                                                      */
/* ------------------------------------------------------------------ */

int MpInputForPlayer(int id)
{
	if (id < 0 || id >= MP_MAX_PLAYERS)
		return 0;

	return gMp.padForPlayer[id];
}

/* ------------------------------------------------------------------ */
/* Input replication                                                   */
/*                                                                     */
/* Every machine drives every car through the engine's own physics. A  */
/* client sends one row (its own pad) up to the host; the host merges  */
/* every row it has seen and broadcasts the whole set, so all machines */
/* simulate all cars. That is the point: engine-driven cars collide    */
/* with each other, and position puppets cannot.                       */
/*                                                                     */
/* Nothing here ever waits for a row. A car whose input has not        */
/* arrived simply repeats the last one, so a slow or lossy link costs  */
/* smoothness, never a stalled frame.                                  */
/* ------------------------------------------------------------------ */

/* A launch means the level is about to load, and a load blocks our own main loop
 * for its whole duration -- during which we neither send nor receive anything.
 * The peer sees exactly the same silence from a machine that has died, so it used
 * to drop us a second or two into a perfectly healthy session. Stand the idle
 * timeout down for the load. */
void MpMarkBusy(int ms)
{
	gMp.busyUntilMs = MpNowMs() + (unsigned long)ms;
}

int MpBusy(void)
{
	return gMp.busyUntilMs != 0 && MpNowMs() < gMp.busyUntilMs;
}

/* The old "meeting point" line-up (MpPlaceSpawns / 'JPSW') is GONE. It
 * teleported every car onto the host's spot carrying the host's single y, which
 * is exactly the trap in ARCHITECTURE section 4. Both machines already agree on
 * the engine's own deterministic spawn, and a client gathers itself beside the
 * host in MpHandleCarState, so there was nothing left for a host broadcast to
 * do. Do not reintroduce one. */

/* ------------------------------------------------------------------ */
/* Collisions, without giving up owner-authority                       */
/*                                                                     */
/* A machine can only move the ONE car it owns, so a naive contact      */
/* would see the follower's push overwritten by the owner's next state  */
/* (the "cars drive through each other" trade-off). Instead, when our   */
/* car touches a peer's we push OUR car and TELL THE PEER'S OWNER to    */
/* push theirs. Both cars actually move, and neither machine ever       */
/* writes a car it does not own.                                       */
/* ------------------------------------------------------------------ */

/* A push may only act on a RECENT pose of the target's own timeline. Twenty
 * frames is 667 ms at 30 Hz: enough for a 250 ms one-way link plus the 100 ms
 * playout buffer and a host relay, short enough that a hit from long ago cannot
 * move anybody. */

/* The largest impulse a WELL-BEHAVED sender can produce, per axis: the normal is
 * MP_FIXEDH-scaled and the push is capped at MP_HIT_MAX_PUSH. The wire carries an
 * int32, so without this ceiling a peer could add +/-2^31 to a car's velocity at
 * any distance -- see MpHandleHit. */

/* The hit counters go to the log once per this many sim frames. Every 300 frames
 * is 10 s at the engine's 30 Hz tick; the plan called this a per-minute line, but
 * mp_localpair runs are commonly 40 s, so a 60 s line would never appear in
 * exactly the runs that need it. */

/* MP_HEARTBEAT=<secs> -- once every N seconds, say that our sim tick is still
 * alive, with the frame the module has reached.
 *
 * This is the liveness signal a harness cannot otherwise get. EVERY other
 * periodic line in this module is MP_DEBUG-gated, so a --no-debug run (which is
 * what a player actually runs) is silent -- and silence from a frozen game is
 * indistinguishable from silence from a quiet one. A frozen run has already been
 * recorded here as a GOOD run, because every pass/fail marker a harness can see
 * is logged in the first seconds of a match:
 *
 *     [pair] verdict: ... dumps=0 -> PASS      <- the game froze at 6 s
 *
 * A line per second is nothing next to MP_DEBUG, and it is unconditional on
 * purpose: this is a testing lever, not debug spam. */
/* ------------------------------------------------------------------ */
/* The test levers, resolved ONCE                                     */
/* ------------------------------------------------------------------ */
/* Every tick below consults its lever EVERY simulation frame, and a libc getenv
 * per frame is pure overhead for a value that cannot change while the game runs.
 * Resolve them all at the first frame; the ticks then read a cached pointer, so
 * their behaviour and their log lines are unchanged. An inert lever stays NULL. */
const char* gHeartbeatStr;

/* ------------------------------------------------------------------ */
/* Traffic slot bands: the car_data slots THIS machine owns traffic in
 *
 * Player cars are replicated by MODEL (MP_CARSTATE_ENTRY carries no carSlot,
 * because a slot number means a different car on two machines). That trick only
 * works because there are a handful of players. Civilians are many, and each
 * machine grows its OWN population from its own random stream (PingInCivCar is
 * live because a match holds NumPlayers at 1), so before any state can be sent
 * the two machines must agree on WHERE a car lives -- and the only way to make
 * that agreement hold is to make sure the two spawners can never pick the same
 * slot.
 *
 * The foundation is a DISJOINT band per machine: machine `rank` may only spawn
 * traffic into [first..last], and every other traffic slot is kept reserved so
 * its own civ spawner cannot reach it. The peer then mirrors a car straight into
 * the slot its owner named.
 *
 * car_data[0..JER_SLOT_MANAGED_FIRST) stays with the players and the
 * JERICHO-managed slots above them (player id == car slot; see MpOnNetSpawn), so
 * the bands live entirely inside the pool the engine reserves for traffic
 * (JER_SLOT_TRAFFIC_FIRST..MAX_CARS, dr2limits.h). Bands are sized by how many
 * machines share the match: two split the pool in half, four in quarters, and the
 * last machine takes the remainder so no slot is orphaned.
 *
 * The POOL is a fixed size, so the number of machines never changes how much
 * traffic the map holds - only which machine spawns it.
 *
 * RE-ASSERTED EVERY FRAME, not set once: a level load ClearMem()s the engine's
 * reservedSlots (mission.c:301) out from under us, and the engine's own cutscene
 * recorder reserves from the low end -- so we manage ONLY the traffic band, and
 * only the bits WE set, never the engine's. */

/* ------------------------------------------------------------------ */
/* The spawn CLAIM: a patch of world belongs to the nearest player
 *
 * Each machine spawns civilians around ITS OWN player (PingInCivCar anchors on
 * MainPlayer.spoolXZ), so two players standing together would both grow a
 * population in the same place. The band keeps the two spawners in different
 * car_data SLOTS; it does not keep them out of the same PLACE.
 *
 * The rule that does: a car belongs to whichever player is nearest to it. A
 * machine KEEPS a civilian in its band only while its own player is that nearest
 * player; if a peer's player is nearer, the peer owns that patch and we ping our
 * copy out -- the loser pings its copy out. Both machines evaluate the SAME
 * positions (every player's car is replicated, so car_data holds them all), so
 * they agree; a DEADBAND plus a lowest-player-id tie-break stops a car sitting on
 * the midpoint from flapping between owners.
 *
 * Until traffic state is mirrored between the machines (the next phase), a culled
 * car is not replaced by the peer's copy yet, so a shared patch is briefly thinner
 * than it will be -- the claim is what makes the two populations disjoint in the
 * first place. */

/* One network tick (per simulation frame). Each machine owns ITS OWN car and
 * broadcasts that; everyone else adopts it, so nobody blocks on the network and
 * every car you see is the truth of the machine driving it. */
/* ------------------------------------------------------------------ */
/* Remote-car interpolation                                            */
/*                                                                     */
/* A snapshot is a SAMPLE of the owner's 30 Hz timeline, not a pose to  */
/* write the moment it lands. The receiver keeps a short ring of them   */
/* and plays them back a fixed 3 frames (100 ms) late, so the remote    */
/* car moves continuously between packets instead of stepping once per  */
/* arrival. The engine still simulates the car every frame; this only   */
/* replaces the hard write that used to happen in MpHandleCarState.     */
/*                                                                     */
/* The cursor advances one owner-frame per sim frame (both machines run */
/* the same fixed 30 Hz tick). It is clamped at newest + EXTRAP_MAX so  */
/* a link stall cannot run it away, and re-anchored when it falls more   */
/* than RESYNC frames behind -- which is what a long stall or lost       */
/* burst looks like when the samples finally arrive.                     */
/* ------------------------------------------------------------------ */

/* MP_CAR_SNAP / MP_CAR_PLAY / gMpPlay are declared at the top of the file: the
 * contact path reads the playout state before this code in file order. */

/* ------------------------------------------------------------------ */
/* Our own pose history                                                */
/*                                                                     */
/* A push names the frame of OUR carstate it collided with. To decide   */
/* whether we were really where it says, we need where we were at that  */
/* frame -- recorded as the pose we sent, because that is exactly what  */
/* the sender could have been displaying. 64 frames is just over two    */
/* seconds at 30 Hz, comfortably more than the validity window.         */
/* ------------------------------------------------------------------ */


void MpLockstepFrame(void)
{
	if (!gMp.running)
		return;

	gMpStage = "sim";

	++gMp.frame;

	/* Place every remote car from its snapshot buffer BEFORE anything is
	 * simulated or sent. This is the one writer of a remote body now: the
	 * playout cursor turns the owner's 30 Hz samples into continuous motion,
	 * and the engine steps the car from there for this frame. */
	MpApplyRemoteCarPoses();

	/* A LIVE JOINER LEAVES THE LEVEL START HERE, BEFORE ANY STATE IS SENT. The
	 * engine builds its car on the level's own start -- where the host came in --
	 * and the owner-authoritative carstate goes out from MpSendOwnCarState later
	 * in this same tick. Placing now means the peers never see the start pose at
	 * all; the old FRAME-hook call ran after the send, and a second state could
	 * overwrite an earlier successful placement. */
	MpPlaceSelfTick();

	/* Re-assert the traffic slot bands FIRST, before anything spawns this frame. A
	 * level load clears the engine's reservedSlots out from under us, so this is
	 * not a one-time setup -- see MpTrafficBandRefresh. */
	MpTrafficBandRefresh();

	/* ...and then give up any civilian of ours that a peer's player now owns, so
	 * two players standing together do not both grow traffic in the same patch. */
	MpTrafficClaimPass();

	/* ...and collect any mirror its owner has stopped speaking about. */
	MpTrafficMirrorAgeTick();

	/* ...and keep a peer's on-duty police car lit and sounding, which its own
	 * engine cannot do for a car it does not own. */
	MpMirrorSirenTick();

	/* the test levers are read from the environment ONCE, not every frame (see
	 * MpResolveTestLevers); the ticks below then only consult the cache */
	MpResolveTestLevers();

	/* test lever: prove the sim tick is still running (MP_HEARTBEAT) */
	MpHeartbeatTick();

	/* test lever: a scripted mid-session car change (inert unless MP_TEST_CARCHANGE) */
	MpTestCarChangeTick();
	MpTestCityChangeTick();
	MpTestCarCycleTick();
	MpTestReloadCarsTick();

	/* test lever: a synthetic traffic contact, to exercise the traffic-hit wire path
	 * without a physical collision (inert unless MP_TEST_TRAFFIC_HIT). */
	MpTestTrafficHitTick();

	/* test lever: keep a wanted level up so the ENGINE spawns police cars and they
	 * pursue (inert unless MP_TEST_FELONY). See MpTestFelonyTick. */
	MpTestFelonyTick();

	/* test lever: drop our car off the map, to exercise the game-over refusal
	 * (inert unless MP_TEST_FALLOFF). */
	MpTestFallOffTick();

	/* test lever: a clean leave part-way through (inert unless MP_TEST_LEAVE), so
	 * that "a deliberate quit" and "a connection died" can be told apart in a
	 * log without a human sitting at the menu. */
	MpTestLeaveTick();

	/* test lever: the HOST kicks a client part-way through (inert unless
	 * MP_TEST_KICK), so the kick wire path + the client's clean teardown is
	 * reproducible without a human at the pause menu. */
	MpTestKickTick();

	/* test lever: get out of the car part-way through (inert unless MP_TEST_ONFOOT),
	 * so the on-foot path has a way to be exercised at all. */
	MpTestOnFootTick();
	MpTestEnterLeftTick();

	/* test lever: import Jericho at a fixed frame, with or without a session, so
	 * the model/page import can be exercised headlessly (inert unless
	 * MP_TEST_JERICHO). */
	{
		const char* tj = getenv("MP_TEST_JERICHO");

		if (tj != NULL && tj[0] != '0' && gMp.frame == 120)
		{
			int ok = MpEnsureJerichoImported() && MpEnsureJerichoInstalled();

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_JERICHO import %s (level %d)\n",
					ok ? "succeeded" : "FAILED", GameLevel);
		}
	}

	/* test lever: run the pause menu's Change car path (inert unless
	 * MP_TEST_PAUSECAR), so a mid-match vehicle change is reproducible headlessly
	 * rather than only by a human at the menu. */
	MpTestPauseCarTick();

	/* test lever: ask for the multiplayer Restart (inert unless MP_TEST_RESTART),
	 * through the engine's own event so the whole chain is under test. */
	MpTestRestartTick();

	/* tell everyone where our wheel is pointing before anything is simulated */
	MpSendInput(MpLocalPad());

	/* Snapshots correct drift across the two simulations; they are NOT the
	 * pose. Placing every car every frame is what made them puppets and threw
	 * away collision responses, so they go out on an interval instead. */
	/* refreshed so the pause menu's names/vehicles/ping are live, and so a
	 * player joining a match already in progress learns the roster */
	if ((gMp.frame % 120) == 0)
	{
		MpHostSendRoster();
		MpHostSendStats();
	}

	/* the timer modes' referee first: it may begin or end the round */
	MpHostMarkTick();

	/* the round state: on the same cadence, and at once when someone joins or leaves */
	MpHostRoundTick((gMp.frame % 120) == 0);
	MpClientRoundCheck();

	/* ...and what this machine shows of a timer mode (nothing in Take a Ride) */
	MpMarkPresentTick();

	/* the other players' cop cars draw their sight cones on the maps */
	MpMapCopsTick();

	/* the same cadence, and for the same reason: a machine that joins late or
	 * misses one still ends up with everybody's colour */
	if (MpIsHost() && (gMp.frame % 120) == 0)
		MpSendColors(1);

	/* AND RE-SEAT ANYONE WHO HAS NO CAR - a sharper reason, and a faster cadence.
	 *
	 * A player who gets OUT of their car and back IN keeps their identity (they still
	 * drive the car they chose) but their CAR_DATA slot is released when they walk.
	 * On every other machine that means carId goes to -1 and their roster row names
	 * 0xff, so they are not drawn. Getting back in clears onFoot and re-announces the
	 * car - but the pass that GIVES a player a car was a LATE-JOINER pass: it ran once,
	 * from the one-shot spawn request (mp.c), so a returning player was never visited
	 * again and stayed invisible on everybody else's screen for the rest of the match.
	 * That is the permanent desync a two-machine LAN game reported: his car never came
	 * back on mine, and mine never on his.
	 *
	 * Faster than the roster because this is a player waiting to be SEEN rather than
	 * bookkeeping - about a second. It costs nothing when nobody needs seating: the
	 * pass skips every seated player, and every on-foot one, on its first line. That
	 * on-foot guard is load-bearing and must stay: re-seating a player who is walking
	 * about is what made the stand-in pedestrian flicker. */
	if ((gMp.frame % 30) == 0)
		MpSpawnLateJoiners();

	{
		/* our own colour, whenever it stops matching what we last sent */
		static int lastOn = -1, lastR = -1, lastG = -1, lastB = -1;
		static int lastModel = -1;

		if (gMp.config.colorOn != lastOn || gMp.config.colorR != lastR ||
		    gMp.config.colorG != lastG || gMp.config.colorB != lastB ||
		    gMp.config.pedModel != lastModel)
		{
			lastOn = gMp.config.colorOn;
			lastR = gMp.config.colorR;
			lastG = gMp.config.colorG;
			lastB = gMp.config.colorB;
			lastModel = gMp.config.pedModel;

			MpSendColors(0);
		}
	}

	/* OWNER-AUTHORITATIVE: every machine sends the ONE car it owns, every frame,
	 * and everyone else adopts that state. The old model had each machine simulate
	 * every car from input that arrived a round trip late and hoped a coarse resync
	 * would pull them back together -- which is exactly where the drift came from. */
	MpSendOwnCarState();

	/* ...and record the pose we just sent. A contact report names one of these
	 * frames; this history is how we check that we were really there. */
	MpRecordSelfPose();

	/* ...and the pedestrian, if we are not in a car: the same owner-authoritative
	 * rule, for a thing that has a heading and a speed instead of a body. */
	MpSendOwnPedState();

	/* ...and the civilians and police OUR band owns. A row names the car_data slot
	 * the car lives in -- our band is disjoint from the peer's, so that slot IS the
	 * shared identity here (unlike a player car, matched by model), and the peer
	 * mirrors the row straight into it. */
	MpSendTrafficState();

	/* Keep any remote player's stand-in pedestrian in step with its owner. On a sim
	 * frame, because this touches the ped table and the network callbacks must not. */
	{
		int pi;

		for (pi = 0; pi < MP_MAX_PLAYERS; pi++)
			MpDriveRemotePed(&gMp.players[pi]);
	}

	/* ...and make sure no car of ours has been handed to the traffic AI behind our
	 * back, which it cannot survive (see the function). */
	MpKeepOurCarsFromTrafficAi();

	/* owner-authoritative contacts: push ourselves, tell the peer's owner */
	MpHitFrame();

	MpNetPoll(0);

	/* ...and if this is a summary frame, the hit counters go to the log. */
	MpHitSummary();

	gMpStage = "sim-done";
}

/* A SATURATING cast to int16, for the wire fields that are still 16-bit.
 *
 * The orientation is a quaternion whose components are normally small, but a
 * violent impact can push one past 32767 -- and a WRAPPING cast turns that into a
 * completely different orientation on the peer, which is how a crash becomes a
 * car pointing somewhere absurd. Clamp instead: a saturated extreme is at least
 * in the right direction. */
int16_t MpSat16(long v)
{
	if (v > 32767)
		return 32767;

	if (v < -32768)
		return -32768;

	return (int16_t)v;
}

/* A ceiling for an ADOPTED angular velocity. The engine's own values reach about
 * 1.5 million, so this is far above anything legitimate and never touches a real
 * value -- it only stops a corrupt or hostile snapshot from handing the physics an
 * absurd spin to digest. */
#define MP_ANGVEL_LIMIT	(8L * 1024L * 1024L)

long MpClampAngVel(long v)
{
	if (v > MP_ANGVEL_LIMIT)
		return MP_ANGVEL_LIMIT;

	if (v < -MP_ANGVEL_LIMIT)
		return -MP_ANGVEL_LIMIT;

	return v;
}

/* ------------------------------------------------------------------ */
/* FOLLOWING THE CAR THE PLAYER IS ACTUALLY DRIVING.
 *
 * Getting out of a car and into another is the ENGINE's own Take a Ride
 * mechanic -- ChangePedPlayerToCar / ChangeCarPlayerToPed (players.c) mutate
 * player[] and the car's pad link IN PLACE and never call InitPlayer, so no spawn
 * path sees it. Nothing tells us either (there is no enter/exit event), so we
 * watch the engine's own player[0].playerCarId (a char; -1 = on foot) and adopt
 * whatever it says.
 *
 * player[0] is always US: every machine runs its one local player in engine slot
 * 0, and the REMOTE players live in the higher slots the mod inits itself. */
void MpFollowLocalCar(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	int driven;

	if (me == NULL || !me->isLocal || !gMp.running)
		return;

	driven = (int)player[0].playerCarId;	/* -1 = on foot */

	if (driven == me->carId)
		return;

	if (driven >= 0 && driven < MAX_CARS)
	{
		CAR_DATA* cp = &car_data[driven];

		/* VALIDATE THE DESTINATION BEFORE MOVING OUR IDENTITY ONTO IT.
		 * A slot the engine cleared (PingOutCar) has ap.carCos == NULL and no model;
		 * a slot whose resident model was released has no mesh. Adopting either as
		 * "what we drive" hands the session a car that cannot exist - and the world
		 * still walks every live CAR_DATA in SetUpCivCollFlags, which reads
		 * ap.carCos->colBox.vz at 0xD8. Leave the identity where it was and say so
		 * once per slot; the engine cannot make a valid player car out of this. */
		if (cp->ap.carCos == NULL ||
			cp->ap.model < 0 || cp->ap.model >= MAX_CAR_RESIDENT_MODELS ||
			!JerCarSlotUsable(cp->ap.model))
		{
			static int sMpBadSeat = -1;

			if (gMpCtx != NULL && sMpBadSeat != driven)
			{
				sMpBadSeat = driven;
				gMpCtx->jer_log(gMpCtx,
					"[mp] car change: slot %d has no usable car (model %d, cosmetics %s) - not adopted\n",
					driven, (int)cp->ap.model, (cp->ap.carCos != NULL) ? "present" : "NULL");
			}

			return;
		}

		/* Back in a car after being ON FOOT (the flag is set below, when we got out): the
		 * player chose this car by walking up to it, so it is what the session must be
		 * told we drive - possibly not the car we left. The FIRST seat of a match (and a
		 * late joiner's) never comes through here with the flag set, which is what keeps
		 * the engine's seat from replacing a chosen identity (carhacks/net.c,
		 * chkNetLocalCar). */
		int reentry = me->onFoot;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] car change: now driving slot %d model %d (was slot %d)\n",
				driven, cp->ap.model, me->carId);

		me->carId = driven;
		me->car = (cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS)
			? residentCarModels[cp->ap.model] : cp->ap.model;
		me->carIsSlot = 0;		/* 'car' is a real model NUMBER now, not a slot */
		me->carCity = GetCarModelSourceCity(cp->ap.model);
		me->palette = cp->ap.palette;
		me->onFoot = 0;
		MpFailsafeNoteCar(cp->ap.model);

		if (reentry)
		{
			MpCarQueryChosen(me->carCity, me->car, 1);

			/* CLOSE THE ROSTER WINDOW. The carstate already carries
			 * MP_CARSTATE_SAME_BODY on this same frame (MpSendOwnCarState), but the
			 * roster only goes out every 120 frames. A host that has just got back
			 * in should publish its row now rather than leave peers reading the
			 * old on-foot row for up to two seconds -- that window is exactly where
			 * a lost/reordered carstate used to leave them stuck. Clients have no
			 * roster to send: their pick already travels through MP_CARQ, and the
			 * host republishes. */
			if (MpIsHost())
				MpHostSendRoster();
		}
	}
	else if (me->carId >= 0)
	{
		/* ON FOOT. The car we left stays in the world as a stopped, empty civ
		 * car (below) so it can be walked back to and re-entered. We only report
		 * "no car" (model 0xFF) so the peers let theirs go too. */
		CAR_DATA* left = &car_data[me->carId];

		/* Leave the car as the stopped, empty civ car ChangeCarPlayerToPed made
		 * it, NOT CONTROL_TYPE_PLAYER.
		 *
		 * ChangeCarPlayerToPed sets controlType = CIV_AI with
		 * thrustState = CIV_AI_THRUST_STOP / ctrlState = CIV_AI_CTRL_EMPTY. In
		 * that state the civ-AI pass is harmless even though the car has no nav
		 * data: CivControl's STOP branch does no work (CivAccelTrafficRules'
		 * STOP case is an empty break, and ctrlState EMPTY takes the safe
		 * default), so it never dereferences the uninitialised ai.c node/lane
		 * fields. And CIV_AI is exactly what TannerCanEnterCar accepts, so the
		 * player can walk back and get in again -- CONTROL_TYPE_PLAYER kept the
		 * car out of the traffic AI but also made it un-enterable, which is the
		 * "can't return to my car" report. Re-assert the stopped/empty state and
		 * clear the wheel, and take its pad away so nothing drives it.
		 *
		 * This runs from PRE_SIM, which is BEFORE the civ-AI pass in the same
		 * frame, so the car is already in its final state when the AI looks. */
		left->controlType = CONTROL_TYPE_CIV_AI;
		left->ai.c.thrustState = CIV_AI_THRUST_STOP;
		left->ai.c.ctrlState = CIV_AI_CTRL_EMPTY;
		left->wheel_angle = 0;
		left->ai.padid = &gMpQuietPad;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] car change: on foot (left slot %d, parked empty for re-entry)\n",
				me->carId);

		/* REMEMBER THE BODY WE ARE STEPPING OUT OF, on our OWN row. Two things need
		 * it: MpSendOwnCarState tells the peers when we walk back into this same
		 * slot (MP_CARSTATE_SAME_BODY) so they re-seat us on the body they released
		 * instead of spawning a second one, and MpChangeCarRun's on-foot path
		 * prefers this car over any other parked one. It is only meaningful while
		 * me->carId == leftCarSlot1 - 1; entering a DIFFERENT car leaves it stale
		 * on purpose, and the next get-OUT overwrites it. */
		me->leftCarSlot1 = me->carId + 1;
		me->leftCarModel = left->ap.model;

		me->carId = -1;
		me->car = -1;
		me->onFoot = 1;		/* OUR row: the next car we get into is a choice (above) */

		/* Publish the on-foot row now for the same reason as re-entry below: the
		 * carstate says NO_CAR from this frame on, but the roster would otherwise
		 * name the car we just left for up to 120 frames. A peer that missed the
		 * transition then sees our old car as still driven. Host only -- a
		 * client's rows reach the others through the host's roster. */
		if (MpIsHost())
			MpHostSendRoster();
	}
}

/* ------------------------------------------------------------------ */
/* ON FOOT                                                            */
/*                                                                    */
/* A player who is not in a car has to look like a PERSON to everyone  */
/* else, not a gap where a car used to be. The engine cannot hand us a */
/* remote machine's character -- only the machine that owns it has it  */
/* -- so we stand a pedestrian in for it: a stock NPC of the Tanner    */
/* model (jer_npc model 0, which the ambient system will NOT recycle   */
/* the way it recycles civilians), and tell it where to stand and how  */
/* fast to move.                                                       */
/*                                                                    */
/* Driving it by SPEED rather than posing it by hand is the point: the */
/* engine then animates the walk, the run and the stand for us, so the */
/* legs are always right and there is no per-frame pose to invent.     */

/* ------------------------------------------------------------------ */
/* PREFERRED CHARACTER                                                */
/*                                                                    */
/* The owner chooses; this file owns the switch. Tanner and Jericho   */
/* are the two SKELETON characters, so changing one is a rebind of the */
/* same bones and vertex lists InitTanner built. The palette is not  */
/* touched: a ped's colour is chosen per draw from JER_EVENT_PED_DRAW, */
/* and re-selecting it here only makes the first frame after a swap   */
/* correct.                                                           */
/*                                                                    */
/* Jericho's JERI_* models belong to Old Havana's model lump. The      */
/* pointers, not the city number, are the authority: before the level */
/* has read its models they are NULL, and drawing one would crash.    */
/* ------------------------------------------------------------------ */

/* WHICH CAR SLOTS ARE OURS.
 *
 * Sticky: once the module has put a player in a car, that slot is a module car for
 * the rest of the session, even after the player gets out of it. That matters
 * because the car is then still in the world with nobody driving it, and the
 * engine's traffic AI treats such a car as fair game (see the top of the file). */

/* ------------------------------------------------------------------ */
/* COLOUR                                                              */
/*                                                                    */
/* Only a player may say what their character looks like, so a colour  */
/* is sent by its OWNER and by nobody else. The one exception is the   */
/* host fanning out a whole table when somebody joins, or on a slow    */
/* timer, so a machine that missed one still ends up right.            */
/*                                                                    */
/* `on` is the feature's whole switch, and it DEFAULTS OFF: with it off */
/* a character keeps the colours the game gave it. That default matters */
/* -- it means a match looks stock until someone asks for otherwise.    */

int MpColorLen(int count)
{
	return (int)(sizeof(MP_COLOR) + (size_t)count * sizeof(MP_COLOR_ENTRY));
}

/* ------------------------------------------------------------------ */
/* The owner's per-car body-crush multiplier.
 *
 * Every machine simulates every player car, so the deformation has to use the
 * SAME multiplier on all of them or the same crash reads differently on two
 * screens. The value is per CAR (not per player): the owner's carstate carries
 * it in half-steps (2 = 1.0x, 20 = 10.0x), and this side hands it to crumple
 * for the car slot that player is driving. No crumple module = the SET is
 * unanswered and the car keeps 1.0x, which is the stock look. */

/* ------------------------------------------------------------------ */
/* The REMOTE player's car: keeping it on the vehicle its owner is driving.
 *
 * A player who gets out of one car and into another changes vehicles mid-session
 * (see MpFollowLocalCar). The carstate carries the model now, so we can follow
 * it; these helpers make our copy match. */

/* The MODE a server plays, named from its GAMEMODE id (MP_GAMEMODE_*, carried by
 * MP_BEACON / MP_WELCOME / the master listing). Session rules (cops on/off, etc.)
 * are settings OF a mode and never rename it: Take a Ride with cops off is still
 * Take a Ride. An id this build does not know reads "-". Keep it in step with the
 * tools' mode_name() (mp_test.py). */
const char* MpModeName(int gamemode)
{
	/* The registry owns the names now (mp_mode.c); "-" for an id it does not know,
	 * exactly as before. */
	return MpModeDisplayName(gamemode);
}

/* ------------------------------------------------------------------ */
/* Asking the other modules about live cars                            */
/* ------------------------------------------------------------------ */
/* jer_carquery.h is the contract: a custom JERICHO event, so this works whether
 * carhacks is compiled in beside us or loaded as a dll, and degrades to "nobody
 * knows" when it is not there at all. */


/* jer_carquery.h's pools are mp_mode.h's: one id space on both sides */
typedef char MpRolePoolAny[(JER_CARQ_POOL_ANY == MP_MODE_POOL_ANY) ? 1 : -1];
typedef char MpRolePoolPolice[(JER_CARQ_POOL_POLICE == MP_MODE_POOL_POLICE) ? 1 : -1];
typedef char MpRolePoolCivilian[(JER_CARQ_POOL_CIVILIAN == MP_MODE_POOL_CIVILIAN) ? 1 : -1];
typedef char MpRoleSlotsFit[(MP_MODE_ROLE_SLOTS_MAX <= JER_CARQ_ROLES_MAX) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* A CHANGE CAR THAT HAD TO WAIT                                        */
/*                                                                      */
/* JER_CARQ_LOAD can legitimately answer "not yet". The pool is six      */
/* spare resident slots (CHK_IMPORT_SPARE_FIRST .. CHK_IMPORT_MAX_SLOTS) */
/* and a change HOLDS its old slot until the release judges it free, so  */
/* with several players changing at once the canonical slot can be busy  */
/* for a frame or two. Measured: three seats changing every 700 ms       */
/* saturate the pool exactly -- 3 x (held old + wanted new) = 6 -- and   */
/* picks were refused.                                                   */
/*                                                                      */
/* Waiting is the right answer, and it is safe because a change is only  */
/* ADVERTISED once it has happened (MpCarQueryChosen on success), so a   */
/* frame or two of waiting here is invisible to the other machines: no   */
/* wire change and no determinism change. What is NOT safe is carrying   */
/* on and adopting the car regardless -- then the car points at a        */
/* resident model whose mesh this machine does not hold, which IS the    */
/* invisible car the instrument below describes, and a crash if the      */
/* model is not resident at all.                                         */
/*                                                                      */
/* Bounded, because a slot can also be held for a reason that outlives   */
/* the change; after MP_CHANGE_WAIT_FRAMES the wait is dropped and the   */
/* refusal is reported like any other.                                   */
/* ------------------------------------------------------------------ */

/* Put OUR player back at the level's own start, in their car, repaired and with
 * no felony -- the MULTIPLAYER meaning of Restart (see MpOnGameQuit in mp.c).
 *
 * The ENGINE's restart throws the level away and rebuilds it, which in a match is
 * not one player's to do: everyone else is in that same level and their session
 * is not ours to reset. So the local player is put back at the start instead, and
 * the others simply see their car arrive there -- the owner-authoritative
 * carstate carries the new pose like any other move.
 *
 * Returns 1 when the reset was applied. */
int MpSoftRestart(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;

	if (me == NULL || !gMp.running)
		return 0;

	/* Being in a vehicle is the point of the request, so on foot we get back into
	 * our parked car first -- the same rule a car change uses. */
	if (me->carId < 0)
	{
		int seat = -1;
		int s;

		for (s = 0; s < MAX_CARS; s++)
		{
			if (!gMpOurCars[s] || MpGetPlayerByCar(s) != NULL)
				continue;

			/* Same destination check as the Change car path: a cleared slot is not
			 * a car, and a car without cosmetics or a usable resident model cannot
			 * be put the player in (SetUpCivCollFlags reads carCos at 0xD8). */
			if (car_data[s].controlType == CONTROL_TYPE_CIV_AI &&
				car_data[s].ap.carCos != NULL &&
				JerCarSlotUsable(car_data[s].ap.model))
			{
				seat = s;
				break;
			}
		}

		if (seat < 0)
		{
			MpNotify("Restart: you are on foot with no car to get back into");
			return 0;
		}

		ChangePedPlayerToCar(0, &car_data[seat]);
		MpFollowLocalCar();
	}

	if (me->carId < 0 || me->carId >= MAX_CARS)
		return 0;

	cp = &car_data[me->carId];

	/* The level's own start for this player (PlayerStartInfo[0] is us), placed THE ENGINE'S
	 * OWN WAY. A car's orientation is a rigid-body QUATERNION (st.n.orientation) and its
	 * position a fixed-point fposition; hd.where and hd.where.m are DERIVED from those by
	 * RebuildCarMatrix. Writing only hd.where - as this used to - leaves the body where it
	 * was and at the roll it had, so the next physics step snaps the car back onto the old
	 * state: the reported "they respawn upside down", and a car that can drop out of the
	 * world. InitCarPhysics writes the whole state the way a level-built car gets it, and
	 * the AI already uses it to place cars (civ_ai.c, leadai.c). */
	if (PlayerStartInfo[0] != NULL)
	{
		LONGVECTOR4 start;
		int sx = PlayerStartInfo[0]->position.vx;
		int sz = PlayerStartInfo[0]->position.vz;

		/* STREAM THE DESTINATION FIRST, or a restart from far away is a restart into the
		 * void. Put back at the level's start after driving off, this is a long
		 * teleport - and a region you are dropped into is never put into a barrel by the
		 * streamer, so the ground there never loads and MapHeight answers 0. The car is
		 * then placed with start[1] = 0 over nothing and falls out of the world. Same
		 * contract as the hop below, including the static: spoolXZ must outlive the call.
		 *
		 * Only stream when the region is genuinely NOT resident. jer_map_spool_to runs a
		 * synchronous spool pass (CheckLoadAreaData + StartSpooling + UpdateSpool), and
		 * calling it when the region is already in stalled both a pair run and a 3-seat
		 * run - that is what the gather's own note records as measured. A restart taken
		 * near the start costs nothing here. */
		if (jer_map_ready())
		{
			int region = jer_map_region_of(sx, sz);

			if (region >= 0 && jer_map_region_has_data(region) &&
				!jer_map_region_resident(region))
			{
				static VECTOR sRestartSpool;	/* spoolXZ must outlive this call */
				VECTOR* oldSpoolXZ = MainPlayer.spoolXZ;

				sRestartSpool.vx = sx;
				sRestartSpool.vy = 0;
				sRestartSpool.vz = sz;
				sRestartSpool.pad = 0;
				MainPlayer.spoolXZ = &sRestartSpool;

				/* 0 = there is nothing there to stream, so we must not place at all:
				 * leaving the player where they are beats dropping them into a void
				 * they cannot drive out of. */
				if (!jer_map_spool_to(sx, sz))
				{
					MainPlayer.spoolXZ = oldSpoolXZ;
					MpNotify("Restart: the level start is not streamable - staying put");
					return 0;
				}

				/* Hand the streamer back: the engine's own convention is the player
				 * car's position vector (cutscene.c points spoolXZ at it), which is the
				 * car we are moving, so it already follows us. Ours pointed at a static. */
				MainPlayer.spoolXZ = oldSpoolXZ;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] soft restart: region %d was not resident at %d,%d - streamed it before placing\n",
						region, sx, sz);
			}
		}

		start[0] = sx;
		start[1] = 0;		/* the engine resolves the ground under the car */
		start[2] = sz;
		start[3] = 0;

		InitCarPhysics(cp, &start, PlayerStartInfo[0]->rotation);

		/* The camera angle is an ABSOLUTE world yaw, so it has to be re-seated against the
		 * car's new heading - the engine's own rule when a player gets into a car
		 * (players.c: cameraAngle = newCar->hd.direction + 1536). Left as it was, the view
		 * keeps aiming from the heading the player was respawned away from, which reads as
		 * the camera flipping and rolling. */
		player[0].cameraAngle = cp->hd.direction + 1536;
		player[0].cameraCarId = cp->id;
	}

	/* Stopped dead: a restart is not a momentum transfer. (InitCarPhysics has already
	 * zeroed the body's linear/angular velocity; hd.speed is not one of its fields.) */
	cp->hd.speed = 0;
	cp->wheel_angle = 0;
	memset(cp->st.n.linearVelocity, 0, sizeof(cp->st.n.linearVelocity));
	memset(cp->st.n.angularVelocity, 0, sizeof(cp->st.n.angularVelocity));

	/* REPAIRED -- the fields a repair touches (the same set sandbox's repair uses),
	 * plus CreateDentableCar, which is the only thing that rebuilds the DRAWN
	 * vertices from the clean model. Without it the car is undamaged and still
	 * looks caved in. */
	cp->totalDamage = 0;
	memset(cp->ap.damage, 0, sizeof(cp->ap.damage));
	CreateDentableCar(cp);
	CarReapplyHubcapState(cp->id);
	cp->ap.needsDenting = 0;

	{
		JER_ARGS_RESET_CAR rc;

		rc.carId = me->carId;
		jer_fire(JER_EVENT_RESET_CAR, &rc);
	}

	/* NO FELONY. GetPlayerFelony picks the car's rating or the pedestrian's, so
	 * clear both: leaving the other one set would put the cops straight back on us
	 * the moment we stepped out. */
	cp->felonyRating = 0;
	pedestrianFelony = 0;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] soft restart: back at the level start %d,%d in slot %d, repaired, felony cleared"
			" (body q %d,%d,%d,%d, fpos %d,%d,%d)\n",
			cp->hd.where.t[0], cp->hd.where.t[2], me->carId,
			cp->st.n.orientation[0], cp->st.n.orientation[1],
			cp->st.n.orientation[2], cp->st.n.orientation[3],
			cp->st.n.fposition[0], cp->st.n.fposition[1], cp->st.n.fposition[2]);

	MpNotify("Restarted: back at the start, car repaired, wanted level cleared");

	return 1;
}

/* ------------------------------------------------------------------ */
/* WARP TO PLAYER                                                      */
/*                                                                     */
/* Put this machine's car (or its own on-foot pedestrian) down near      */
/* another player's: close enough to be useful, out of the path they are  */
/* already committed to, and on ground it can actually stand on.          */
/*                                                                        */
/* On foot the same search runs and the local player's own pedestrian is  */
/* moved. The TARGET may be on foot as well: their pose arrives in        */
/* MP_TAG_PED, so nobody needs a car to be warped to.                     */
/*                                                                     */
/* The spot is CHOSEN, not guessed. A fixed ring of candidate offsets  */
/* around the target is walked in order and the first that is clear, on */
/* real ground and OFF their trajectory wins, with a road preferred to  */
/* a shoulder. Two of those are the whole feature:                     */
/*                                                                     */
/*   - NEVER AHEAD. Every offset is behind or beside, so the arriving  */
/*     car cannot materialise inside the target's braking distance.    */
/*     That is the "instantly clipping" case, and it is handled by      */
/*     which candidates exist rather than by a fudge factor.           */
/*                                                                     */
/*   - SPOOL BEFORE ASKING THE GROUND. MapHeight answers 0 for ground  */
/*     that is not resident, and 0 reads as a perfectly good height,   */
/*     so testing candidates before jer_map_spool_to has streamed the  */
/*     destination is exactly how a warp lands in the void.            */
/*                                                                     */
/* Placed the way the engine places a car (InitCarPhysics: the rigid-body
/* quaternion and the fixed-point fposition, NOT hd.where) and for the same
/* reason the respawn above had to learn. The camera is re-seated with it,
/* because cameraAngle is an absolute world yaw. */

/* How far from the target the near/far candidates sit, and the clearance a car
 * needs to be put down. The near ones are "alongside you", the far ones a
 * junction away. */

/* ------------------------------------------------------------------ */
/* FINDING A ROAD SPOT NEAR A POINT                                    */
/*                                                                     */
/* One search, three users: the warp (beside a player), the respawn     */
/* (nearest road to a wedged car) and the join spawn (a RANDOM road     */
/* point near the level start, so sixteen players do not all appear in  */
/* the same lay-by). It rings outward and stops at the first ring with  */
/* a road point on it, so "near" is the nearest ring that has road at   */
/* all rather than whichever candidate happened to pass a test first.   */
/*                                                                     */
/* The gates are the warp's, unchanged: JerRoadAt says there is road,   */
/* MpWarpSpotOk says the ground is resident and clear, and the CALLER   */
/* streams the winner once (MpWarpStreamTo) -- forcing region after     */
/* region evicts the ones just loaded, and there are only four barrels. */

/* A RESPAWN goes somewhere else, not back onto the road you just crashed on: a road
 * point in a ring around the car, roughly 60-150 m out (12000-30000 world units; a car
 * is about 1000 units long), clear of every other player's car, facing along the road.
 * Already-resident regions are tried first so a respawn rarely has to stream; then
 * regions that need spooling. 0 when nothing qualified -- the callers then fall back
 * to the nearest road, as before. */

/* A map-wide road spot for a late join into a FULL-CITY Take a Ride.
 *
 * The near-start ring cannot serve here: the level's start may be in one corner
 * of the city. The map streams as a 2x2 window of REGIONS, so a point cannot be
 * tested until its region is in a barrel -- and forcing region after region
 * evicts the one just loaded (there are four barrels). So this chooses ONE
 * data-bearing region, keeps that choice across ticks, streams it once, and only
 * then offers several large-offset centers inside it to the same road search the
 * near-start path uses. `carId` is our own car, still standing on the level
 * start; it must not veto its own destination.
 *
 * Returns 1 and fills the outputs. 0 means this region has no usable road for
 * us: the caller takes the safe near-start road instead of streaming again. */

/* How old an on-foot pose may be and still be a warp target: poses arrive every
 * few frames while someone is on foot, so a few seconds of silence means the copy
 * is stale (they got back in, or left). */

/* ------------------------------------------------------------------ */
/* RESPAWN ON THE NEAREST ROAD                                         */
/*                                                                     */
/* The pause menu's Respawn, and the answer to both "I am wedged in    */
/* the scenery" and "I warped and landed in the void": put the car back */
/* on the nearest bit of ROAD, repaired and upright.                   */
/*                                                                     */
/* Road rather than "wherever is clear", deliberately: clear ground can */
/* be the flat roof of a building or a hillside the car cannot leave,   */
/* while a road is somewhere the level is BUILT to be driven on. The    */
/* search rings outward and stops at the first ring with a road point   */
/* on it, so what comes back is the nearest one rather than whichever   */
/* happened to pass a test first.                                      */
/*                                                                     */
/* And it REFUSES when there is nothing to land on, leaving the car put */
/* -- a respawn that drops the player into the void is worse than no    */
/* respawn, which is exactly how the warp earned these helpers.         */
/* The ring geometry lives with the shared search now:                  */
/* MP_ROAD_RING_STEP / MP_ROAD_RINGS / MP_ROAD_PROBES.                  */

/* ------------------------------------------------------------------ */
/* THE ROUND-START CLUSTER (Fox N Hounds / Car Bomb Tag)               */
/*                                                                     */
/* When the referee goes COUNTDOWN every machine puts ITS OWN car near  */
/* one ANCHOR car: cars are owner-authoritative (each owner's carstate  */
/* is adopted verbatim), so a host placing somebody else's car would be */
/* undone by that owner's next sample -- and a dedicated server has no  */
/* world at all. The anchor is the lowest present id (the JPMK rows,    */
/* so every machine agrees) whose car this machine can see.             */
/*                                                                     */
/*   - the moment COUNTDOWN begins (WAITING already satisfied), every   */
/*     machine runs a real respawn on its own car: wreck latch cleared  */
/*     (jer_carexplode_reset, every slot -- hold is local), damage and  */
/*     dents repaired, handbrake / thrust / wheelspin dropped, death    */
/*     state and wanted level cleared, JER_EVENT_RESET_CAR fired. This  */
/*     is not done during waiting and is not deferred mid-count; even   */
/*     a later placement miss leaves the car unlocked and repaired.     */
/*     The latch used to live until session teardown (PresentClear),    */
/*     so a bomb holder wrecked at 0:00 stayed locked through the next  */
/*     GO -- InitCarPhysics does not clear handbrake, and hold would    */
/*     re-apply the lock the instant they were marked again.            */
/*   - the same tick, the anchor snaps to the nearest road              */
/*     (MpFindRoadSpotNear, the pause-menu Respawn's search); everyone  */
/*     else searches around a slot on a ring about the anchor's         */
/*     current pose (one slot per player, MP_CLUSTER_PER_RING to a      */
/*     ring, so two searches do not start from the same point), keeps   */
/*     a road spot within MP_CLUSTER_RADIUS of the anchor and           */
/*     MP_JOIN_CLEAR of every other car, streams it (MpWarpStreamTo)    */
/*     and lands on it (MpPlaceOnGround);                               */
/*   - every car faces along its road's lane (random only where the     */
/*     road gives no heading), stopped and                              */
/*     repaired, wanted level cleared, as the Respawn row does.         */
/*                                                                     */
/* The distances are the join spawn's, not a new unit system: the slot  */
/* ring steps by the join's own retry step (MP_JOIN_CLEAR + 1200) and   */
/* the clearance is MP_JOIN_CLEAR. Nothing ticks meanwhile -- the       */
/* referee keeps every clock frozen until GO -- and the pad is held     */
/* (MpRoundHoldsCar), so nobody drives off the line early.         */
/* If the ground is still streaming, placement retries for up to 2 s;   */
/* then the pause menu's warp to the anchor is the fallback; failing    */
/* that, the already-reset car stays where it is and the log says so    */
/* (the round still starts).                                            */

/* ------------------------------------------------------------------ */
/* COPS N ROBBERS: THE CHASE START (MP_MODE_SPAWN_CHASE)                */
/*                                                                      */
/* Every round happens somewhere else. At COUNTDOWN, each machine puts  */
/* its OWN car:                                                         */
/*  - the getaway: a random road anywhere on the map -- the map-wide   */
/*    join spawn (MpFindRoadSpotWide: one random data-bearing region,   */
/*    streamed once, up to 8 random centres in it), facing along its   */
/*    lane (MpRoadHeadingAt);                                           */
/*  - a cop: on the same road BEHIND the getaway, facing the same way. */
/*    It waits for the getaway's new pose to arrive (moved more than    */
/*    MP_CHASE_MOVED from where it was, or MP_CHASE_WAIT_FRAMES), then  */
/*    walks back from it along the road in MP_CHASE_STEP steps, taking  */
/*    the road's heading at each step (the way closer to the getaway's),*/
/*    so a bend is followed. Distance behind, by the cop's place among  */
/*    the cops (rank 0, 1, 2 ... by id):                                */
/*        MP_CHASE_FIRST + rank * MP_CHASE_GAP                          */
/*      = 4000 for the first cop, 7000 the second, 10000 the third ...  */
/*    a spot not clear of other players walks on, at most              */
/*    MP_CHASE_EXTRA further -- short of the next band, so two cops     */
/*    never pick the same spot. A dead end (no road within MP_CHASE_SNAP*/
/*    of the next step) stops the walk where it is.                     */
/* Failing all that by MP_CHASE_GIVEUP_FRAMES: the start cluster's      */
/* fallback (the pause menu's warp beside the getaway), then where the  */
/* car is.                                                              */
/* ------------------------------------------------------------------ */

/* The parked player body a remote owner just got into: stopped (CIV_AI, quiet
 * pad), driven by nobody, not a traffic mirror and not our own car, of the model
 * the owner reports, nearest to the owner's pose. -1 when there is none. */

/* (the old client-only MpSendCarState is folded into MpSendOwnCarState above:
 * in the owner-authoritative model every machine sends exactly one car) */

/* The transport hands every complete message here. */
/* 'JPPN' -- liveness. Answer on the SAME connection it arrived on, so the
 * sender's recv timer is refreshed and it can trust the silence timeout. */
static void MpHandlePing(int connIndex, const unsigned char* p, int len)
{
	MP_PING pg;

	if (len < (int)sizeof(MP_PING))
		return;

	memcpy(&pg, p, sizeof(pg));

	if (MpIsHost() && connIndex >= 0)
		MpSendConn(connIndex, MP_TAG_PONG, 0, &pg, sizeof(pg));
	else
		MpSendToHost(MP_TAG_PONG, 0, &pg, sizeof(pg));
}

/* ------------------------------------------------------------------ */
/* Traffic mirrors: the peer's cars, drawn here and driven by THEM
 *
 * A row names the car_data slot its owner owns. Our bands are disjoint, so that
 * slot is free on this machine and the mirror is built straight into it -- the
 * slot IS the shared identity here, the one respect in which traffic differs from
 * a player car (which is matched by model).
 *
 * The mirror is CONTROL_TYPE_PLAYER: the local civ AI must never drive it (it is
 * the peer's car), CheckPingOut only ever recycles CIV_AI cars so it cannot take
 * it, and our own spawner cannot reach it because the slot lies in the peer's
 * band, which MpTrafficBandRefresh keeps reserved. The pose is written from the
 * wire every frame, exactly as a remote player car's is.
 *
 * CONTROL_TYPE_PLAYER is set AFTER building the car through InitCar's CIV_AI
 * path, deliberately: InitCar's PLAYER path writes player[cp->id] (civ_ai.c:147),
 * and a traffic slot (>= MP_TRAFFIC_SLOT_FIRST) can run past MAX_PLAYERS -- so
 * building it AS a player car would be an out-of-bounds write for the top slots.
 * The CIV_AI path does not touch player[], and once the control type is flipped
 * the civ state it set up is inert. */

/* A mirror the owner has stopped speaking about is gone: the owning machine sends
 * its whole band every frame, so silence means the car left the world (or the peer
 * dropped). The explicit REMOVE row is the fast path; this is the backstop that
 * keeps a lost packet from leaving a ghost car standing forever. */

void MpHandleMessage(int connIndex, const char* tag, const unsigned char* payload, int len)
{
	memcpy(gMpLastRxTag, tag, 4);
	gMpLastRxTag[4] = '\0';
	gMpLastRxConn = connIndex;
	gMpLastRxFrame = gMp.frame;

	if (MpDebugOn() && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] recv %.4s len=%d conn=%d\n", tag, len, connIndex);

	/* A HELLO or a WELCOME means the handshake is under way, so this
	 * connection has stopped being a stranger holding a slot. */
	if (memcmp(tag, MP_TAG_HELLO, 4) == 0 || memcmp(tag, MP_TAG_WELCOME, 4) == 0)
		MpConnHandshakeDone(connIndex);

	if (memcmp(tag, MP_TAG_HELLO, 4) == 0)
	{
		MpHandleHello(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_WELCOME, 4) == 0)
	{
		MpHandleWelcome(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_INPUT, 4) == 0)
	{
		MpHandleInput(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_ROSTER, 4) == 0)
	{
		MpHandleRoster(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_CAR, 4) == 0)
	{
		MpHandleCar(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_REJECT, 4) == 0)
	{
		MpHandleReject(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_CHANNEL, 4) == 0)
	{
		MpHandleChannel(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_START, 4) == 0)
	{
		MpHandleStart(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_CARSTATE, 4) == 0)
	{
		MpHandleCarState(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_TRAFFIC, 4) == 0)
	{
		MpHandleTraffic(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_HIT, 4) == 0)
	{
		MpHandleHit(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_PED, 4) == 0)
	{
		MpHandlePedState(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_STATS, 4) == 0)
	{
		MpHandleStats(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_ROUND, 4) == 0)
	{
		MpHandleRound(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_MARK, 4) == 0)
	{
		MpHandleMark(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_COLOR, 4) == 0)
	{
		MpHandleColor(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_CHAT, 4) == 0)
	{
		MpHandleChat(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_LEAVE, 4) == 0)
	{
		MpHandleLeave(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_KICK, 4) == 0)
	{
		MpHandleKick(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_PING, 4) == 0)
	{
		MpHandlePing(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_PONG, 4) == 0)
	{
		/* the peer echoed the tick we sent it: that round trip is our ping to it */
		if (len >= (int)sizeof(MP_PING))
		{
			MP_PING pg;

			memcpy(&pg, payload, sizeof(pg));
			MpConnSetPing(connIndex, MpNowMs() - (unsigned long)pg.tick);
		}

		return;
	}

	/* No protocol surface sends a bare 'JPSS' (the launch config rides in 'JPST',
	 * and the tag itself has been retired -- see mp_proto.h); unknown tags are
	 * ignored, which is also what makes a retired one harmless. */
}
