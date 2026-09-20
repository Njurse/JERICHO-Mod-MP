/*
 * mp_session.c -- JERICHO Multiplayer session: bring-up, the join
 * handshake (with the mod-match lobby policy) and message dispatch.
 *
 * The per-frame input lockstep and the host state-resync fallback land in
 * later steps; this file currently owns the handshake and the send/recv
 * dispatch that the transport (mp_net.c) calls into.
 */
#include "jericho.h"
#include "jer_events.h"
#include "mp.h"

#include "driver2.h"
#include "mission.h"
#include "glaunch.h"
#include "replays.h"
#include "pad.h"
#include "cars.h"
#include "convert.h"	/* _RotMatrixY: a car's box is built from its matrix */
extern int gBootMpLevel;	/* main.c: 1 = the small multiplayer map, 0 = the full city */
extern int gBootMpArena;	/* main.c: which multiplayer map (0/1) */
extern int gWantNight;		/* glaunch.c: 1 = the night take-a-ride level variant */
#include "players.h"	/* InitPlayer: a late joiner needs a car the same way the engine makes one */
#include "handling.h"	/* LongQuaternion2Matrix: rebuild a car's matrix from its body */
#include "state.h"
#include "dr2roads.h"	/* FindSurfaceD2: the real ground height at a point */

#include <string.h>
#include <stdio.h>
#include <math.h>	/* sqrt: the deviation readout below */

/* defined below, needed by the launch path above them */
static int MpAssignedCarModel(int playerId);

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
static int gMpLaunched;
static int gMpLocalAppliedPad = -1;	/* the pad that drove OUR car, -1 = none seen yet */

/* mp.c calls this from the NET_INPUT hook for our own car, so we replicate the
 * pad the engine is actually driving us with. */
void MpNoteLocalPad(int pad)
{
	gMpLocalAppliedPad = pad;
}

void MpSessionReset(void)
{
	gMpLaunched = 0;
	gMpLocalAppliedPad = -1;
	gMp.gamemode = MP_GAMEMODE_TAKEADRIDE;
	gMp.city = 0;
	gMp.timeOfDay = -1;
	gMp.weather = -1;
	gMp.seed = 0;
	gMp.frame = 0;
	gMp.running = 0;
	gMp.leaving = 0;
	gMp.modsMatched = 1;
	gMp.localPlaced = 0;
	gMp.lastRejectReason = MP_REJECT_NONE;
	gMp.lastRejectText[0] = '\0';
}

int MpBeginHost(void)
{
	/* Already the host: do NOT reset.
	 *
	 * Re-entering the host menu, or the frontend reaching this a second time,
	 * used to tear the whole session down -- listener closed, every peer
	 * dropped -- so a player who was mid-join was sent MP_TAG_LEAVE and told
	 * "the host ended the match", then kicked back to the frontend moments
	 * after being accepted. Hosting twice is not an error, it just has nothing
	 * left to do. */
	if (gMp.role == MP_ROLE_HOST)
		return 1;

	MpSessionReset();
	MpResetPlayers();

	gMp.role = MP_ROLE_HOST;
	gMp.localPlayerId = 0;
	{
		MP_PLAYER* me = MpAddPlayer(0, gMp.config.playerName, 1);
		{
			extern u_char defaultPlayerPalette;

			/* our own car/palette, so the roster advertises them to joiners */
			me->car = gMp.config.car;
			me->carIsSlot = gMp.config.carIsSlot;
			me->palette = defaultPlayerPalette;
		}
		if (me != NULL) me->carId = 0;
	}

	if (!MpHostBegin())
	{
		gMp.role = MP_ROLE_NONE;
		return 0;
	}

	/* NO advertising here. A server is only listed once a match is actually
	 * running (MpStartMatch does it) -- otherwise a browser sees a host that is
	 * not ready to be joined, and players are invited to connect to a lobby
	 * whose host is still picking a city. Joining still works the moment the
	 * beacon carries a live game.
	 */

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] hosting as '%s' (mod enforcement=%d)\n",
			gMp.config.playerName, gMp.config.modCheck);

	return 1;
}

/* The frontend's join: start the connect and return at once, so the menu keeps
 * drawing "Connecting to <ip>:<port>..." until the handshake resolves; the
 * poll in MpNetPoll finishes it. This is the ONLY join path (the UI, -join and
 * MP_AUTOSTART all use it), so a slow or dead address never stalls a frame. */
int MpBeginJoinAsync(const char* host, int port)
{
	if (host == NULL || host[0] == '\0')
		host = "127.0.0.1";

	/* Say what is wrong with the address instead of failing to connect to a
	 * loopback nobody asked for. */
	if (!MpIsValidAddress(host))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] '%s' is not a dotted-quad IP address\n", host);

		jer_error("%s is not a valid IP address", host);
		return 0;
	}

	MpSessionReset();
	MpResetPlayers();

	gMp.role = MP_ROLE_CLIENT;
	gMp.localPlayerId = -1;

	if (!MpClientConnectBegin(host, port))
	{
		gMp.role = MP_ROLE_NONE;
		MpJoinStateSet(MP_JOIN_FAILED);

		jer_error("Could not join the server at %s:%d", host, port);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] join FAILED: could not reach %s:%d\n", host, port);

		return 0;
	}

	MpDiscoveryStart(0);

	return 1;
}

void MpLeaveSession(void)
{
	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] MpLeaveSession (role=%d running=%d)\n", (int)gMp.role, gMp.running);

	gMp.leaving = 1;

	/* if we are the host, tell everyone the match is ending so they go back
	 * to their frontend with a notice rather than a bare connection loss */
	if (gMp.role == MP_ROLE_HOST)
		MpHostByeAll();

	MpClientDisconnect();
	MpHostEnd();
	MpDiscoveryStop();
	MpResetPlayers();

	gMp.role = MP_ROLE_NONE;
	gMp.connected = 0;
	gMp.running = 0;
}

/* Launch the agreed level locally: set the pending globals from the session
 * and enter the game. NumPlayers stays 1 so each machine gets the FULL screen
 * (no split-screen); the other players are puppet cars managed by the module
 * and placed from the network every frame. */
static void MpLaunchLocal(void)
{
	/* Idempotent: see gMpLaunched. A second SetState(STATE_GAMESTART) would load
	 * the level again and restart the match on the joining machine. */
	if (gMpLaunched)
		return;

	gMpLaunched = 1;

	GameLevel = gMp.city;
	GameType = GAME_TAKEADRIVE;
	NumPlayers = 1;
	/* The arena is part of the session now, not a local boot flag: gSubGameNumber
	 * (offset by 440 in glaunch to reach the M58/M498 multiplayer levels) is taken
	 * from the shape the host published, so the host and a client that booted with
	 * a different -mp still load the SAME map. It used to be hardcoded 0, which
	 * silently ignored the arena on every machine. */
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
		gMpCtx->jer_log(gMpCtx, "[mp] launching: city %d mode %d (1=TAKEADRIVE, 0=MISSION!) subgame %d time %d weather %d players %d\n",
			GameLevel, GameType, gSubGameNumber, wantedTimeOfDay, wantedWeather, NumPlayers);

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
	if (gMp.config.car >= 0)
	{
		int car = gMp.config.car;

		if (gMp.config.carIsSlot)
		{
			extern char carNumLookup[4][10];
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
		 * model) and could make both cars identical. */
		int me = (gMp.localPlayerId >= 0) ? gMp.localPlayerId : 0;

		wantedCar[0] = MpAssignedCarModel(me);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] no car chosen -> assigned model %d (player %d, city %d)\n",
				wantedCar[0], me, GameLevel);
	}

	/* Log the host's own car sources. WITH -mpcar wantedCar[0] is the pick; without
	 * one it is the assigned model above -- and if NEITHER ran, the engine's level
	 * default decides, which is the case where the machines used to disagree. */
	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] car sources: config.car=%d isSlot=%d wantedCar[0]=%d startinfo[0]=%d\n",
			gMp.config.car, gMp.config.carIsSlot, wantedCar[0],
			(PlayerStartInfo[0] != NULL) ? PlayerStartInfo[0]->model : -9);

	SetState(STATE_GAMESTART);
}

/* Host: broadcast the agreed config, then launch locally for everyone. */
/* ------------------------------------------------------------------ */
/* The roster                                                          */
/* ------------------------------------------------------------------ */

/* Publish who is in the match: one row per player, ASCENDING PLAYER ID (the host
 * is id 0 and so comes first). Broadcast before a launch -- every machine needs
 * to know how many cars to spawn before its level loads -- and refreshed every
 * couple of seconds so the ping column is live. */
/* A player's chosen vehicle as a concrete MODEL: "slotN" is resolved against the
 * SESSION's city (GameLevel) -- the city the match actually runs in -- while a raw
 * model number passes straight through. -1 when unset. This is the ONE place the
 * slot/model ambiguity is resolved, so a slot picked on one machine becomes the
 * same model on the other. */
static int MpPlayerCarModel(int car, int isSlot)
{
	extern char carNumLookup[4][10];

	if (car < 0)
		return -1;

	if (isSlot)
	{
		int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
		int slotNo = (car >= 1 && car <= 10) ? car : 1;

		return carNumLookup[lvl][slotNo - 1];
	}

	return car;
}

/* The car EVERY machine assigns a player who did not choose one (no -mpcar). It
 * must be the SAME number on both machines AND different per player, so the two
 * cars are both distinguishable and agreed on. The level's own car table
 * (carNumLookup, indexed by player id) is exactly that: bounded by the level's
 * pool, and both machines compute it from the same city.
 *
 * The old code applied this to REMOTE players only and left the LOCAL player on
 * the level's default, so the machines disagreed about the local player's car
 * (the client saw the host as slot 0 with a palette that did not match, because
 * the owner's palette was applied to a different model), and two un-chosen
 * players could both come out as the same model if the level's default coincided
 * with one of the assigned ones. */
static int MpAssignedCarModel(int playerId)
{
	extern char carNumLookup[4][10];
	int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
	int k = (playerId >= 0) ? (playerId % 4) : 0;

	return carNumLookup[lvl][k];
}

void MpHostSendRoster(void)
{
	MP_ROSTER r;
	int id, len;

	if (!MpIsHost())
		return;

	memset(&r, 0, sizeof(r));

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		MP_ROSTER_ENTRY* e;

		if (p == NULL)
			continue;

		e = &r.entries[r.count++];
		e->id = (uint8_t)id;
		e->flags = (uint8_t)(id == 0 ? MP_ROSTER_FLAG_HOST : 0);
		e->carId = 0xff;
		e->model = 0xff;
		e->reserved = (uint16_t)p->palette;

		/* the car this player ASKED for, resolved here on the host (the session
		 * city) -- so a joiner knows everyone's car before anything is spawned.
		 * The spawn overrides this with the model actually loaded. */
		{
			int m = MpPlayerCarModel(p->car, p->carIsSlot);

			if (m >= 0 && m <= 0xff)
				e->model = (uint8_t)m;
		}

		e->ping = (uint16_t)(id == 0 ? 0 : MpPingForPlayer(id));
		snprintf(e->name, sizeof(e->name), "%s", p->name);

		if (p->carId >= 0 && p->carId < MAX_CARS)
		{
			CAR_DATA* cp = &car_data[p->carId];

			e->carId = (uint8_t)p->carId;
			e->x = cp->hd.where.t[0];
			e->y = cp->hd.where.t[1];
			e->z = cp->hd.where.t[2];

			/* on foot: nobody is driving that car any more */
			e->model = (cp->controlType == CONTROL_TYPE_NONE) ? 0xff : (uint8_t)cp->ap.model;
		}
	}

	len = (int)(sizeof(MP_ROSTER) - (size_t)(MP_MAX_PLAYERS - r.count) * sizeof(MP_ROSTER_ENTRY));
	MpHostBroadcast(MP_TAG_ROSTER, 0, &r, len);
}

/* Give a car to any player who has none, exactly the way the engine's own
 * player-creation loop does it. A peer that joins a match already in progress
 * gets no car from the engine at all -- and a player with no car is invisible
 * on every screen, has no input applied to anything, and cannot be hit. That is
 * the "no client shows up on the host" report.
 *
 * The player cars occupy slots [0, n) and are assigned in ascending player id,
 * so the first slot that is not already claimed is simply how many rows have
 * one. player[] is MAX_PLAYERS (16) entries, so this is safe past slot 1. */
void MpSpawnLateJoiners(void)
{
	int id, slot, spawned = 0;

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		int rot, k;
		char padid;

		if (p == NULL || p->isLocal || p->carId >= 0)
			continue;

		slot = 0;
		for (k = 0; k < MP_MAX_PLAYERS; k++)
		{
			MP_PLAYER* q = MpGetPlayer(k);

			if (q != NULL && q->carId >= 0)
				slot++;
		}

		if (slot < 1 || slot >= MAX_CARS)
			continue;

		/* copy the local player's start record, then overwrite what is ours to
			 * choose (see MpOnNetSpawn for the same construction at level init) */
		PlayerStartInfo[slot] = &ReplayStreams[slot].SourceType;
		memcpy((u_char*)PlayerStartInfo[slot], (u_char*)PlayerStartInfo[0], sizeof(STREAM_SOURCE));

		PlayerStartInfo[slot]->type = 1;
		PlayerStartInfo[slot]->controlType = CONTROL_TYPE_PLAYER;
		PlayerStartInfo[slot]->flags = 0;

		/* Same pool-checked per-player vehicle as the level-init spawn
		 * (MpOnNetSpawn): the host's own model is NOT this player's, and copying it
		 * made the joiner's car identical to the host's on the host's screen. */
		{
			int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
			int cid = MpPlayerCarModel(p->car, p->carIsSlot);

			/* an explicit pick, else the model every machine assigns this player */
			if (cid < 0)
				cid = MpAssignedCarModel(id);

			PlayerStartInfo[slot]->model = (u_char)cid;
			PlayerStartInfo[slot]->palette = (u_char)(p->palette >= 0 ? p->palette : 0);

			if (slot >= 0 && slot < 2)
				wantedCar[slot] = cid;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] late joiner: player %d -> slot %d model %d (city %d)\n",
					id, slot, PlayerStartInfo[slot]->model, lvl);
		}

		/* NOTHING ELSE TOUCHES THE POSITION. The engine's own spawn for this slot is
		 * already the SAME on both machines (verified: the second car lands at the
		 * same x/z/y whether we look from the host or the client), and it is already
		 * on the ground. The old code moved the slot alongside the host's live car
		 * and forced the local start record's vy -- which reads 0 -- so the car
		 * started in the air and fell in. */

		rot = car_data[0].hd.direction;
		PlayerStartInfo[slot]->rotation = rot;

		padid = (char)-slot;

		InitPlayer(&player[slot], &car_data[slot], CONTROL_TYPE_PLAYER, rot,
			(LONGVECTOR4*)&PlayerStartInfo[slot]->position,
			PlayerStartInfo[slot]->model, PlayerStartInfo[slot]->palette, &padid);

		/* the same field the engine sets for a player car it created itself */
		car_data[slot].ap.needsDenting = 1;

		/* InitPlayer resolves the start record, so place the car after it and
			 * rebuild the matrix (the collision box is built from the matrix) */
		car_data[slot].hd.where.t[0] = PlayerStartInfo[slot]->position.vx;
		car_data[slot].hd.where.t[1] = car_data[0].hd.where.t[1];
		car_data[slot].hd.where.t[2] = PlayerStartInfo[slot]->position.vz;
		car_data[slot].hd.direction = rot;
		{
			MATRIX m;

			_RotMatrixY(&m, (short)rot);
			memcpy(car_data[slot].hd.where.m, m.m, sizeof(car_data[slot].hd.where.m));
		}

		p->carId = slot;
		spawned++;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] late joiner: player %d given car slot %d\n", id, slot);
	}

	if (spawned > 0)
	{
		/* tell everyone, so the clients build the car too instead of waiting for
		 * a snapshot to paste a transform onto whatever is in that slot */
		MpHostSendRoster();
	}
}

static void MpHandleRoster(const unsigned char* p, int len)
{
	const int fixed = (int)(sizeof(MP_ROSTER) - MP_MAX_PLAYERS * sizeof(MP_ROSTER_ENTRY));
	MP_ROSTER r;
	int n, i;

	if (len < fixed || ((len - fixed) % (int)sizeof(MP_ROSTER_ENTRY)) != 0)
		return;

	n = (len - fixed) / (int)sizeof(MP_ROSTER_ENTRY);
	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	memset(&r, 0, sizeof(r));
	memcpy(&r, p, (size_t)fixed + (size_t)n * sizeof(MP_ROSTER_ENTRY));

	/* Adopt the players we did not know about. Their CAR SLOT is deliberately
	 * NOT taken from here: carId is a local slot and the host's numbering means
	 * nothing on this machine. We assign our own in the spawn, walking player
	 * ids in this same order so both sides agree on who is who. */
	for (i = 0; i < n; i++)
	{
		MP_ROSTER_ENTRY* e = &r.entries[i];
		MP_PLAYER* pl = MpGetPlayer(e->id);

		if (pl == NULL)
			pl = MpAddPlayer(e->id, e->name, e->id == gMp.localPlayerId);

		if (pl == NULL)
			continue;

		if (e->name[0] != 0)
			snprintf(pl->name, sizeof(pl->name), "%s", e->name);

		pl->isHost = (e->flags & MP_ROSTER_FLAG_HOST) ? 1 : 0;

		/* isLocal is only knowable once we have been welcomed: the roster is sent
		 * BEFORE the welcome (deliberately -- a live joiner must know who else is
		 * in the match before it spawns cars), so at that moment localPlayerId is
		 * still -1 and every row would be marked as someone else. Mark it only
		 * when it is known, and re-mark on every roster so it stays right. */
		if (gMp.localPlayerId >= 0)
			pl->isLocal = (e->id == gMp.localPlayerId) ? 1 : 0;
		pl->pingMs = (int)e->ping;

		/* Adopt this player's CAR too. Without this a third client kept the
		 * level-table fallback for everyone and showed the wrong vehicles. The host
		 * resolved the model (whole roster, session city), so it is a model here. */
		if (!pl->isLocal && e->model != 0xff)
		{
			pl->car = (int)e->model;
			pl->carIsSlot = 0;
		}
		pl->palette = (int)e->reserved;
	}
}

int MpStartMatch(void)
{
	MP_START st;

	if (!MpIsHost() || gMp.running)
		return 0;


	/* An unattended session (-host / MP_AUTOSTART) takes its level from the
	 * engine's own -level/-mp. Adopted HERE and not when the arguments were
	 * parsed, because -level only sets gBootLevel at that point and GameLevel
	 * is assigned afterwards. A frontend lobby does not need this: the player
	 * picked a city in the menus. */
	if (gMp.autoSession)
		gMp.city = GameLevel;
	/* the lobby values may still be unset (-1): clamp to valid ones BEFORE
	 * both the broadcast and the local launch so host and clients agree, and
	 * the mission loader never sees an out-of-range index */
	if (gMp.city < 0)
		gMp.city = 0;
	if (gMp.timeOfDay < 0)
		gMp.timeOfDay = TIME_DAY;	/* a session with no time is a DAY match (0 = DAWN) */
	if (gMp.weather < 0)
		gMp.weather = 0;

	gMp.running = 1;

	/* now that the match is actually live, start advertising it: the LAN
	 * browser lists live games (and they accept spawn-ins). */
	MpDiscoveryStart(1);
	gMp.seed = 0x5eed1318u;		/* shared run seed */

	memset(&st, 0, sizeof(st));
	st.session.gamemode = (uint8_t)gMp.gamemode;
	st.session.city = (uint8_t)gMp.city;
	st.session.timeOfDay = (uint8_t)(gMp.timeOfDay < 0 ? 0 : gMp.timeOfDay);
	st.session.weather = (uint8_t)(gMp.weather < 0 ? 0 : gMp.weather);
	st.session.seed = gMp.seed;
	st.session.numPlayers = (uint8_t)gMp.playerCount;
	st.session.state = MP_SESSION_STARTING;

	/* The roster goes first, so a client knows how many player cars to spawn
	 * BEFORE its level loads. Without it the client added no car for the host
	 * and left the level's own AI car sitting in that slot. */
	MpHostSendRoster();

	MpHostBroadcast(MP_TAG_START, MP_FLAG_RELIABLE, &st, sizeof(st));

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] start match: mode %d city %d time %d weather %d -> %d client(s)\n",
			gMp.gamemode, gMp.city, gMp.timeOfDay, gMp.weather, MpPeerCount());

	MpLaunchLocal();

	return 1;
}

/* ------------------------------------------------------------------ */
/* Handshake                                                           */
/* ------------------------------------------------------------------ */
void MpSendHello(void)
{
	unsigned char buf[sizeof(MP_HELLO) + MP_MAX_MODS * sizeof(MP_MOD_INFO)];
	MP_HELLO h;
	int n;

	n = MpBuildManifest((MP_MOD_INFO*)(buf + sizeof(MP_HELLO)), MP_MAX_MODS);

	memset(&h, 0, sizeof(h));
	h.protoVersion = (uint16_t)MP_PROTO_VERSION;
	h.sdkVersion = (uint16_t)JERICHO_SDK_VERSION;
	h.gameBuild = MpBuildHash();
	/* Tell the host which vehicle we want, so it spawns OUR car for us rather than
	 * guessing: without this each machine substituted its own idea of a player's
	 * car and the two disagreed (the joiner's car looked like the host's). A slot
	 * (-mpcar slotN) cannot be resolved yet -- GameLevel is the host's, unknown
	 * here -- so only a plain car id travels; the rest falls back. */
	/* Send the RAW selection plus a SLOT FLAG: "slotN" is a per-city frontend slot,
	 * so only the HOST can resolve it (against the session city -- we are still in
	 * our boot city here). A raw model number passes straight through. reserved[1]
	 * carries our palette so the host paints our car the colour we see. */
	{
		extern u_char defaultPlayerPalette;

		MP_PLAYER* me = MpLocalPlayer();

		h.car = (uint16_t)((gMp.config.car >= 0) ? gMp.config.car : 0xFFFF);
		h.reserved[0] = (uint8_t)(gMp.config.carIsSlot ? 1 : 0);
		h.reserved[1] = (uint8_t)((me != NULL && me->carId >= 0)
			? (uint8_t)car_data[me->carId].ap.palette
			: (uint8_t)defaultPlayerPalette);
	}
	snprintf(h.playerName, sizeof(h.playerName), "%s", gMp.config.playerName);
	h.modCount = (uint8_t)n;

	memcpy(buf, &h, sizeof(h));

	MpSendToHost(MP_TAG_HELLO, MP_FLAG_RELIABLE, buf,
		(int)(sizeof(h) + (size_t)n * sizeof(MP_MOD_INFO)));

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] hello sent (%d mods)\n", n);
}

/* Does the client's manifest satisfy the host's policy? */
static int MpModsMatchHost(const MP_MOD_INFO* cm, int cn, int strict)
{
	MP_MOD_INFO hm[MP_MAX_MODS];
	int hn = MpBuildManifest(hm, MP_MAX_MODS);
	int i, j, found;

	for (i = 0; i < cn; i++)
	{
		found = 0;

		for (j = 0; j < hn; j++)
		{
			if (strcmp(cm[i].id, hm[j].id) == 0)
			{
				if (strict == MP_MODCHECK_EXACT && strcmp(cm[i].version, hm[j].version) != 0)
					return 0;
				found = 1;
				break;
			}
		}

		if (!found)
			return 0;
	}

	/* every host-enabled mod must also be present on the client */
	for (j = 0; j < hn; j++)
	{
		found = 0;

		for (i = 0; i < cn; i++)
		{
			if (strcmp(cm[i].id, hm[j].id) == 0)
			{
				found = 1;
				break;
			}
		}

		if (!found)
			return 0;
	}

	return 1;
}

static void MpSendReject(int connIndex, int reason, const char* text)
{
	MP_REJECT r;

	memset(&r, 0, sizeof(r));
	r.reason = (uint8_t)reason;
	snprintf(r.text, sizeof(r.text), "%s", text != NULL ? text : "");

	MpSendConn(connIndex, MP_TAG_REJECT, 0, &r, sizeof(r));
}

static int MpAllocPlayerId(void)
{
	int id;

	for (id = 1; id < MP_MAX_PLAYERS; id++)
	{
		if (MpGetPlayer(id) == NULL)
			return id;
	}

	return -1;
}

static void MpSendWelcome(int connIndex, int playerId, int matched)
{
	MP_WELCOME w;

	memset(&w, 0, sizeof(w));
	w.playerId = (uint8_t)playerId;
	w.maxPlayers = MP_MAX_PLAYERS;
	w.modsEnforced = (uint8_t)gMp.config.modCheck;
	w.modsMatched = (uint8_t)(matched ? 1 : 0);
	w.running = (uint8_t)(gMp.running ? 1 : 0);
	w.subGame = (uint8_t)(gSubGameNumber & 0xff);
	w.mpLevel = (uint8_t)(gBootMpLevel ? 1 : 0);	/* the map SHAPE, not the arena */
	w.arena = (uint8_t)(gBootMpArena & 0x01);	/* which shape: the arena belongs in the session, not a local -mp */
	w.gamemode = (uint8_t)gMp.gamemode;
	w.city = (uint8_t)gMp.city;
	w.timeOfDay = (uint8_t)(gMp.timeOfDay < 0 ? 0 : gMp.timeOfDay);
	w.weather = (uint8_t)(gMp.weather < 0 ? 0 : gMp.weather);
	w.seed = gMp.seed;
	/* OUR vehicle, so the joiner spawns the car we actually drive instead of
	 * guessing a different one from the level table -- that guess is how the two
	 * machines ended up disagreeing about the host's car. 0xFF = "I have not
	 * chosen one", and the joiner falls back to the deterministic pick. */
	/* Our OWN car as a resolved MODEL -- GameLevel is our city by now, so a slot
	 * resolves correctly and the client spawns the car we actually drive. */
	{
		int m = MpPlayerCarModel(gMp.config.car, gMp.config.carIsSlot);

		w.hostCar = (uint8_t)((m >= 0 && m <= 0xff) ? m : 0xFF);
	}

	/* The roster must be on the wire BEFORE the welcome. A live joiner launches
	 * the moment it is welcomed, and if it does not yet know who else is in the
	 * match it spawns no car for them -- leaving the level's own AI car (a cop)
	 * sitting in that player's slot on that machine, which is exactly the
	 * one-sided "placeholder cop car". TCP keeps the order for us. */
	MpHostSendRoster();

	MpSendConn(connIndex, MP_TAG_WELCOME, MP_FLAG_RELIABLE, &w, sizeof(w));

	/* The player on the other end is about to launch a level and will say
	 * nothing for the whole load. Grant the grace from here, not only from
	 * our own launches: a host that started its match a minute ago has
	 * already spent its window, and would otherwise time out the very
	 * player it just accepted. */
	MpMarkBusy(MP_BUSY_LAUNCH_MS);
}

static void MpHandleHello(int connIndex, const unsigned char* p, int len)
{
	MP_HELLO h;
	const MP_MOD_INFO* mods;
	int n, id, matched = 1;

	if (len < (int)sizeof(MP_HELLO))
	{
		MpConnClose(connIndex);
		return;
	}

	memcpy(&h, p, sizeof(h));
	n = h.modCount;
	if (n > MP_MAX_MODS)
		n = MP_MAX_MODS;

	if (len < (int)(sizeof(MP_HELLO) + (size_t)n * sizeof(MP_MOD_INFO)))
	{
		MpConnClose(connIndex);
		return;
	}

	mods = (const MP_MOD_INFO*)(p + sizeof(MP_HELLO));

	/* only the host processes a hello */
	if (!MpIsHost())
	{
		MpConnClose(connIndex);
		return;
	}

	/* Protocol and SDK version are always required -- they decide whether the
	 * two builds can speak to each other at all. The BUILD hash is not: it is
	 * FNV1a of "git describe --tags --always --dirty", so it changes on every
	 * commit and even on an unclean tree, and two people playing from dev
	 * builds would never match. It is now behind the lobby's opt-in
	 * "Strict Version" toggle, which is off by default. */
	if (h.protoVersion != (uint16_t)MP_PROTO_VERSION ||
	    h.sdkVersion != (uint16_t)JERICHO_SDK_VERSION ||
	    (gMp.config.strictVersion && h.gameBuild != MpBuildHash()))
	{
		if (gMpCtx)
			gMpCtx->jer_log(gMpCtx, "[mp] reject: version mismatch (proto %d/%d, sdk %d/%d, build %d/%d, strict=%d)\n",
				h.protoVersion, MP_PROTO_VERSION, h.sdkVersion, JERICHO_SDK_VERSION,
				h.gameBuild, MpBuildHash(), gMp.config.strictVersion);

		MpSendReject(connIndex, MP_REJECT_VERSION, "game/protocol version mismatch");
		MpConnShutdownGraceful(connIndex);
		return;
	}

	if (gMp.config.modCheck != MP_MODCHECK_OFF)
	{
		matched = MpModsMatchHost(mods, n, gMp.config.modCheck);

		if (!matched)
		{
			if (gMpCtx)
				gMpCtx->jer_log(gMpCtx, "[mp] reject: mod mismatch (enforcement=%d)\n", gMp.config.modCheck);

			MpSendReject(connIndex, MP_REJECT_MODS, "mod list does not match the host");
			MpConnShutdownGraceful(connIndex);
			return;
		}
	}

	/* A match that is already live accepts joiners (they spawn in). The
	 * discovery beacon only advertises live matches, so this is the normal
	 * case for a game found in the LAN browser. */

	id = MpAllocPlayerId();
	if (id < 0)
	{
		MpSendReject(connIndex, MP_REJECT_FULL, "server is full");
		MpConnShutdownGraceful(connIndex);
		return;
	}

	MpConnAssignPlayer(connIndex, id);

	{
		MP_PLAYER* pl = MpAddPlayer(id, h.playerName, 0);

		if (pl != NULL)
		{
			pl->modsMatched = matched;
			pl->car = (h.car == 0xFFFF) ? -1 : (int)h.car;
			pl->carIsSlot = h.reserved[0] ? 1 : 0;
			pl->palette = (int)h.reserved[1];

			if (gMpCtx)
				gMpCtx->jer_log(gMpCtx, "[mp] hello from player %d: vehicle %d\n", id, pl->car);
		}
	}

	/* A match that is already running has no car for this player: the engine
	 * creates player cars exactly once, at level init (InitGameVariables), and
	 * that has long since happened. Ask for one on the NEXT FRAME -- building it
	 * here would be doing engine work from inside the network poll. */
	if (gMp.running)
		gMp.pendingSpawn = 1;

	MpSendWelcome(connIndex, id, matched);

	MpNotifyf("%s joined", h.playerName);

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] player %d '%s' joined (%d/%d)%s\n",
			id, h.playerName, gMp.playerCount, MP_MAX_PLAYERS,
			matched ? "" : " [mods differ]");
}

/* Chat (scaffolding): the host is the echo point, so a line is announced
 * exactly once on every peer. Clients send to the host, which republishes to
 * everyone (including the sender); the host announces its own line directly. */
void MpSendChat(const char* text)
{
	MP_CHAT c;

	if (!MpIsActive() || text == NULL || text[0] == '\0')
		return;

	memset(&c, 0, sizeof(c));
	c.playerId = (uint8_t)(gMp.localPlayerId < 0 ? 0 : gMp.localPlayerId);
	snprintf(c.text, sizeof(c.text), "%s", text);

	if (MpIsHost())
	{
		MpNotifyf("%s: %s", gMp.config.playerName, c.text);
		MpHostBroadcast(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
	}
	else
	{
		MpSendToHost(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
	}
}

static void MpHandleChat(const unsigned char* p, int len)
{
	MP_CHAT c;
	MP_PLAYER* pl;

	if (len < (int)sizeof(MP_CHAT))
		return;

	memcpy(&c, p, sizeof(c));
	c.text[sizeof(c.text) - 1] = '\0';

	pl = MpGetPlayer(c.playerId);
	MpNotifyf("%s: %s", (pl != NULL) ? pl->name : "?", c.text);

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
}

/* The host ended the match: tell the player and go back to the frontend. */
static void MpHandleLeave(const unsigned char* p, int len)
{
	(void)p;
	(void)len;

	if (gMp.role == MP_ROLE_HOST)
		return;			/* a client asked to leave; its row is dropped anyway */

	jer_error("The host ended the match");
	MpReturnToFrontend();
}

static void MpHandleWelcome(const unsigned char* p, int len)
{
	MP_WELCOME w;

	if (len < (int)sizeof(MP_WELCOME))
		return;

	memcpy(&w, p, sizeof(w));

	gMp.localPlayerId = w.playerId;
	gMp.modsMatched = w.modsMatched ? 1 : 0;
	gMp.gamemode = w.gamemode;
	gMp.city = w.city;
	gMp.timeOfDay = w.timeOfDay;
	gMp.weather = w.weather;
	gMp.seed = w.seed;

	/* Enforce the host's map. Which multiplayer map (arena 0/1), and whether it is
	 * a multiplayer map at all, are LOCAL boot flags -- -mp and -level set them --
	 * so two machines could quietly load different maps and then disagree about
	 * everything standing on them. The host is the authority: its session says
	 * which shape the level is and the client loads that, whatever its own
	 * arguments said. */
	gBootMpLevel = w.mpLevel ? 1 : 0;
	gBootMpArena = (int)w.arena;
	/* NOT MpSetSubGame(w.subGame): glaunch.c multiplies gSubGameNumber by 440 when it derives the mission number, so pushing the host's raw internal value through it lands on a mission number hundreds out of range -- a level that does not exist, and then a car with no data reaching ComputeCarLightingLevels. That is an access violation, and it was mine. */

	MpAddPlayer(0, "Host", 0);

	/* The host told us which car it drives; without it we guessed from the level
	 * table and showed the host in a different car than the host's own screen. */
	if (w.hostCar != 0xFF)
	{
		MP_PLAYER* host = MpGetPlayer(0);

		if (host != NULL)
			host->car = (int)w.hostCar;
	}

	{
		MP_PLAYER* me = MpAddPlayer(w.playerId, gMp.config.playerName, 1);
		{
			extern u_char defaultPlayerPalette;

			me->car = gMp.config.car;
			me->carIsSlot = gMp.config.carIsSlot;
			me->palette = defaultPlayerPalette;
		}

		if (me != NULL)
		{
			me->modsMatched = gMp.modsMatched;
			me->carId = 0;		/* our own car is engine slot 0 */
		}
	}

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] accepted as player %d (matched=%d gamemode=%d city=%d)\n",
			w.playerId, gMp.modsMatched, w.gamemode, w.city);

	MpJoinStateSet(MP_JOIN_READY);	/* the "Connecting..." row goes away */

	/* the match is already live: adopt the host's config and launch so we
	 * spawn into the running game (the LAN browser only lists live games). */
	if (w.running)
	{
		gMp.running = 1;
		gMp.subGame = w.subGame;

		/* adopt the host's level so we load the same map, then let the
		 * joining player pick their own vehicle -- the car select's START
		 * launches us into the running game. */
		GameLevel = gMp.city;
		MpSetSubGame(gMp.subGame);

		if (gMp.timeOfDay >= 0)
			wantedTimeOfDay = gMp.timeOfDay;
		if (gMp.weather >= 0)
			wantedWeather = gMp.weather;

		if (gMpCtx)
			gMpCtx->jer_log(gMpCtx, "[mp] joined a LIVE match - pick a car\n");

		if (gMp.autoSession)
		{
			/* Launched from -join / MP_AUTOSTART: nobody is here to press the
			 * car select's START. Only ASK for the launch from here -- this runs
			 * inside the network poll, and changing game state (SetState) from
			 * inside a hook is re-entrant and took the game down. The launch
			 * itself happens on the next frame. */
			gMp.pendingLaunch = 1;
		}
		else
		{
			MpUiOpenCarSelect();
		}
	}
}

/* Launch the client into the host's level (called from the START claim after
 * the joining player has chosen a car). */
void MpClientLaunch(void)
{
	MpLaunchLocal();
}

static void MpHandleReject(const unsigned char* p, int len)
{
	MP_REJECT r;

	if (len < (int)sizeof(MP_REJECT))
		return;

	memcpy(&r, p, sizeof(r));

	gMp.lastRejectReason = r.reason;
	snprintf(gMp.lastRejectText, sizeof(gMp.lastRejectText), "%s", r.text);

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] join refused: %s (reason %d)\n", r.text, r.reason);

	MpClientDisconnect();
	gMp.role = MP_ROLE_NONE;
	MpJoinStateSet(MP_JOIN_FAILED);	/* the UI shows the refusal, not "Connecting..." */
}

/* The addon net bridge: deliver an inbound MP_CHANNEL to modules (and, on the
 * host, relay a client's payload to the other clients). */
static void MpHandleChannel(int connIndex, const unsigned char* p, int len)
{
	MP_CHANNEL h;
	int peer;

	if (len < (int)sizeof(MP_CHANNEL))
		return;

	memcpy(&h, p, sizeof(h));

	if (len < (int)(sizeof(MP_CHANNEL) + (size_t)h.len))
		return;

	peer = MpIsHost() ? MpConnPlayerId(connIndex) : 0;
	if (peer < 0)
		peer = 0;

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_CHANNEL, h.reliable ? MP_FLAG_RELIABLE : 0, p, len);

	MpBridgeDeliver(h.name, peer, p + sizeof(MP_CHANNEL), (int)h.len);
}

static void MpHandleStart(const unsigned char* p, int len)
{
	/* An auto session launches itself here too: the usual case is that the host
	 * was already waiting in its lobby, so it starts the match AFTER we join and
	 * no welcome-time launch ever happened. Nothing here is re-entrant -- this
	 * runs from the poll, so the launch is deferred like the other one. */
	if (gMp.autoSession && !gMp.running)
		gMp.pendingLaunch = 1;
	/* the host is launching: it will be silent for the load */
	MpMarkBusy(MP_BUSY_LAUNCH_MS);

	MP_START st;

	if (len < (int)sizeof(MP_START))
		return;

	memcpy(&st, p, sizeof(st));

	gMp.gamemode = st.session.gamemode;
	gMp.city = st.session.city;
	gMp.timeOfDay = st.session.timeOfDay;
	gMp.weather = st.session.weather;
	gMp.seed = st.session.seed;
	gMp.running = 1;

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] host started: mode %d city %d time %d weather %d\n",
			gMp.gamemode, gMp.city, gMp.timeOfDay, gMp.weather);

	MpLaunchLocal();
}

/* JER_EVENT_NET_SPAWN: create one car per remote player. Slots past the local
 * players get negative pad ids in the spawn loop; the module keeps them placed
 * from the network each frame. */
int MpOnNetSpawn(void* userdata, void* args)
{
	JER_ARGS_NET_SPAWN* sp = (JER_ARGS_NET_SPAWN*)args;
	int slot, i;

	(void)userdata;

	sp->added = 0;

	if (!MpIsActive() || gMp.playerCount <= 1)
		return JER_RESULT_CONTINUE;

	slot = sp->numPlayers;

	/* ASCENDING PLAYER ID, not registry-row order. Rows are handed out
	 * first-free, so walking them lets two machines put the same two players in
	 * opposite slots -- and then each drives the other's car, or reads a level
	 * AI car as a player. Player ids are the one ordering both sides agree on. */
	for (i = 0; i < MP_MAX_PLAYERS && slot < sp->maxPlayers; i++)
	{
		MP_PLAYER* p = MpGetPlayer(i);

		if (p == NULL || p->isLocal)
			continue;

		PlayerStartInfo[slot] = &ReplayStreams[slot].SourceType;
		memcpy((u_char*)PlayerStartInfo[slot], (u_char*)PlayerStartInfo[0], sizeof(STREAM_SOURCE));

		PlayerStartInfo[slot]->type = 1;
		PlayerStartInfo[slot]->controlType = CONTROL_TYPE_PLAYER;
		PlayerStartInfo[slot]->flags = 0;

		/* Each remote player gets a POOL-CHECKED vehicle: the engine's own
		 * per-level car table (carNumLookup), indexed by player id, so both
		 * machines independently pick the SAME car for the same player. The old
		 * code inherited the LOCAL player's model from the memcpy above, so on the
		 * host the client drove the host's car and on the client the host drove the
		 * client's -- two machines simulating different cars for one player, and
		 * with a chosen special car (e.g. -mpcar 12) an unloaded model on both.
		 * carNumLookup's entries are the level's own cars, so this cannot ask for a
		 * model the level lacks. Only remote cars reach here (the local player is
		 * skipped above); i is that player's id. */
		{
			int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
			/* an explicit pick (-mpcar) if there is one, else the model EVERY machine
			 * assigns this player id -- see MpAssignedCarModel */
			int cid = MpPlayerCarModel(p->car, p->carIsSlot);

			if (cid < 0)
				cid = MpAssignedCarModel(p->id);

			PlayerStartInfo[slot]->model = (u_char)cid;
			PlayerStartInfo[slot]->palette = (u_char)(p->palette >= 0 ? p->palette : 0);

			/* Also put it in wantedCar, which the engine re-applies to
			 * PlayerStartInfo[] as its LAST word before the level runs (main.c).
			 * Something between here and the spawn loop resets the remote slot's model
			 * to the level default, and wantedCar is the one channel that survives it.
			 * wantedCar is 2 entries -- for the local players -- so only slot 1 fits. */
			if (slot >= 0 && slot < 2)
				wantedCar[slot] = cid;

			if (gMpCtx != NULL)
			{
				VECTOR g, nrm, out;
				sdPlane* plane = NULL;

				g.vx = PlayerStartInfo[slot]->position.vx;
				g.vy = PlayerStartInfo[slot]->position.vy;
				g.vz = PlayerStartInfo[slot]->position.vz;
				FindSurfaceD2(&g, &nrm, &out, &plane);

				gMpCtx->jer_log(gMpCtx,
					"[mp] netspawn: player %d -> slot %d model %d (city %d, asked %d)\n",
					i, slot, PlayerStartInfo[slot]->model, lvl, p->car);

				gMpCtx->jer_log(gMpCtx,
					"[mp] spawn y: slot %d vy=%d (local slot 0 vy=%d) surface y=%d\n",
					slot, PlayerStartInfo[slot]->position.vy,
					PlayerStartInfo[0]->position.vy, out.vy);
			}
		}

		/* The HEIGHT is the local start record's -- forcing it to 0 dropped the
		 * remote car in from above, because the level's datum is not y=0. */
		PlayerStartInfo[slot]->position.vy = PlayerStartInfo[0]->position.vy;
		PlayerStartInfo[slot]->position.vx = PlayerStartInfo[0]->position.vx + 900 * slot;
		PlayerStartInfo[slot]->position.vz = PlayerStartInfo[0]->position.vz;
		PlayerStartInfo[slot]->rotation = PlayerStartInfo[0]->rotation;

		p->carId = slot;	/* the CAR_DATA slot this player drives */
		slot++;
	}

	sp->added = slot - sp->numPlayers;

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] added %d remote player car(s)\n", sp->added);

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Input lockstep                                                      */
/* ------------------------------------------------------------------ */
#define MP_BARRIER_MS 200
#define MP_SYNC_INTERVAL	30	/* host resync snapshot cadence (frames) */
#define MP_SYNC_SNAP_DIST	600	/* divergence past which the remote car is corrected. Was 300: the host's correction of the client's car fired on ordinary simulation differences, which reads as the host overcorrecting. */
#define MP_SYNC_HARD_DIST	3000	/* divergence past which the correction TELEPORTS. Anything closer is eased across a quarter of the gap per snapshot instead, because a hard snap is the "the car warped weirdly" the player sees. */

int MpInputForPlayer(int id)
{
	if (id < 0 || id >= MP_MAX_PLAYERS)
		return 0;

	return gMp.padForPlayer[id];
}


/* forward: the owner's own-car replication (defined later) */
static void MpSendOwnCarState(void);
static void MpTestCarChangeTick(void);
static int  MpCarIsSomeones(int slot);

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

/* The pad the local player is holding this frame, in the engine's own mapped
 * bit space (what ProcessCarPad would read for a local car). */
static int MpLocalPad(void)
{
	/* The pad that actually drove OUR car last frame, whatever produced it. Reading
	 * Pads[] alone missed the test bot, which injects its pad in the NET_INPUT hook
	 * rather than into the pad state, so a bot run replicated 0 and the peer's copy
	 * of our car only ever moved when a resync snapped it there. */
	if (gMpLocalAppliedPad >= 0)
		return gMpLocalAppliedPad;

	{
		int id = gMp.localPlayerId;
		int padId = 0;

		if (id >= 0 && id < MP_MAX_PLAYERS)
		{
			MP_PLAYER* me = &gMp.players[id];

			if (me->carId >= 0 && me->carId < MAX_CARS && car_data[me->carId].ai.padid != NULL)
			{
				int p = *car_data[me->carId].ai.padid;

				if (p >= 0 && p < 2)
					padId = p;
			}
		}

		return (int)Pads[padId].mapped;
	}
}

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

void MpSendInput(int pad)
{
	unsigned char buf[sizeof(MP_INPUT) + MP_MAX_PLAYERS * sizeof(MP_PLAYER_INPUT)];
	MP_INPUT h;
	int i, k = 0, len;

	if (!gMp.running || gMp.localPlayerId < 0 || gMp.localPlayerId >= MP_MAX_PLAYERS)
		return;

	/* our own row is part of the set whichever side we are */
	gMp.padForPlayer[gMp.localPlayerId] = pad;

	memset(&h, 0, sizeof(h));
	h.frame = (uint32_t)gMp.frame;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER_INPUT r;

		if (!gMp.players[i].active)
			continue;

		/* the host only forwards input it has actually seen */
		if (i != gMp.localPlayerId && !gMp.inputHave[i])
			continue;

		memset(&r, 0, sizeof(r));
		r.playerId = (uint8_t)i;
		r.pad = (uint16_t)gMp.padForPlayer[i];

		memcpy(buf + sizeof(MP_INPUT) + (size_t)k * sizeof(r), &r, sizeof(r));
		k++;
	}

	h.count = (uint8_t)k;
	memcpy(buf, &h, sizeof(h));

	len = (int)(sizeof(MP_INPUT) + (size_t)k * sizeof(MP_PLAYER_INPUT));

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_INPUT, 0, buf, len);
	else
		MpSendToHost(MP_TAG_INPUT, 0, buf, len);
}

static void MpHandleInput(const unsigned char* p, int len)
{
	MP_INPUT h;
	int n, i;

	if (len < (int)sizeof(MP_INPUT))
		return;

	memcpy(&h, p, sizeof(h));

	n = h.count;
	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	if (len < (int)(sizeof(MP_INPUT) + (size_t)n * sizeof(MP_PLAYER_INPUT)))
		return;

	for (i = 0; i < n; i++)
	{
		MP_PLAYER_INPUT r;

		memcpy(&r, p + sizeof(MP_INPUT) + (size_t)i * sizeof(MP_PLAYER_INPUT), sizeof(r));

		if (r.playerId >= MP_MAX_PLAYERS)
			continue;

		/* our own row comes back to us in the host's set: our local pad is the
		 * truth for our own car, so leave it alone */
		if (r.playerId == gMp.localPlayerId)
			continue;

		gMp.padForPlayer[r.playerId] = (int)r.pad;
		gMp.inputHave[r.playerId] = 1;
	}
}

/* Line every player car up on one patch of road, spaced out, all facing the
 * same way.
 *
 * The meeting point has to come from ONE machine. If each side simply puts the
 * other players' cars next to itself, the two placements disagree, the resync
 * sees a big divergence and drags them apart again -- cars rubber-banding
 * between spawn points. The host is that machine: it already owns the roster,
 * and its own car is standing at a spawn the level chose.
 */
void MpPlaceSpawns(int x, int y, int z, int heading)
{
	/* RETIRED: this used to teleport EVERY car onto the host's car spot with the
	 * host's car Y. That is what put the cars in the air -- a single y taken from
	 * the host and applied at every other car's x/z leaves them above or below the
	 * ground actually under them, and the engine then pulls them down (logs showed
	 * the local car at y=75 falling to 26 on the first sim frames).
	 *
	 * It was there because the two machines were thought to spawn on opposite
	 * sides of the map. They do not: with the line-up off, BOTH machines place
	 * both cars at exactly the same x/z/y (the engine's own spawn is
	 * deterministic), so the map's baked start is already agreed on and there is
	 * nothing to correct. Kept as a logged no-op so the call sites stay obvious. */
	if (gMpCtx != NULL)
	{
		static unsigned long lastMs;

		if ((MpNowMs() - lastMs) > 1000)
		{
			lastMs = MpNowMs();
			gMpCtx->jer_log(gMpCtx,
				"[mp] spawn: using the map's own start (line-up retired; was %d,%d,%d)\n", x, y, z);
		}
	}
}

static void MpHandleSpawn(const unsigned char* p, int len)
{
	MP_SPAWN s;

	if (len < (int)sizeof(s))
		return;

	memcpy(&s, p, sizeof(s));
	MpPlaceSpawns(s.x, s.y, s.z, s.heading);
}

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
#define MP_FIXEDH		4096	/* the velocity fix-point scale (FIXEDH / units << 12) */
#define MP_HIT_RADIUS		900	/* world units; car body scale */
#define MP_HIT_MIN_CLOSING	4	/* whole units/frame; don't fire on mere adjacency */
#define MP_HIT_PUSH		60	/* per-cent of the closing speed given up */
#define MP_HIT_MAX_PUSH		20	/* whole units/frame cap */
#define MP_HIT_ARM_FRAMES	120	/* let the spawn/drop settle before contacts count */
#define MP_HIT_COOLDOWN_FRAMES	15

static void MpHitFrame(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* mine;
	int i;
	static unsigned long armFrame;   /* contacts only count after the spawn settles */

	if (me == NULL || me->carId < 0)
		return;

	if (armFrame == 0)
		armFrame = gMp.frame + MP_HIT_ARM_FRAMES;
	if (gMp.frame < armFrame)
		return;

	mine = &car_data[me->carId];

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* pl = &gMp.players[i];
		CAR_DATA* other;
		long dx, dy, dz, d2, d, px, py, pz, closing, push;
		MP_HIT h;

		if (!pl->active || pl->isLocal || pl->id == me->id || pl->carId < 0)
			continue;
		if (gMp.frame - pl->lastHitFrame < MP_HIT_COOLDOWN_FRAMES)
			continue;

		other = &car_data[pl->carId];

		dx = other->hd.where.t[0] - mine->hd.where.t[0];
		dy = other->hd.where.t[1] - mine->hd.where.t[1];
		dz = other->hd.where.t[2] - mine->hd.where.t[2];
		d2 = dx * dx + dy * dy + dz * dz;

		if (d2 == 0 || d2 > (long)MP_HIT_RADIUS * MP_HIT_RADIUS)
			continue;

		d = (long)sqrt((double)d2);
		if (d == 0)
			continue;

		px = dx * MP_FIXEDH / d;	/* unit normal in the velocity fix-point, US -> THEM */
		py = dy * MP_FIXEDH / d;
		pz = dz * MP_FIXEDH / d;

		closing = ((mine->st.n.linearVelocity[0] - other->st.n.linearVelocity[0]) * px
			+ (mine->st.n.linearVelocity[1] - other->st.n.linearVelocity[1]) * py
			+ (mine->st.n.linearVelocity[2] - other->st.n.linearVelocity[2]) * pz) / MP_FIXEDH;

		if (closing < MP_HIT_MIN_CLOSING)
			continue;

		push = closing * MP_HIT_PUSH / 100;
		if (push > MP_HIT_MAX_PUSH)
			push = MP_HIT_MAX_PUSH;

		/* we give up the closing component ... */
		mine->st.n.linearVelocity[0] -= px * push / MP_FIXEDH;
		mine->st.n.linearVelocity[1] -= py * push / MP_FIXEDH;
		mine->st.n.linearVelocity[2] -= pz * push / MP_FIXEDH;

		/* ... and ask THEIR owner to give it up too. */
		memset(&h, 0, sizeof(h));
		h.targetId = (uint8_t)pl->id;
		h.impulse[0] = px * push / MP_FIXEDH;
		h.impulse[1] = py * push / MP_FIXEDH;
		h.impulse[2] = pz * push / MP_FIXEDH;

		pl->lastHitFrame = gMp.frame;

		if (MpIsHost())
		{
			int ci = MpConnFindByPlayer(pl->id);

			if (ci >= 0)
				MpSendConn(ci, MP_TAG_HIT, 0, &h, sizeof(h));
		}
		else
			MpSendToHost(MP_TAG_HIT, 0, &h, sizeof(h));

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] hit: we bumped player %d (closing %ld, push %ld,%ld,%ld)\n",
				pl->id, closing, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
	}
}

static void MpHandleHit(int connIndex, const unsigned char* p, int len)
{
	MP_HIT h;
	MP_PLAYER* me = MpLocalPlayer();

	if (len < (int)sizeof(h))
		return;

	memcpy(&h, p, sizeof(h));

	if (me != NULL && h.targetId == me->id && me->carId >= 0)
	{
		CAR_DATA* cp = &car_data[me->carId];

		cp->st.n.linearVelocity[0] += h.impulse[0];
		cp->st.n.linearVelocity[1] += h.impulse[1];
		cp->st.n.linearVelocity[2] += h.impulse[2];

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] hit: player %d bumped us (push %ld,%ld,%ld)\n",
				MpConnPlayerId(connIndex), (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
		return;
	}

	/* The host is the hub: a hit aimed at another CLIENT is relayed to it. */
	if (MpIsHost())
	{
		int ci = MpConnFindByPlayer(h.targetId);

		if (ci >= 0 && ci != connIndex)
			MpSendConn(ci, MP_TAG_HIT, 0, p, len);
	}
}

/* One network tick (per simulation frame). Each machine owns ITS OWN car and
 * broadcasts that; everyone else adopts it, so nobody blocks on the network and
 * every car you see is the truth of the machine driving it. */
void MpLockstepFrame(void)
{
	if (!gMp.running)
		return;

	++gMp.frame;

	/* test lever: a scripted mid-session car change (inert unless MP_TEST_CARCHANGE) */
	MpTestCarChangeTick();

	/* tell everyone where our wheel is pointing before anything is simulated */
	MpSendInput(MpLocalPad());

	/* Snapshots correct drift across the two simulations; they are NOT the
	 * pose. Placing every car every frame is what made them puppets and threw
	 * away collision responses, so they go out on an interval instead. */
	/* refreshed so the pause menu's names/vehicles/ping are live, and so a
	 * player joining a match already in progress learns the roster */
	if ((gMp.frame % 120) == 0)
		MpHostSendRoster();

	/* OWNER-AUTHORITATIVE: every machine sends the ONE car it owns, every frame,
	 * and everyone else adopts that state. The old model had each machine simulate
	 * every car from input that arrived a round trip late and hoped a coarse resync
	 * would pull them back together -- which is exactly where the drift came from. */
	MpSendOwnCarState();

	/* owner-authoritative contacts: push ourselves, tell the peer's owner */
	MpHitFrame();

	MpNetPoll(0);
}

/* ------------------------------------------------------------------ */
/* Owner-authoritative car replication                                 */
/*                                                                     */
/* Each machine sends the ONE car it owns, every frame. Its owner is   */
/* the truth for that car and every other machine adopts the state, so */
/* nobody is guessing where somebody else's car is from delayed input. */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* MP_TEST_CARCHANGE=<seconds> -- test lever for a MID-SESSION CAR CHANGE.
 *
 * N seconds into a live match, get out of our car and into the nearest civilian
 * one, which is exactly what a player does in Take a Ride. It calls the ENGINE's
 * own ChangePedPlayerToCar -- the function the ped mechanic calls -- so the path
 * under test is the real one and the mod does not poke player[] itself.
 * Inert unless the env var is set. */
static void MpTestCarChangeTick(void)
{
	static int stage;
	static unsigned long startMs;
	const char* s;
	const char* comma;
	int secs, exitSecs = 0, i, best = -1, bestDiff = -1;
	long bestD = 0, bestDiffD = 0;
	MP_PLAYER* me;
	CAR_DATA* mine;
	unsigned long now;

	if (stage >= 2)
		return;

	s = getenv("MP_TEST_CARCHANGE");

	if (s == NULL || !gMp.running)
		return;

	secs = atoi(s);

	/* MP_TEST_CARCHANGE=<changeSecs>[,<exitSecs>] -- the second number makes it
	 * also GET OUT that long after the change, so the peer-side on-foot path (and
	 * the engine's own player -> CIV_AI handover) is exercised as well. */
	comma = strchr(s, ',');
	if (comma != NULL)
		exitSecs = atoi(comma + 1);

	if (startMs == 0)
		startMs = MpNowMs();

	now = MpNowMs();

	if (stage == 1)
	{
		if (exitSecs <= 0 || (int)((now - startMs) / 1000) < secs + exitSecs)
			return;

		stage = 2;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] TEST car change: getting OUT now\n");

		ChangeCarPlayerToPed(0);
		return;
	}

	if ((int)((now - startMs) / 1000) < secs)
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0)
		return;

	mine = &car_data[me->carId];

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		long dx, dy, dz, d;

		/* ONLY a proper civilian car. Taking over a car that is NOT one (a spooled
		 * placeholder, a special) and then vacating it hands the traffic AI a car
		 * whose civ-AI state was never set up, and it crashes in CivSteerAngle. Keep
		 * the lever to the same kind of car a player could actually get into. */
		if (i == me->carId || cp->controlType != CONTROL_TYPE_CIV_AI)
			continue;

		if (MpCarIsSomeones(i))
			continue;

		/* squared distances are 64 bit: a 32-bit one wraps negative on a far car
		 * and reads as the NEAREST one */
		dx = (long)cp->hd.where.t[0] - mine->hd.where.t[0];
		dy = (long)cp->hd.where.t[1] - mine->hd.where.t[1];
		dz = (long)cp->hd.where.t[2] - mine->hd.where.t[2];
		d = dx * dx + dy * dy + dz * dz;

		if (best < 0 || d < bestD)
		{
			best = i;
			bestD = d;
		}

		/* prefer a car of a DIFFERENT model, so the test always exercises the
		 * model swap and not just a move between two cars of the same kind */
		if (cp->ap.model != mine->ap.model && (bestDiff < 0 || d < bestDiffD))
		{
			bestDiff = i;
			bestDiffD = d;
		}
	}

	if (bestDiff >= 0)
	{
		best = bestDiff;
		bestD = bestDiffD;
	}

	if (best < 0)
		return;		/* no traffic nearby yet -- try again next frame */

	/* what was there to choose from, and what we are -- so a run says whether the
	 * takeover could even change the model */
	if (gMpCtx != NULL)
	{
		char list[160];
		int c, shown = 0;

		list[0] = 0;

		for (c = 0; c < MAX_CARS && shown < 8; c++)
		{
			size_t used;

			if (c == me->carId || car_data[c].controlType == CONTROL_TYPE_NONE)
				continue;

			used = strlen(list);
			snprintf(list + used, sizeof(list) - used, " %d:m%d", c, car_data[c].ap.model);
			shown++;
		}

		gMpCtx->jer_log(gMpCtx,
			"[mp] TEST car change: we are model %d; what is in the world:%s\n",
			mine->ap.model, list);
	}

	stage = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] TEST car change: taking over slot %d (model %d), %ld away\n",
			best, car_data[best].ap.model, bestD);

	ChangePedPlayerToCar(0, &car_data[best]);
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
static void MpFollowLocalCar(void)
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

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] car change: now driving slot %d model %d (was slot %d)\n",
				driven, cp->ap.model, me->carId);

		me->carId = driven;
		me->car = cp->ap.model;
		me->carIsSlot = 0;		/* 'car' is a real model now, not a slot */
		me->palette = cp->ap.palette;
	}
	else if (me->carId >= 0)
	{
		/* ON FOOT. The car we left is the ENGINE's to keep: it has already been
		 * handed back to CIV_AI and stays in the world where it was. We only
		 * report "no car" (model 0xFF) so the peers let theirs go too. */
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] car change: on foot (left slot %d)\n",
				me->carId);

		me->carId = -1;
		me->car = -1;
	}
}

static void MpSendOwnCarState(void)
{
	unsigned char buf[sizeof(MP_CARSTATE) + sizeof(MP_CARSTATE_ENTRY)];
	MP_CARSTATE h;
	MP_CARSTATE_ENTRY e;
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;
	int len;

	if (me == NULL)
		return;

	/* The engine may have moved us into another car (or out of one) since the
	 * last frame -- adopt it BEFORE reading the pose, so this very frame already
	 * carries the change and the peers never see the old vehicle again. */
	MpFollowLocalCar();

	memset(&h, 0, sizeof(h));
	h.frame = gMp.frame;
	h.count = 1;

	memset(&e, 0, sizeof(e));
	e.playerId = (uint8_t)me->id;
	e.flags = MP_CARSTATE_HAS_BODY;

	/* NO CAR (on foot): model 0xFF and NO pose. The peer must not adopt the zero
	 * position -- that would drag the car it was driving to the origin -- it
	 * releases that car instead. */
	e.model = MP_CARSTATE_NO_CAR;
	e.carSlot = MP_CARSTATE_NO_CAR;

	if (me->carId >= 0 && me->carId < MAX_CARS)
	{
		cp = &car_data[me->carId];

		e.palette = (uint8_t)cp->ap.palette;	/* the colour WE see our car in -- we own it */
		e.model = (uint8_t)cp->ap.model;	/* ...and WHAT we are driving */
		e.carSlot = (uint8_t)me->carId;		/* so a peer can drive the same car */
		e.x = cp->hd.where.t[0];
		e.y = cp->hd.where.t[1];
		e.z = cp->hd.where.t[2];
		e.heading = cp->hd.direction;
		e.orient[0] = (int16_t)cp->st.n.orientation[0];
		e.orient[1] = (int16_t)cp->st.n.orientation[1];
		e.orient[2] = (int16_t)cp->st.n.orientation[2];
		e.orient[3] = (int16_t)cp->st.n.orientation[3];
		e.vel[0] = cp->st.n.linearVelocity[0];
		e.vel[1] = cp->st.n.linearVelocity[1];
		e.vel[2] = cp->st.n.linearVelocity[2];
		e.angVel[0] = (int16_t)cp->st.n.angularVelocity[0];
		e.angVel[1] = (int16_t)cp->st.n.angularVelocity[1];
		e.angVel[2] = (int16_t)cp->st.n.angularVelocity[2];
	}

	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), &e, sizeof(e));
	len = (int)(sizeof(h) + sizeof(e));

	/* The host is the hub: it broadcasts its OWN car to every client, and relays
	 * each client's car on (MpHandleCarState does the relay). A client sends its
	 * one car to the host. */
	if (MpIsHost())
		MpHostBroadcast(MP_TAG_CARSTATE, 0, buf, len);
	else
		MpSendToHost(MP_TAG_CARSTATE, 0, buf, len);
}

/* ------------------------------------------------------------------ */
/* The REMOTE player's car: keeping it on the vehicle its owner is driving.
 *
 * A player who gets out of one car and into another changes vehicles mid-session
 * (see MpFollowLocalCar). The carstate carries the model now, so we can follow
 * it; these helpers make our copy match. */

/* Is this CAR_DATA slot already spoken for by some OTHER player? */
static int MpCarIsSomeones(int slot)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = MpGetPlayer(i);

		if (p != NULL && p->active && p->carId == slot)
			return 1;
	}

	return 0;
}

/* A remote player got OUT. The car we were driving for them is left exactly as
 * it is, standing where they left it.
 *
 * DELIBERATELY NOT handed to the traffic AI. That car is one the MOD created
 * (InitPlayer), so its civil-AI state was never set up; flipping its controlType
 * to CIV_AI hands it to the engine's traffic AI, which then steers a car with no
 * AI data -- an access violation inside CivSteerAngle (found from a dump:
 * REDRIVER2_dev.exe rva 0xC961). Leaving it a player car and simply never sending
 * it anything again does what was asked for ANYWAY: it stays put (the engine's
 * input fallback coasts it to a stop) instead of an AI driving it away. */
static void MpReleaseRemoteCar(MP_PLAYER* p)
{
	CAR_DATA* cp;

	if (p == NULL || p->carId < 0 || p->carId >= MAX_CARS)
		return;

	cp = &car_data[p->carId];

	if (cp->controlType == CONTROL_TYPE_PLAYER)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d got out; car slot %d left standing where it was\n",
				p->id, p->carId);
	}
}

/* Make this player's car in OUR world the vehicle their owner just got into.
 *
 * IN PLACE, on the slot we already drive for them -- the safe default: only the
 * cosmetic model and the colour change, so nothing about this world's car slots
 * is disturbed and a wrong guess cannot hijack an unrelated car.
 *
 * RE-LINK only when it is SAFE: the slot the owner named is a plain traffic car
 * HERE -- same model, CIV_AI, claimed by nobody -- so our copy is literally the
 * same car in both worlds and the pose we adopt lands on the right entity. */
static void MpAdoptRemoteCar(MP_PLAYER* p, int model, int slot)
{
	CAR_DATA* cp = NULL;

	if (p == NULL)
		return;

	if (slot >= 0 && slot < MAX_CARS && slot != p->carId)
	{
		CAR_DATA* other = &car_data[slot];

		if (other->ap.model == model
		    && other->controlType == CONTROL_TYPE_CIV_AI
		    && !MpCarIsSomeones(slot))
		{
			MpReleaseRemoteCar(p);		/* what they left goes back */

			p->carId = slot;
			cp = other;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] player %d changed car: re-linked to slot %d (model %d)\n",
					p->id, slot, model);
		}
	}

	if (cp == NULL && p->carId >= 0 && p->carId < MAX_CARS)
		cp = &car_data[p->carId];

	if (cp == NULL)
		return;

	/* SWAP IN PLACE -- but only to a model the renderer actually HAS. Pointing
	 * ap.model at a mesh we never loaded is a crash, not a cosmetic glitch, so an
	 * unavailable model keeps the old one and says so. */
	if (cp->ap.model != model)
	{
		if (model >= 0 && model < MAX_CAR_RESIDENT_MODELS
		    && gCarCleanModelPtr[model] != NULL)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] player %d changed car: model %d -> %d (slot %d)\n",
					p->id, cp->ap.model, model, p->carId);

			cp->ap.model = model;
			p->car = model;
			p->carIsSlot = 0;
		}
		else if (gMpCtx != NULL)
		{
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d changed car: model %d is not loaded here; keeping %d\n",
				p->id, model, cp->ap.model);
		}
	}
}

static void MpHandleCarState(int connIndex, const unsigned char* p, int len)
{
	MP_CARSTATE h;
	int i, n;

	if (len < (int)sizeof(MP_CARSTATE))
		return;

	memcpy(&h, p, sizeof(h));
	n = h.count;
	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	if (len < (int)(sizeof(MP_CARSTATE) + (size_t)n * sizeof(MP_CARSTATE_ENTRY)))
		return;

	for (i = 0; i < n; i++)
	{
		MP_CARSTATE_ENTRY e;
		MP_PLAYER* pl;
		CAR_DATA* cp;

		memcpy(&e, p + sizeof(MP_CARSTATE) + i * sizeof(e), sizeof(e));

		pl = MpGetPlayer(e.playerId);

		if (pl == NULL || pl->isLocal)
			continue;

		/* NO CAR: the owner is on foot. Let the car we were driving for them go
		 * back to the world -- it stays where they left it (their own engine
		 * does the same), so nobody is left steering an empty car. */
		if (e.model == MP_CARSTATE_NO_CAR)
		{
			MpReleaseRemoteCar(pl);
			continue;
		}

		if (pl->carId < 0 || pl->carId >= MAX_CARS)
			continue;

		cp = &car_data[pl->carId];

		/* The owner changed VEHICLE: match it BEFORE adopting this pose, so the
		 * body we are about to write lands on the right car.
		 *
		 * Compare the model the car ACTUALLY renders with, not pl->car: the roster
		 * refresh (every 120 frames) writes the owner's new model into pl->car
		 * long before the car itself is changed, so gating on pl->car silently
		 * agreed with the roster while the car on screen stayed the old one. */
		if (cp->ap.model != (int)e.model)
		{
			MpAdoptRemoteCar(pl, (int)e.model, (int)e.carSlot);

			if (pl->carId < 0 || pl->carId >= MAX_CARS)
				continue;

			cp = &car_data[pl->carId];
		}

		/* OWNER-AUTHORITATIVE: this car belongs to another machine, so its owner is
		 * the truth. Adopt the WHOLE body every snapshot -- no easing, no tolerance.
		 * (The accepted cost: because our engine's response to a contact is
		 * overwritten by the owner's next frame, a car we drive into is not pushed.) */
		{
			long dx = (long)e.x - (long)cp->hd.where.t[0];
			long dy = (long)e.y - (long)cp->hd.where.t[1];
			long dz = (long)e.z - (long)cp->hd.where.t[2];
			long d2 = dx * dx + dy * dy + dz * dz;
			int dh = ((e.heading - cp->hd.direction + 2048) & 4095) - 2048;

			/* The HOST<->CLIENT deviation for this REMOTE car: how far our own
			 * simulation of it is from its owner's snapshot. One line per
			 * snapshot that arrives, for every remote car. A small steady value is
			 * input latency; a growing one is the two simulations drifting apart;
			 * a spiky one is the resync fighting the engine. This is the number to
			 * watch for "the cars are not where they are on the other machine".
			 * (|d| is the world-unit distance, dh the heading error in 1/4096
			 * turns.) Logged on arrival, NOT gated on our own frame counter: the
			 * peer's snapshot arrives on ITS cadence, which need not line up with
			 * ours -- gating on `frame % 30` silently logged nothing. */
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] adopt: player %d snap %u |d|=%ld d2=%ld (dx=%ld dy=%ld dz=%ld) dh=%d pal=%d md=%d\n",
					e.playerId, (unsigned)h.frame, (long)sqrt((double)d2), d2, dx, dy, dz, dh, (int)e.palette, (int)e.model);

			/* Adopt in full: the owner is the truth for its own car. */

			/* keep the remote car placed and alive: the engine spools a
			 * non-local player car out of the world, so re-assert it
			 * (controlType included, or the spooler parks it). */
			cp->controlType = CONTROL_TYPE_PLAYER;

			{
				int tx = e.x, ty = e.y, tz = e.z;

				cp->hd.where.t[0] = tx;
				cp->hd.where.t[1] = ty;
				cp->hd.where.t[2] = tz;

				/* Write the WHOLE body, not a position and a heading: hd.direction
				 * is an OUTPUT the engine re-derives from st.n.orientation, so a
				 * correction that set only the heading left the car's ATTITUDE
				 * unsynced (driving around upside down). Rebuild the handling matrix
				 * with the engine's own quaternion helper rather than poking
				 * hd.where. */
				if (e.flags & MP_CARSTATE_HAS_BODY)
				{
					LONGQUATERNION q;
					MATRIX m;
					int i;

					for (i = 0; i < 4; i++)
						q[i] = e.orient[i];

					cp->st.n.orientation[0] = q[0];
					cp->st.n.orientation[1] = q[1];
					cp->st.n.orientation[2] = q[2];
					cp->st.n.orientation[3] = q[3];

					for (i = 0; i < 3; i++)
						cp->st.n.linearVelocity[i] = e.vel[i];
					for (i = 0; i < 3; i++)
						cp->st.n.angularVelocity[i] = e.angVel[i];

					LongQuaternion2Matrix(&q, &m);
					m.t[0] = tx;
					m.t[1] = ty;
					m.t[2] = tz;
					memcpy(&cp->hd.where, &m, sizeof(m));
				}

			}
		}

		cp->hd.direction = e.heading;
		pl->lastStateFrame = gMp.frame;	/* the fallback gate in MpOnNetInput reads this */

		/* The OWNER is the colour authority: paint its car the colour the owner
		 * sees. cp->ap.palette is exactly what the renderer hands to
		 * DrawCarObject, so this recolours the car on the very next frame. */
		if (!pl->isLocal && cp->ap.palette != (u_char)e.palette)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] palette: player %d %d -> %d (from owner)\n",
					pl->id, (int)cp->ap.palette, (int)e.palette);

			cp->ap.palette = (u_char)e.palette;
		}

		/* Client-side gather: the first time we hear a peer's car, drop our
		 * own car right next to it so both players start together -- each
		 * machine's take-a-ride spawn point can be right across the map. */
		if (!MpIsHost() && !gMp.localPlaced)
		{
			MP_PLAYER* me = MpLocalPlayer();

			if (me != NULL && me->carId >= 0 && me->carId != pl->carId)
			{
				CAR_DATA* my = &car_data[me->carId];
				MATRIX m;

				/* The gather uses the PEER's resolved y (e.y), so it does not lift
				 * our car -- same trap as the spawn, and it is not hit here. */
				my->hd.where.t[0] = e.x + 600;
				my->hd.where.t[1] = e.y;
				my->hd.where.t[2] = e.z;
				my->hd.direction = e.heading;

				/* Rebuild the handling matrix too: setting only t[]/direction left the
				 * collision box at the OLD spot until the engine next recomputed it --
				 * and this is a teleport, so it matters. */
				_RotMatrixY(&m, (short)e.heading);
				memcpy(my->hd.where.m, m.m, sizeof(my->hd.where.m));

				gMp.localPlaced = 1;

				if (gMpCtx)
					gMpCtx->jer_log(gMpCtx, "[mp] gathered next to peer at %d,%d,%d\n", e.x, e.y, e.z);
			}
		}

		/* The host is the hub: pass a CLIENT's car on to the other clients, so every
		 * machine sees every car (each sends only its own). */
		if (MpIsHost() && connIndex >= 0)
			MpHostRelay(connIndex, MP_TAG_CARSTATE, 0, p, len);
	}
}

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

void MpHandleMessage(int connIndex, const char* tag, const unsigned char* payload, int len)
{
	if (getenv("MP_DEBUG") != NULL && gMpCtx != NULL)
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
		MpHandleInput(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_SPAWN, 4) == 0)
	{
		MpHandleSpawn(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_ROSTER, 4) == 0)
	{
		MpHandleRoster(payload, len);
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

	if (memcmp(tag, MP_TAG_HIT, 4) == 0)
	{
		MpHandleHit(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_CHAT, 4) == 0)
	{
		MpHandleChat(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_LEAVE, 4) == 0)
	{
		MpHandleLeave(payload, len);
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

	/* SESSION is not used yet; unknown tags are ignored. */
}
