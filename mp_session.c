/*
 * mp_session.c -- JERICHO Multiplayer session: bring-up, the join handshake
 * (with the mod-match lobby policy), the per-frame owner-authoritative car
 * sync and input replication, the roster, the on-foot stand-ins, chat, and the
 * send/recv dispatch that the transport (mp_net.c) calls into.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_pause_menu.h"	/* JER_PAUSE_QUIT_*: the pause menu's codes (mirror MENU_QUIT_*) */
#include "mp.h"
#include "mp_carquery.h"	/* the mp <-> carhacks live-car query contract */

#include "driver2.h"
#include "mission.h"
#include "glaunch.h"
#include "replays.h"
#include "pad.h"
#include "cars.h"
#include "convert.h"	/* _RotMatrixY: a car's box is built from its matrix */
#include "cosmetic.h"	/* car_cosmetics[slot]: the box a rebuilt car needs */
#include "system.h"	/* CITY_COUNT and LevelNames: the city registry MpCarCityName reads */
#include "denting.h"	/* CreateDentableCar: the only writer of the drawn vertex dump */
#include "civ_ai.h"	/* reservedSlots: the array PingInCivCar's free-slot scan reads */
#include "felony.h"	/* felonyRating / pedestrianFelony: what a restart clears */
extern int gBootMpLevel;	/* main.c: 1 = the small multiplayer map, 0 = the full city */
extern int gBootMpArena;	/* main.c: which multiplayer map (0/1) */
extern int gWantNight;		/* glaunch.c: 1 = the night take-a-ride level variant */
#include "players.h"	/* InitPlayer: a late joiner needs a car the same way the engine makes one */
#include "handling.h"	/* LongQuaternion2Matrix: rebuild a car's matrix from its body */
#include "state.h"
#include "dr2roads.h"	/* FindSurfaceD2: the real ground height at a point */
#include "pedest.h"	/* pUsedPeds: our player's own pedestrian, when on foot */
#include "jer_npc.h"	/* jer_npc_*: the pedestrian we stand in for a REMOTE one */
#include "jer_ped_palette.h"	/* jer_ped_palette_reset: scope the suit-colour cache to a session */

#include <string.h>
#include <stdio.h>
#include <math.h>	/* sqrt: the deviation readout below */

/* WHICH CAR SLOTS ARE OURS.
 *
 * Sticky: once the module has put a player in a car, that slot is a module car for
 * the rest of the session, even after the player gets out of it. It matters
 * because a car you have left is still in the world with nobody driving it, and
 * the engine's traffic AI treats such a car as fair game.
 *
 * Cleared when the session ends: a stale mark would make us take a STOCK traffic
 * car for one of ours on the next match, which would break the traffic instead. */
static unsigned char gMpOurCars[MAX_CARS];

/* Slots that hold a PEER's traffic mirror. The band already keeps the two
 * spawners in disjoint slots, but a mirror must ALSO be remembered as not-ours so
 * teardown removes exactly what we built and nothing of the engine's. */
static unsigned char gMpPeerTraffic[MAX_CARS];	/* 1 = we built a peer's car here */
static unsigned gMpPeerTrafficSeen[MAX_CARS];	/* frame we last heard about a mirror */
static unsigned char gMpTrafficSent[MAX_CARS];	/* slots WE published last frame */
static unsigned gMpTrafficSelfContact[MAX_CARS];/* frame OUR engine resolved a contact with our traffic car */
static unsigned gMpTrafficHitFrame[MAX_CARS];	/* rate limit for reporting a contact with a peer's traffic */

/* Last reported palette value LOGGED per player, so the continuous carstate
 * stream logs a correction once per reported value instead of every packet.
 * File scope (not a function static) so the session can clear it: a leftover
 * "already logged" mark would swallow the first report after a reconnect. */
static signed char gPaletteLogged[MP_MAX_PLAYERS];

/* A pad nobody writes, for cars that must sit still. Pad 1 is free because a match
 * keeps NumPlayers at 1 -- each machine drives the whole screen. Not -1 and not
 * NULL: the engine indexes Pads[] with it and some sites do not check. */
static char gMpQuietPad = 1;

/* Frame at which car-to-car contacts start counting, for THIS match. Session
 * state rather than a function-local: MpSessionReset zeroes gMp.frame between
 * matches, and a function-local would survive holding the PREVIOUS match's
 * value, silently disabling collisions until the frame counter climbed back up
 * to it -- minutes into a second match if the first one ran long. */
static unsigned long gMpHitArmFrame;

/* Frame at which OUR engine last reported a contact with each peer's car. The
 * RECEIVING half of a hand-off consults it: when our own engine has just resolved
 * the same contact its rigid-body response is already in our car's velocity, and
 * the module's push on top of that would land one collision twice. Session state,
 * for the same reason as the arm frame above. */
static unsigned long gMpSelfContactFrame[MP_MAX_PLAYERS];

/* defined below, needed by the launch path above them */
static int MpAssignedCarModel(int playerId);
static int MpPlayerCarReady(const MP_PLAYER* p);

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
	gMpHitArmFrame = 0;	/* collisions arm afresh in every match */
	memset(gMpSelfContactFrame, 0, sizeof(gMpSelfContactFrame));
	gMp.running = 0;
	gMp.leaving = 0;
	gMp.modsMatched = 1;
	gMp.localPlaced = 0;

	/* the palette-correction log is throttled per reported value; clear that
	 * memory so a fresh session reports its first correction */
	memset(gPaletteLogged, 0, sizeof(gPaletteLogged));

	/* A new session starts with a clean suit-colour cache, so a colour minted for
	 * a player in a previous match cannot be handed back for a different one
	 * (the level load clears it too; this makes the scope explicit and
	 * independent of a level reload). */
	jer_ped_palette_reset();
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
			me->carCity = gMp.config.carCity;
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
	 * loopback nobody asked for. Whether the NAME exists is the resolver's
	 * business and is reported at connect time (MpResolveHost). */
	if (!MpIsValidAddress(host))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] '%s' is not a usable host name or address\n", host);

		jer_error("%s is not a valid host", host);
		return 0;
	}

	MpSessionReset();
	MpResetPlayers();

	gMp.role = MP_ROLE_CLIENT;
	gMp.localPlayerId = -1;

	/* A fresh join gets the full set of connection attempts (see MP_JOIN_ATTEMPTS in
	 * mp_net.c: the user asked for three, after a launch where one failed and had to be
	 * fixed by hand in the menu). */
	MpJoinRetryClear();

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

/* forward: session teardown takes the peer's traffic mirrors out of the world
 * (defined with the mirror code, further down). */
static void MpTrafficMirrorsClear(void);

void MpLeaveSession(void)
{
	if (gMpCtx != NULL)
	{
		MpConnEvent("leaving the session", -1, "deliberate");

		gMpCtx->jer_log(gMpCtx, "[mp] MpLeaveSession (role=%d running=%d)\n", (int)gMp.role, gMp.running);
	}

	gMp.leaving = 1;

	/* if we are the host, tell everyone the match is ending so they go back
	 * to their frontend with a notice rather than a bare connection loss */
	if (gMp.role == MP_ROLE_HOST)
		MpHostByeAll();
	else
		MpClientBye();	/* and if we are the client, tell the host WE are going */

	MpClientDisconnect();
	MpHostEnd();
	MpDiscoveryStop();
	MpResetPlayers();

	gMp.role = MP_ROLE_NONE;
	gMp.connected = 0;
	gMp.running = 0;

	/* The cars stay in the world, but they stop being ours the moment the session
	 * does: a stale mark would make the next match take a STOCK traffic car for
	 * one of ours and break the traffic instead of protecting it. */
	memset(gMpOurCars, 0, sizeof(gMpOurCars));

	/* ...and the peer's traffic mirrors, which are OUR builds of THEIR cars. */
	MpTrafficMirrorsClear();

	/* ...and the traffic bands stop being ours too: the engine's reservedSlots must
	 * go back to the engine once we are not replicating traffic (see the band
	 * section below). Leaving them set would keep stock traffic out of six slots
	 * for no reason after the session. */
	MpTrafficBandClear();
}

/* Launch the agreed level locally: set the pending globals from the session
 * and enter the game. NumPlayers stays 1 so each machine gets the FULL screen
 * (no split-screen); the other players are puppet cars managed by the module
 * and placed from the network every frame. */
/* Tell the host which car this player ended up with. The HELLO carried the CONFIG
 * value at CONNECT time - long before the player reached the car select - so
 * without this every other machine drew a car they never chose. Only a client
 * sends: the host's own car is in the roster it builds anyway. */
static void MpSendPickedCar(int model, int city)
{
	MP_CAR c;

	if (MpIsHost() || !gMp.connected)
		return;

	memset(&c, 0, sizeof(c));
	c.model = (model >= 0 && model < 0xFF) ? (uint8_t)model : 0xFF;
	c.city = (city >= 0 && city < CITY_COUNT) ? (uint8_t)city : 0xFF;

	MpSendToHost(MP_TAG_CAR, 0, &c, sizeof(c));

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] told the host our car: model %d city %d\n",
			(int)c.model, (int)c.city);
}

static void MpLaunchLocal(void)
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
	extern char carNumLookup[CITY_COUNT][10];

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

/* The CITY a player's chosen vehicle belongs to, paired with MpPlayerCarModel's
 * model number. A "slotN" pick is a per-city frontend SLOT, so it resolves in the
 * SESSION's city; a raw model number may name a city explicitly (-mpcar
 * city:model). Either way the default is MP_CAR_CITY_SESSION -- the level's own. */
static int MpPlayerCarCity(int city, int isSlot)
{
	if (isSlot || city < 0 || city >= CITY_COUNT)
		return MP_CAR_CITY_SESSION;

	return city;
}

/* ------------------------------------------------------------------ */
/* Seating every player: the level's cars, and its two spare slots      */
/* ------------------------------------------------------------------ */

/* The car model each city keeps in its SPECIAL slot (slot 7). MEASURED, by booting
 * each level and reading the residents the engine prints:
 *
 *     chicago   1 2 3 0 4 -1 -1 10
 *     havana    1 2 3 0 4 -1 -1 11
 *     lasvegas  1 3 4 0 2 -1 -1 9
 *     rio       1 2 3 0 4 -1 -1 8
 *
 * So EVERY level seats five usable car models (slots 0..4 -- the set {0,1,2,3,4},
 * in a different order per city), leaves slots 5 and 6 EMPTY (mission.c defaults
 * them to -1 and says they exist for modules to fill), and keeps its own special in
 * slot 7. Five cars is fewer than MP_MAX_PLAYERS, which is exactly what made "every
 * player drives their own car" impossible past the fifth player: the assignment
 * wrapped and handed two players the same model. */
static const int MP_CITY_SPECIAL[4] = { 10, 11, 9, 8 };	/* chicago, havana, lasvegas, rio */

#define MP_EXTRA_SLOTS	2		/* the level's spare resident slots: 5 and 6 */

/* Which extra models mp puts in those slots, per session city -- MEASURED against
 * the level's own car-model table, not guessed (see MP_EXTRA_SLOTS below for how).
 *
 * They are read from the level's OWN file (modelSource = -1), which is the only
 * thing that works: the engine's importer holds ONE source city per level
 * (models.c:446 picks the first slot that names one), so asking for a different
 * city per slot silently looks the model up in the FIRST city's table -- a
 * havana-coded spare slot was read against chicago's file and reported "which has
 * no such model". Reading the level's own table by model index has no such trap.
 *
 * The numbers stay distinct from the five domestic ones and from the session
 * city's own special, so no two player ids can be handed the same car. */
static const int MP_CITY_EXTRA[4][MP_EXTRA_SLOTS] = {
	{ 9, 11 },	/* chicago  (its special is 10) */
	{ 9, 10 },	/* havana   (its special is 11) */
	{ 8, 10 },	/* lasvegas (its special is 9)  */
	{ 9, 10 },	/* rio      (its special is 8)  */
};

static int MpExtraModel(int lvl, int which)
{
	return MP_CITY_EXTRA[lvl & 3][which];
}

/* What the spare slots HOLD this level, as mp left them. The assignment below has
 * to name models that are really resident: naming one a level does not have is not
 * a cosmetic failure, because the car keeps the model it had and the machines then
 * disagree about who drives what. */
static int gMpExtraModel[MP_EXTRA_SLOTS];
static int gMpExtraSet;

/* JER_EVENT_CAR_DATA_SOURCE: fired once per level, before any CARMODEL_* file is
 * read, with the resident model table and a per-slot source city in hand. A match
 * uses it to ask the level for enough distinct cars to seat every player. */
int MpOnCarDataSource(void* userdata, void* args)
{
	JER_ARGS_CAR_DATA_SOURCE* a = (JER_ARGS_CAR_DATA_SOURCE*)args;
	int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
	int i;

	(void)userdata;

	gMpExtraSet = 0;

	for (i = 0; i < MP_EXTRA_SLOTS; i++)
		gMpExtraModel[i] = -1;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	/* MP_EXTRA_SLOTS="slot:model[,slot:model]" chooses the match's spare cars by hand
	 * -- and does it WITHOUT a live session, which is what made the table above
	 * measurable: one `-level <city>` boot per candidate says whether that level's
	 * own table has the model (or takes the game down trying it, which is an answer
	 * too). Inert unless set. */
	{
		const char* over = getenv("MP_EXTRA_SLOTS");
		const char* p = over;

		while (p != NULL && *p != '\0')
		{
			int slot = 0, model = 0;

			while (*p >= '0' && *p <= '9')
				slot = slot * 10 + (*p++ - '0');

			if (*p == ':')
				p++;

			while (*p >= '0' && *p <= '9')
				model = model * 10 + (*p++ - '0');

			if (slot >= 5 && slot < 5 + MP_EXTRA_SLOTS && model > 0)
				gMpExtraModel[slot - 5] = model;

			while (*p != '\0' && *p != ',')
				p++;

			if (*p == ',')
				p++;
		}
	}

	/* Only in a live session (or when asked by hand): a single-player level must not
	 * load cars nobody is going to drive. */
	if (!gMp.running && getenv("MP_EXTRA_SLOTS") == NULL)
		return JER_RESULT_CONTINUE;

	if (a->models == NULL || a->modelSource == NULL ||
	    a->count < 5 + MP_EXTRA_SLOTS)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MP_EXTRA_SLOTS; i++)
	{
		int slot = 5 + i;
		int model = (gMpExtraModel[i] > 0) ? gMpExtraModel[i] : MpExtraModel(lvl, i);

		/* A spare slot another module already claimed is ITS business -- keep what
		 * it put there and let the assignment name it. */
		if (a->models[slot] > 0)
		{
			gMpExtraModel[i] = a->models[slot];

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] resident slot %d already holds model %d (not ours) -- player %d will drive it\n",
					slot, a->models[slot], 5 + i);
			continue;
		}

		a->models[slot] = model;
		a->modelSource[slot] = -1;	/* the level's OWN table: the one source that works */
		gMpExtraModel[i] = model;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] resident slot %d = model %d (the level's own), so player %d has a car of their own\n",
				slot, model, 5 + i);
	}

	gMpExtraSet = 1;

	/* The local player's car was decided before the level loaded, when the spare
	 * slots were not set yet (the launch happens before LoadMission). If this player
	 * is one of the players those slots exist for, name the model that is ACTUALLY
	 * in the slot -- and only when they made no explicit pick, so -mpcar keeps the
	 * last word. */
	if (gMp.localPlayerId >= 5 && gMp.localPlayerId < 5 + MP_EXTRA_SLOTS &&
	    gMp.config.car < 0)
	{
		int i2 = gMp.localPlayerId - 5;

		if (gMpExtraModel[i2] > 0)
		{
			wantedCar[0] = gMpExtraModel[i2];

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] player %d: our own car is the spare slot's model %d\n",
					gMp.localPlayerId, wantedCar[0]);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* The car EVERY machine assigns a player who did not choose one (no -mpcar). It
 * must be the SAME number on every machine AND different per player, so the cars
 * are both distinguishable and agreed on. The level's own car table
 * (carNumLookup, indexed by player id) is exactly that: the same for every city,
 * and READABLE at level init -- unlike the resident models, which the engine has
 * not filled yet at this point (the diag prints residents= 0 0 0 0 ...).
 *
 * Indexed by PLAYER ID, with no modulo. `playerId % 4` wrapped ids 4..7 back onto
 * ids 0..3, so from the FIFTH player two players were handed the same car -- and
 * with the palette owner-authoritative the two machines then showed two cars
 * nobody could tell apart. Eight ids, eight distinct models:
 *
 *   ids 0..4  the level's five domestic cars (carNumLookup's own first five, which
 *             are the five models every level has resident)
 *   ids 5..6  the spare resident slots mp fills (see MpOnCarDataSource)
 *   id 7      the level's special (slot 7)
 *
 * Ids 0..3 keep the models they had before, so a pair behaves exactly as it did.
 *
 * The old code applied this to REMOTE players only and left the LOCAL player on
 * the level's default, so the machines disagreed about the local player's car
 * (the client saw the host as slot 0 with a palette that did not match, because
 * the owner's palette was applied to a different model), and two un-chosen
 * players could both come out as the same model if the level's default coincided
 * with one of the assigned ones. */
static int MpAssignedCarModel(int playerId)
{
	extern char carNumLookup[CITY_COUNT][10];
	int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;

	if (playerId <= 0)
		return carNumLookup[lvl][0];		/* the host */

	if (playerId < 5)
		return carNumLookup[lvl][playerId];	/* the level's own five cars */

	if (playerId < 5 + MP_EXTRA_SLOTS)
	{
		int i = playerId - 5;

		if (gMpExtraSet && gMpExtraModel[i] > 0)
			return gMpExtraModel[i];	/* what the slot really holds */

		return MpExtraModel(lvl, i);
	}

	return MP_CITY_SPECIAL[lvl];			/* the 8th player: the special */
}

/* The city MpAssignedCarModel's model number belongs to: ALWAYS the session's
 * own -- that function indexes the level's car table. Paired with it so a caller
 * names both halves of an identity. */
static int MpAssignedCarCity(void)
{
	return MP_CAR_CITY_SESSION;
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
		e->modelCity = MP_CAR_CITY_SESSION;
		e->reserved = (uint16_t)p->palette;

		/* Whether THIS host has a car for that player yet -- the same gate the
		 * spawn uses, and the only way a third machine can tell "their pick has
		 * landed" (it never sees the pick message, which goes to the host). */
		if (MpPlayerCarReady(p))
			e->flags |= MP_ROSTER_FLAG_CAR_READY;

		/* the car this player drives, resolved here on the host -- so a joiner
		 * knows everyone's car before anything is spawned. An explicit pick keeps
		 * its own city (a cross-city car); a player who chose nothing gets the
		 * model every machine assigns them, which is always the session's city.
		 * The spawn overrides this with the model actually loaded. */
		{
			int m = MpPlayerCarModel(p->car, p->carIsSlot);
			int c = MpPlayerCarCity(p->carCity, p->carIsSlot);

			if (m < 0)
			{
				m = MpAssignedCarModel(id);
				c = MpAssignedCarCity();
			}

			if (m >= 0 && m <= 0xff)
			{
				e->model = (uint8_t)m;
				e->modelCity = (uint8_t)c;
			}
		}

		/* ON FOOT: name no car, whatever the player owns. The roster is only
		 * refreshed every 120 frames, so a peer that seats a player from it
		 * cannot tell "has a car" from "is driving it"; the model field is where
		 * that is said, and 0xff is the same "no car" the carstate uses. Doing
		 * this is what stops a peer re-spawning a car for a player who has got
		 * out (see MP_PLAYER.onFoot). */
		if (p->carId < 0)
			e->model = 0xff;

		e->ping = (uint16_t)(id == 0 ? 0 : MpPingForPlayer(id));
		snprintf(e->name, sizeof(e->name), "%s", p->name);

		if (p->carId >= 0 && p->carId < MAX_CARS)
		{
			CAR_DATA* cp = &car_data[p->carId];

			e->carId = (uint8_t)p->carId;

			/* on foot: nobody is driving that car any more. Otherwise describe it
			 * the same way the carstate does -- a model NUMBER + its city, never
			 * our resident slot index (which means a different car elsewhere). */
			if (cp->controlType == CONTROL_TYPE_NONE ||
				cp->ap.model < 0 || cp->ap.model >= MAX_CAR_RESIDENT_MODELS ||
				residentCarModels[cp->ap.model] < 0)
			{
				e->model = 0xff;
			}
			else
			{
				int src = GetCarModelSourceCity(cp->ap.model);

				e->model = (uint8_t)residentCarModels[cp->ap.model];
				e->modelCity = (uint8_t)((src >= 0) ? src : MP_CAR_CITY_SESSION);
			}
		}
	}

	len = (int)(sizeof(MP_ROSTER) - (size_t)(MP_MAX_PLAYERS - r.count) * sizeof(MP_ROSTER_ENTRY));
	MpHostBroadcast(MP_TAG_ROSTER, 0, &r, len);
}

/* The pool-checked vehicle for one remote player: an explicit -mpcar pick if
 * there is one, else the model EVERY machine independently assigns this player id
 * (MpAssignedCarModel) -- never the local player's model, which is what made two
 * machines simulate different cars for the same player. Writes the start record's
 * model and palette and, for the two slots the engine re-applies, wantedCar.
 * Returns the model chosen.
 *
 * Shared by the level-init spawn (MpOnNetSpawn) and the late-joiner spawn: they
 * carried two copies of this and had already drifted apart once. */
static int MpSetStartCar(int slot, MP_PLAYER* p)
{
	int cid = MpPlayerCarModel(p->car, p->carIsSlot);
	int chosen = (p->carConfirmed || p->id == 0);	/* the host is the authority for its own car */

	if (cid < 0)
		cid = MpAssignedCarModel(p->id);

	/* A GUESS IS NOT A CHOICE, and a guess must never beat the level.
	 *
	 * When this player has not chosen yet, cid came from MpAssignedCarModel and is a
	 * position in the SESSION, not a claim about this level: player 1 -> model 1, which
	 * VEGAS has and CHICAGO does not. Writing that into the start record (and into
	 * wantedCar[slot] below, which the engine applies as its last word before the level
	 * runs) does not stay a number - the engine's own guard swaps it for resident slot 0
	 * ("car model 1 has no data in this level"), or silently hands the player the level's
	 * own car of that number, which is exactly "it only did chicago 2 (domestic)".
	 *
	 * A CHOICE is left alone whatever the level holds: carhacks is responsible for
	 * holding a picked car, that is the import/hotload path. */
	if (!chosen && !JerCarSlotUsable(cid))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d has not chosen a car and the session's model %d is not in this level - leaving the level's own car\n",
				p->id, cid);

		cid = -1;		/* the record keeps its own start car */
	}

	if (cid >= 0)
	{
		PlayerStartInfo[slot]->model = (u_char)cid;
		PlayerStartInfo[slot]->palette = (u_char)(p->palette >= 0 ? p->palette : 0);
	}

	/* wantedCar is 2 entries -- the local players' -- so only slots 0..1 fit. It is
	 * the channel the engine re-applies as its LAST word before the level runs, and
	 * the only place a remote slot's model can be corrected after something resets
	 * it. Only a real choice goes in: see the guard above. */
	if (chosen && slot >= 0 && slot < 2)
		wantedCar[slot] = cid;

	return (cid >= 0) ? cid : (int)PlayerStartInfo[slot]->model;
}

/* Is this player's vehicle ready to be built?
 *
 * Only a player who has CHOSEN a car gets one, so that a joiner's vehicle does not
 * appear on every screen while they are still in the car select. The HOST is the
 * exception, and must be: it is the authority for its own car, it never sends the
 * pick message (only clients do), so gating it would mean a client never builds
 * the host's car at all - "the client can't see the host". Player 0 is always the
 * host. */
static int MpPlayerCarReady(const MP_PLAYER* p)
{
	return p->carConfirmed || p->id == 0;
}

/* JERICHO: is the LOCAL player's car a real choice yet? See mp.h. A joiner that has
 * not picked is driving whatever its own machine happened to have - measured: a client
 * advertises "CHICAGO model 0" while it is still on the title screen. carhacks must not
 * fold that in: it takes a resident slot nobody drives, and the real pick then finds
 * its canonical slot occupied by it. This is the same flag the roster publishes
 * (MP_ROSTER_FLAG_CAR_READY), and the host is always "chosen" - it is the authority for
 * its own car and never sends a pick message. */
int MpLocalCarChosen(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = MpGetPlayer(i);

		if (p != NULL && p->isLocal)
			return MpPlayerCarReady(p);
	}

	return 0;
}

/* Give a car to any player who has none, exactly the way the engine's own
 * player-creation loop does it. A peer that joins a match already in progress
 * gets no car from the engine at all -- and a player with no car is invisible
 * on every screen, has no input applied to anything, and cannot be hit. That is
 * the "no client shows up on the host" report.
 *
 * A slot is CLAIMED by whoever drives it, so the slot for a new car is the
 * smallest CAR_DATA slot no player is already driving -- NOT "how many rows have
 * one". Counting is only right while the cars fill a contiguous [0, n); once a
 * player leaves, its slot is free but the count points at an occupied one, and
 * two players end up in the same car. player[] is MAX_PLAYERS (16) entries, so
 * this is safe past slot 1. */
void MpSpawnLateJoiners(void)
{
	int id, slot, spawned = 0;

	/* THE LEVEL'S OWN START RECORDS MUST EXIST FIRST. The placement below copies
	 * player 0's record and both call sites can arrive while a level is still
	 * coming up -- a city change reloads one -- and PlayerStartInfo[0] is NULL
	 * until the engine builds it during the load (main.c:3485). Dereferencing it
	 * is the 0xC0000005 reported as "changing cities as a client joining the
	 * game": MpSpawnLateJoiners+0x188 in the JERICHO dump.
	 *
	 * Ask again next frame instead of giving up: the caller clears the request
	 * before calling this, so a deferred pass has to re-arm itself. */
	if (PlayerStartInfo[0] == NULL)
	{
		gMp.pendingSpawn = 1;
		return;
	}

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		int rot, k;
		char padid;

		/* Only a player who has CHOSEN a car gets one -- see carConfirmed. And
		 * never one the owner has told us is ON FOOT: the request can also be
		 * armed by the roster, and re-seating a player who is walking about is
		 * what made the stand-in pedestrian flicker. */
		if (p == NULL || p->isLocal || p->carId >= 0 || p->onFoot || !MpPlayerCarReady(p))
			continue;

		/* The smallest CAR_DATA slot no player is already driving. Counting the
		 * rows was only correct while the cars occupied a contiguous [0, n): after
		 * a player LEAVES, its slot is free but the count still points at a slot
		 * that is taken. Slot 0 is the local player's, and the same scan skips it
		 * because the local row drives it. */
		slot = -1;
		for (k = 0; k < MAX_CARS; k++)
		{
			if (MpGetPlayerByCar(k) == NULL)
			{
				slot = k;
				break;
			}
		}

		if (slot < 1 || slot >= MAX_CARS || slot >= MAX_PLAYERS)
		{
			/* past the engine's own player table: InitPlayer below would write
			 * through the end of player[] (MAX_PLAYERS), so a player who has no
			 * slot in it cannot be seated -- say so rather than corrupt. */
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] spawn: no car slot for player %d (found %d, engine holds %d player rows)\n",
					id, slot, MAX_PLAYERS);
			continue;
		}

		/* THE LEVEL'S OWN START, one lane per player -- NOT a copy of player 0's record.
		 *
		 * The engine builds every player's start itself: slot 0 from the mission or
		 * levelstartpos, and slot 1 at +600 in x beside it (main.c:3401-3405), with
		 * ONLY x and z set (main.c:3384 leaves vy = 0) so that placement resolves the
		 * height later (main.c:641-642). A LIVE join has no engine record for this
		 * slot -- the level was built for one player and the start table was filled
		 * then -- so build the same shape here: the local record's start point, offset
		 * one 600-unit lane per player id, the level's own heading, and no y.
		 *
		 * Copying slot 0's whole record (what this did) put every late joiner on the
		 * MISSION START POINT OF PLAYER 0 -- the player who is already in the match --
		 * and the placement below then forced the local car's LIVE y on top of it. A y
		 * from one x/z applied at another is the "late joiners spawn above the host"
		 * report, and the engine has to pull the car down afterwards. */
		PlayerStartInfo[slot] = &ReplayStreams[slot].SourceType;

		memset((char*)PlayerStartInfo[slot], 0, sizeof(STREAM_SOURCE));

		PlayerStartInfo[slot]->type = 1;
		PlayerStartInfo[slot]->controlType = CONTROL_TYPE_PLAYER;
		PlayerStartInfo[slot]->flags = 0;

		{
			int lane = 600 * id;		/* the engine's own step for player 2 */

			PlayerStartInfo[slot]->position.vx = PlayerStartInfo[0]->position.vx + lane;
			PlayerStartInfo[slot]->position.vz = PlayerStartInfo[0]->position.vz;
			/* vy stays 0 -- the engine's convention, not an oversight: placement
			 * resolves the ground under the car. */
			PlayerStartInfo[slot]->rotation = PlayerStartInfo[0]->rotation;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] spawn: player %d -> slot %d at the level's own start %d,%d (+%d lane), no y\n",
					id, slot, PlayerStartInfo[slot]->position.vx,
					PlayerStartInfo[slot]->position.vz, lane);
		}

		/* Same pool-checked per-player vehicle as the level-init spawn, via the
		 * shared helper (see MpSetStartCar). */
		{
			int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
			int cid = MpSetStartCar(slot, p);

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] late joiner: player %d -> slot %d model %d (city %d)\n",
					id, slot, cid, lvl);
		}

		/* The car keeps the height InitPlayer took from the record (0, the engine's own
		 * convention) and the engine's placement resolves the ground under it, exactly
		 * as it does for a car created at level init. There is deliberately NO y
		 * override from the local car: a y from one x/z applied at another is what put
		 * late joiners above the host and then dropped them in. */
		rot = PlayerStartInfo[slot]->rotation;

		padid = (char)-slot;

		InitPlayer(&player[slot], &car_data[slot], CONTROL_TYPE_PLAYER, rot,
			(LONGVECTOR4*)&PlayerStartInfo[slot]->position,
			PlayerStartInfo[slot]->model, PlayerStartInfo[slot]->palette, &padid);

		/* the same field the engine sets for a player car it created itself */
		car_data[slot].ap.needsDenting = 1;

		/* InitPlayer resolves the start record, so place the car after it and
			 * rebuild the matrix (the collision box is built from the matrix) */
		car_data[slot].hd.where.t[0] = PlayerStartInfo[slot]->position.vx;
		car_data[slot].hd.where.t[2] = PlayerStartInfo[slot]->position.vz;
		car_data[slot].hd.direction = rot;

		if (MpDebugOn() && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] spawn: player %d placed at %d,%d,%d (y is the record's, not the local car's)\n",
				id, car_data[slot].hd.where.t[0], car_data[slot].hd.where.t[1],
				car_data[slot].hd.where.t[2]);
		{
			MATRIX m;

			_RotMatrixY(&m, (short)rot);
			memcpy(car_data[slot].hd.where.m, m.m, sizeof(car_data[slot].hd.where.m));
		}

		p->carId = slot;
		spawned++;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d had no car on this machine - given the free slot %d\n", id, slot);
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
			pl->carCity = (e->modelCity == MP_CAR_CITY_SESSION) ? -1 : (int)e->modelCity;
		}
		pl->palette = (int)e->reserved;

		/* The host has a car for them: build OUR copy of it. Without this a third
		 * machine never gives a late joiner a car at all -- its carstate entries
		 * are then dropped (a player with no car here) and the player is INVISIBLE
		 * on every screen but the host's own, while still colliding: the reported
		 * "clients can see themselves and the host, but not other clients".
		 *
		 * Only the host ever asked for the spawn before (MpHandleCar, the pick
		 * message), and a pick goes to the host alone -- so this flag is the only
		 * thing that tells a client "their pick has landed, build them".
		 *
		 * Never from inside the poll: the spawn is deferred to the next frame like
		 * every other spawn this module makes. */
		if (!pl->isLocal && (e->flags & MP_ROSTER_FLAG_CAR_READY))
		{
			pl->carConfirmed = 1;

			/* ...but do NOT ask for a car for someone the owner has told us is
			 * ON FOOT. The host's roster names the car a player OWNS, which is
			 * still true while they are standing next to it, so without this the
			 * request fired on every roster (every 120 frames) and re-spawned a
			 * car for a player who had got out -- the stand-in pedestrian then
			 * appeared and vanished in a loop. The carstate already says it in
			 * one frame; this is the roster's copy of the same fact. */
			if (gMp.running && pl->carId < 0 && !pl->onFoot)
				gMp.pendingSpawn = 1;
		}
	}

	/* EXACTLY ONE ROW IS OURS, and it must be marked after EVERY roster.
	 *
	 * The mark is what the whole local-car path keys on - MpLocalPlayer(), which car is
	 * ours to send state for, and which row mp's remote-car machinery (adopt, rebuild,
	 * release) must leave alone. The adopt pass above can only mark a row it SEES, and a
	 * client's own row can be added before localPlayerId is known (it comes from WELCOME,
	 * which is not guaranteed to precede the first roster), leaving no row marked local at
	 * all. Measured on the rig: a client's log contained NO row with local=1, so on that
	 * machine MpLocalPlayer() answered NULL.
	 *
	 * Re-asserted here, unconditionally, and logged only when the mark actually changes, so
	 * a roster that arrives every couple of seconds is not noisy. */
	if (gMp.localPlayerId >= 0)
	{
		MP_PLAYER* me = NULL;
		int changed = 0;

		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			MP_PLAYER* row = &gMp.players[i];
			int should = (row->active && row->id == gMp.localPlayerId) ? 1 : 0;

			if (row->isLocal != should)
			{
				row->isLocal = should;
				changed = 1;
			}

			if (should)
				me = row;
		}

		/* The roster named nobody as us: adopt our own row, so the local path always has
		 * one to work with (a client whose row never arrived would otherwise have no local
		 * player at all). */
		if (me == NULL)
		{
			me = MpAddPlayer(gMp.localPlayerId, gMp.config.playerName, 1);

			if (me != NULL)
			{
				me->isLocal = 1;
				changed = 1;
			}
		}

		if (changed && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] local row is player %d (row %d, car %d) - from localPlayerId\n",
				gMp.localPlayerId, (me != NULL) ? (int)(me - gMp.players) : -1,
				(me != NULL) ? me->carId : -1);
	}

	/* AND REMOVE THE ONES THE ROSTER NO LONGER NAMES - they have left.
	 *
	 * This was the whole of "cars still dont disappear when the clients disconnect": the
	 * adopt pass above only ever ADDED players, and the one function that puts a remote car
	 * back in the world (MpReleaseRemoteCar) was only reached for a player who went ON FOOT
	 * (MP_CARSTATE_NO_CAR), never for one who left the session. So on every machine that
	 * was not the host, a departed player's car stood there parked with nobody driving it,
	 * for the rest of the match.
	 *
	 * The roster is the right place: it is the host's list of who is in the match, so
	 * "not named" is exactly "gone". MpRemovePlayer does the world-side work (the car goes
	 * back to the world, the slot is recycled, the row is dropped so nothing re-pastes a
	 * transform onto it) - see mp_players.c. */
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* pl = &gMp.players[i];
		int j, named = 0;

		if (!pl->active || pl->isLocal)
			continue;

		for (j = 0; j < n; j++)
		{
			if ((int)r.entries[j].id == pl->id)
			{
				named = 1;
				break;
			}
		}

		if (named)
			continue;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d is no longer in the roster - taking their car out of the world\n",
				pl->id);

		MpRemovePlayer(pl->id);
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

	/* TEST LEVER (MP_TEST_TIMEWEATHER=<time>,<weather>): stands in for the host's
	 * choices on the frontend's screens -- the one step a padless run cannot drive --
	 * so "does the session TAKE the host's picks?" is a line in a headless run rather
	 * than something only a human at the Time of Day screen can check. It writes the
	 * SAME globals that screen writes (wantedTimeOfDay / wantedWeather), not gMp.*,
	 * so it exercises the seeding below instead of bypassing it. Inert unless set. */
	{
		const char* s = getenv("MP_TEST_TIMEWEATHER");

		if (s != NULL)
		{
			int t = -1, w = -1;

			if (sscanf(s, "%d,%d", &t, &w) >= 1)
			{
				if (t >= 0)
					wantedTimeOfDay = t;
				if (w >= 0)
					wantedWeather = w;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] test: frontend time/weather <- %d,%d (MP_TEST_TIMEWEATHER)\n",
						wantedTimeOfDay, wantedWeather);
			}
		}
	}

	/* THE STOCK SCREENS ARE THE HOST'S SETTINGS UI. A host picks its city on the
	 * frontend's city screen and its time of day / condition on the combined Time
	 * of Day screen, so the session takes THOSE values instead of a fixed default:
	 * seed the session from the engine's own picks, then clamp whatever is still
	 * unset. Before this a session forced TIME_DAY / weather 0 ("a session with no
	 * time is a DAY match") and the host's choice on the Time of Day screen was
	 * silently thrown away, while the city came from gMp.city's default (0 = Miami)
	 * because nothing in the current Single Player / Multiplayer flow ever set it --
	 * the lobby that did was replaced by the SP/MP menu. */
	if (gMp.autoSession || MpIsHost())
		gMp.city = GameLevel;
	if (gMp.timeOfDay < 0 && wantedTimeOfDay >= 0)
		gMp.timeOfDay = wantedTimeOfDay;
	if (gMp.weather < 0 && wantedWeather >= 0)
		gMp.weather = wantedWeather;

	/* Whatever is STILL unset gets a valid value BEFORE both the broadcast and the
	 * local launch so host and clients agree, and the mission loader never sees an
	 * out-of-range index */
	if (gMp.city < 0)
		gMp.city = 0;
	if (gMp.timeOfDay < 0)
		gMp.timeOfDay = TIME_DAY;
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

	MpConnMatchStarted();	/* the stage stops lying here: we are IN the match now */

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
	 * carries our palette so the host paints our car the colour we see, and
	 * reserved[2] the CITY that raw model number belongs to (a cross-city pick) --
	 * so the host's row for us names the same (city, model) we do. */
	{
		extern u_char defaultPlayerPalette;

		MP_PLAYER* me = MpLocalPlayer();

		h.car = (uint16_t)((gMp.config.car >= 0) ? gMp.config.car : 0xFFFF);
		h.reserved[0] = (uint8_t)(gMp.config.carIsSlot ? 1 : 0);
		h.reserved[2] = (uint8_t)MpPlayerCarCity(gMp.config.carCity, gMp.config.carIsSlot);
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

/* A client picked a car. The host records it and - for a match already running -
 * builds the vehicle NOW rather than at HELLO time: a joining player has no car
 * until they have chosen one, which is what stops a placeholder car appearing on
 * everybody's screen while they are still in the car select. */
static void MpHandleCar(int connIndex, const unsigned char* p, int len)
{
	MP_CAR c;
	MP_PLAYER* pl;
	int id;

	if (len < (int)sizeof(MP_CAR) || !MpIsHost())
		return;

	memcpy(&c, p, sizeof(c));

	id = MpConnPlayerId(connIndex);
	pl = MpGetPlayer(id);

	if (pl == NULL || !pl->active)
		return;

	pl->car = (c.model == 0xFF) ? -1 : (int)c.model;
	pl->carIsSlot = 0;
	pl->carCity = (c.city < CITY_COUNT) ? (int)c.city : -1;
	pl->carConfirmed = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] player %d picked car model %d (city %d)\n",
			id, pl->car, pl->carCity);

	/* A running match has no car for this player yet - the engine creates player
	 * cars only at level init - so ask for one on the next frame, never from
	 * inside the network poll. */
	if (gMp.running && pl->carId < 0)
		gMp.pendingSpawn = 1;

	/* republish, so every machine draws the car that was actually picked */
	MpHostSendRoster();
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
	 * two builds can speak to each other at all. The BUILD hash is not: it
	 * compares the RELEASE SERIES of JERICHO_BUILD_VERSION, not the string
	 * itself, because the string is `git describe --tags --always --dirty` and
	 * the same release reads differently depending on who built it how
	 * ("0.9.0", "0.9.0-dirty", "0.9.0-3-gabc1234"). Comparing the raw string
	 * refused every pair that was not identical in provenance, which is every
	 * cross-platform pair and every pair where one side built locally. It is
	 * still behind the lobby's opt-in "Strict Version" toggle, default off. */
	if (h.protoVersion != (uint16_t)MP_PROTO_VERSION ||
	    h.sdkVersion != (uint16_t)JERICHO_SDK_VERSION ||
	    (gMp.config.strictVersion && h.gameBuild != MpBuildHash()))
	{
		if (gMpCtx)
		{
			char series[64];

			/* Log the SERIES and not just the two hashes: a player comparing
			 * builds needs to see which builds disagreed. */
			MpBuildSeries(series, sizeof(series));
			gMpCtx->jer_log(gMpCtx, "[mp] reject: version mismatch (proto %d/%d, sdk %d/%d, strict=%d, host series %s build %d, peer build %d)\n",
				h.protoVersion, MP_PROTO_VERSION, h.sdkVersion, JERICHO_SDK_VERSION,
				gMp.config.strictVersion, series, MpBuildHash(), h.gameBuild);
		}

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
			pl->car = (h.car == 0xFFFF) ? -1 : (int)h.car;
			pl->carIsSlot = h.reserved[0] ? 1 : 0;
			pl->carCity = (h.reserved[2] < CITY_COUNT) ? (int)h.reserved[2] : -1;
			pl->palette = (int)h.reserved[1];

			if (gMpCtx)
				gMpCtx->jer_log(gMpCtx, "[mp] hello from player %d: vehicle %d\n", id, pl->car);
		}
	}

	/* NO car is built here any more. A HELLO arrives when the client CONNECTS - before
	 * it has shown the player the car select - so a car spawned now would be a
	 * placeholder nobody chose, sitting on every other machine with the level's
	 * default model until the player got round to picking. MpHandleCar builds it
	 * when it hears which car they actually chose. */

	MpSendWelcome(connIndex, id, matched);

	MpNotifyf("%s joined", h.playerName);

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] player %d '%s' joined (%d/%d)%s\n",
			id, h.playerName, gMp.playerCount, MP_MAX_PLAYERS,
			matched ? "" : " [mods differ]");
}

/* Chat: the host is the echo point, so a line is announced
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
static void MpHandleLeave(int connIndex, const unsigned char* p, int len)
{
	(void)p;
	(void)len;

	if (gMp.role == MP_ROLE_HOST)
	{
		/* A client told us it is going. Close THAT connection right now, and say
		 * why. This used to just return -- 'a client asked to leave; its row is
		 * dropped anyway' -- so the socket sat there until the 30 s liveness check
		 * reaped it and the log reported the drop as "timeout". That single wrong
		 * word sent a whole investigation chasing a silence that never happened,
		 * and it left a zombie socket (and a later RST) for half a minute. */
		MpConnEvent("peer sent LEAVE", connIndex, "the client is leaving");
		MpConnDrop(connIndex, "peer sent LEAVE");
		return;
	}

	jer_error("The host ended the match");

	MpConnEvent("host ended the match", -1, "the session is over");

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
			me->carCity = gMp.config.carCity;
			me->palette = defaultPlayerPalette;
		}

		if (me != NULL)
		{
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

		/* Only a player who has CHOSEN a car gets one built here. A peer whose pick has
		 * not arrived is left with NO car on purpose: their vehicle must not appear on
		 * every screen before they have picked it in the car select. MpHandleCar builds
		 * it the moment the pick lands. */
		if (p == NULL || p->isLocal || !MpPlayerCarReady(p))
			continue;

		PlayerStartInfo[slot] = &ReplayStreams[slot].SourceType;
		memcpy((u_char*)PlayerStartInfo[slot], (u_char*)PlayerStartInfo[0], sizeof(STREAM_SOURCE));

		PlayerStartInfo[slot]->type = 1;
		PlayerStartInfo[slot]->controlType = CONTROL_TYPE_PLAYER;
		PlayerStartInfo[slot]->flags = 0;

		/* POOL-CHECKED vehicle, via the shared helper (see MpSetStartCar). Only remote
		 * cars reach here -- the local player is skipped above. */
		{
			int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
			int cid = MpSetStartCar(slot, p);

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
					i, slot, cid, lvl, p->car);

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

/* Declared here because MpLockstepFrame runs before its body below (the test
 * levers are all one tick, and this one has to sit with the others). NOT static:
 * mp.c's frame hook calls it as well, for the single-player case where the lockstep
 * tick never runs (see mp.h). */
void MpTestPauseCarTick(void);
static void MpTestRestartTick(void);

int MpInputForPlayer(int id)
{
	if (id < 0 || id >= MP_MAX_PLAYERS)
		return 0;

	return gMp.padForPlayer[id];
}


/* forward: the owner's own-car replication (defined later) */
static void MpSendOwnCarState(void);
static void MpSendOwnPedState(void);
static void MpSendTrafficState(void);
static void MpTrafficMirrorsClear(void);
static void MpTrafficMirrorAgeTick(void);
static int  MpSlotInOurBand(int slot);
static int  MpTrafficSlotOwnerRank(int slot);
static void MpTrafficContactPush(MP_PLAYER* me, int slot);
static long long MpDistSq64(int dx, int dz);
static void MpTestTrafficHitTick(void);
static void MpDriveRemotePed(MP_PLAYER* p);
static void MpKeepOurCarsFromTrafficAi(void);
static void MpSendColors(int whole);
static void MpTestCarChangeTick(void);
static void MpTestCityChangeTick(void);
static void MpTestCarCycleTick(void);
static void MpTestLeaveTick(void);
static void MpTestOnFootTick(void);
static void MpTestFallOffTick(void);


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
#define MP_FIXEDH		4096	/* the velocity fix-point scale (1 world unit/frame << 12) */
#define MP_HIT_RADIUS		900	/* world units; car body scale (the probe FALLBACK only) */
#define MP_HIT_MIN_CLOSING	4	/* WHOLE units/frame; don't fire on mere adjacency */
#define MP_HIT_PUSH		60	/* per-cent of the closing speed given up */
#define MP_HIT_MAX_PUSH		20	/* WHOLE units/frame cap */
#define MP_HIT_ARM_FRAMES	120	/* let the spawn/drop settle before contacts count */
#define MP_HIT_COOLDOWN_FRAMES	15
#define MP_HIT_SELF_FRAMES	4	/* our own engine reported this contact this recently */
#define MP_HIT_PROBE_QUIET	90	/* ... and the probe stands down this long after one */

/* Contacts arm afresh in every match (see gMpHitArmFrame at the top of this file
 * for why the arm frame is session state and not a function local). */
static int MpHitArmed(void)
{
	if (gMpHitArmFrame == 0)
		gMpHitArmFrame = gMp.frame + MP_HIT_ARM_FRAMES;

	return (gMp.frame >= gMpHitArmFrame);
}

/* The contact normal US -> THEM in the velocity fix-point (MP_FIXEDH = 1.0), and
 * the closing speed in WHOLE units per frame.
 *
 * THE UNITS ARE THE WHOLE POINT. A velocity is fixed-point, so the dot product of
 * a velocity difference with a fixed-point normal carries MP_FIXEDH TWICE; it is
 * reduced here, once, rather than left 4096x too large. The thresholds and the cap
 * above are documented in whole units/frame and are now actually in them -- the
 * previous code compared and scaled FIXEDH values against those numbers, which made
 * every hand-off 1/4096 of the intended push: a contact that moved nothing, which
 * is what "the cars drive through each other" was. */
static long MpContactNormal(CAR_DATA* mine, CAR_DATA* other, long* px, long* py, long* pz)
{
	/* 64-bit: a world span squared overflows 32, and two cars a region apart is
	 * an ordinary thing to ask about. */
	long long dx = (long long)other->hd.where.t[0] - (long long)mine->hd.where.t[0];
	long long dy = (long long)other->hd.where.t[1] - (long long)mine->hd.where.t[1];
	long long dz = (long long)other->hd.where.t[2] - (long long)mine->hd.where.t[2];
	long long d2 = dx * dx + dy * dy + dz * dz;
	long d;

	*px = MP_FIXEDH;
	*py = 0;
	*pz = 0;

	if (d2 <= 0)
		return 0;

	d = (long)sqrt((double)d2);

	if (d == 0)
		return 0;

	*px = (long)(dx * MP_FIXEDH / d);
	*py = (long)(dy * MP_FIXEDH / d);
	*pz = (long)(dz * MP_FIXEDH / d);

	return (long)(((long long)(mine->st.n.linearVelocity[0] - other->st.n.linearVelocity[0]) * *px
		+ (long long)(mine->st.n.linearVelocity[1] - other->st.n.linearVelocity[1]) * *py
		+ (long long)(mine->st.n.linearVelocity[2] - other->st.n.linearVelocity[2]) * *pz)
		/ MP_FIXEDH / MP_FIXEDH);
}

/* Report a contact between OUR car and `pl`'s: give up our own share of the
 * closing speed here, and ask THEIR owner to give up theirs. Returns 1 when a
 * contact was reported (the caller's cooldown mark is set here, so both triggers
 * share one rate limit).
 *
 * `localHalf` decides whether OUR car is pushed as well. The engine-contact path
 * passes 0: our own engine has just resolved this contact, so its response is
 * already ours and a second impulse would double the hit. The probe fallback
 * passes 1, because by definition the engine did not see that overlap. */
static int MpContactPush(MP_PLAYER* me, MP_PLAYER* pl, int localHalf, const char* why)
{
	CAR_DATA* mine = &car_data[me->carId];
	CAR_DATA* other = &car_data[pl->carId];
	long px, py, pz, closing, push, ix, iy, iz;
	MP_HIT h;

	closing = MpContactNormal(mine, other, &px, &py, &pz);

	if (closing < MP_HIT_MIN_CLOSING)
		return 0;

	push = closing * MP_HIT_PUSH / 100;

	if (push > MP_HIT_MAX_PUSH)
		push = MP_HIT_MAX_PUSH;

	/* The impulse is a VELOCITY delta, and px is ALREADY normal * MP_FIXEDH, so a
	 * whole-unit push along it is one multiply: (n << 12) * push == n * push << 12.
	 * Dividing here as well (what the old code did) is the 4096x that made the
	 * hand-off inert. */
	ix = px * push;
	iy = py * push;
	iz = pz * push;

	if (localHalf)
	{
		mine->st.n.linearVelocity[0] -= (int)ix;
		mine->st.n.linearVelocity[1] -= (int)iy;
		mine->st.n.linearVelocity[2] -= (int)iz;
	}

	memset(&h, 0, sizeof(h));
	h.targetId = (uint8_t)pl->id;
	h.impulse[0] = (int)ix;
	h.impulse[1] = (int)iy;
	h.impulse[2] = (int)iz;

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
		gMpCtx->jer_log(gMpCtx,
			"[mp] hit: we bumped player %d (%s: closing %ld, giving up %ld units/frame)\n",
			pl->id, why, closing, push);

	return 1;
}

/* JER_EVENT_COLLISION -- the engine has just resolved a car-to-car contact
 * (handling.c fires it for the pair it resolved, BEFORE either car's impulse is
 * applied, so the velocities read here are still the pre-impact ones: the true
 * closing speed).
 *
 * This is the primary trigger for the hand-off, and it is the fix for "the
 * collision is not always registered on the remote player": probing for proximity
 * on a later frame asked the question after our own engine had already absorbed
 * the closing velocity, so it read ~0 and nothing was sent for exactly the hits
 * that mattered. */
int MpOnCarContact(void* ud, void* args)
{
	JER_ARGS_COLLISION* a = (JER_ARGS_COLLISION*)args;
	MP_PLAYER* me = MpLocalPlayer();
	MP_PLAYER* them;
	int mineId, otherId;

	(void)ud;

	/* car1 == NULL is a car-vs-WORLD hit: not ours to relay. */
	if (a == NULL || a->car0 == NULL || a->car1 == NULL)
		return JER_RESULT_CONTINUE;

	if (me == NULL || me->carId < 0 || !gMp.running)
		return JER_RESULT_CONTINUE;

	mineId = CAR_INDEX((CAR_DATA*)a->car0);
	otherId = CAR_INDEX((CAR_DATA*)a->car1);

	/* TRAFFIC contacts. Two things, both about cars that are not a player's own:
	 *  - remember a contact involving OUR traffic car, so the owner's engine
	 *    response and a peer's push do not both move it (see MpHandleHit);
	 *  - a contact between OUR car and a PEER's traffic car (a mirror on this
	 *    machine) must reach the slot's OWNER, where the car really is. */
	{
		int ourTraffic = -1;
		int localHit = -1;

		if (MpSlotInOurBand(mineId))
			ourTraffic = mineId;
		if (MpSlotInOurBand(otherId))
			ourTraffic = otherId;

		if (ourTraffic >= 0)
			gMpTrafficSelfContact[ourTraffic] = gMp.frame;

		if (mineId == me->carId && otherId >= 0 && otherId < MAX_CARS && gMpPeerTraffic[otherId])
			localHit = otherId;
		else if (otherId == me->carId && mineId >= 0 && mineId < MAX_CARS && gMpPeerTraffic[mineId])
			localHit = mineId;

		if (localHit >= 0)
		{
			MpTrafficContactPush(me, localHit);
			return JER_RESULT_CONTINUE;
		}
	}

	if (mineId == me->carId)
		them = MpGetPlayerByCar(otherId);
	else if (otherId == me->carId)
		them = MpGetPlayerByCar(mineId);
	else
		return JER_RESULT_CONTINUE;	/* two other cars; not our business */

	if (them == NULL || !them->active || them->isLocal || them->carId < 0)
		return JER_RESULT_CONTINUE;

	/* Remember it whether or not we act, so the RECEIVING half knows the engine
	 * has this contact in hand and does not add the peer's push on top. */
	gMpSelfContactFrame[them->id] = gMp.frame;

	if (!MpHitArmed())
		return JER_RESULT_CONTINUE;

	if (gMp.frame - them->lastHitFrame < MP_HIT_COOLDOWN_FRAMES)
		return JER_RESULT_CONTINUE;

	MpContactPush(me, them, 0, "engine contact");

	return JER_RESULT_CONTINUE;
}

/* The FALLBACK probe, on the sim frame. The engine's own contact hook above is
 * what reports a hit; this only catches an overlap the engine never resolved --
 * a snapshot teleport, or a car that arrived while we were loading. It applies the
 * LOCAL half as well, precisely because the engine did not move us in that case. */
static void MpHitFrame(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* mine;
	int i;

	if (me == NULL || me->carId < 0)
		return;

	if (!MpHitArmed())
		return;

	mine = &car_data[me->carId];

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* pl = &gMp.players[i];
		CAR_DATA* other;
		long long dx, dy, dz, d2;

		if (!pl->active || pl->isLocal || pl->id == me->id || pl->carId < 0)
			continue;
		if (gMp.frame - pl->lastHitFrame < MP_HIT_COOLDOWN_FRAMES)
			continue;

		/* STAND DOWN while the engine is reporting contacts with this peer. The
		 * probe is the fallback for an overlap the engine never saw; left to run
		 * whenever it likes it takes the shared cooldown first (it is a distance
		 * test, so it fires on a near miss the engine's collision boxes do not
		 * touch) and masks the accurate trigger for the next 15 frames. Measured
		 * before this: 3..5 engine contacts against ~68 probe hits in one pair
		 * run, i.e. the primary trigger was starved by its own fallback. */
		if (gMpSelfContactFrame[pl->id] != 0 &&
			(gMp.frame - gMpSelfContactFrame[pl->id]) < MP_HIT_PROBE_QUIET)
			continue;

		other = &car_data[pl->carId];

		dx = (long long)other->hd.where.t[0] - (long long)mine->hd.where.t[0];
		dy = (long long)other->hd.where.t[1] - (long long)mine->hd.where.t[1];
		dz = (long long)other->hd.where.t[2] - (long long)mine->hd.where.t[2];
		d2 = dx * dx + dy * dy + dz * dz;

		if (d2 == 0 || d2 > (long long)MP_HIT_RADIUS * MP_HIT_RADIUS)
			continue;

		MpContactPush(me, pl, 1, "probe");
	}
}

static void MpHandleHit(int connIndex, const unsigned char* p, int len)
{
	MP_HIT h;
	MP_PLAYER* me = MpLocalPlayer();
	int from;

	if (len < (int)sizeof(h))
		return;

	memcpy(&h, p, sizeof(h));

	from = MpConnPlayerId(connIndex);

	/* A TRAFFIC contact: targetId is a car_data slot, not a player id. Only the
	 * OWNER of the slot applies it (its own CIV_AI/PURSUER_AI car) -- on anyone else
	 * it is a mirror rewritten from the wire, so a push there would be lost. Our own
	 * engine may already have resolved the same contact against the peer's mirror
	 * (marked in MpOnCarContact); within MP_HIT_SELF_FRAMES the push is that
	 * duplicate and is dropped. */
	if (h.flags & MP_HIT_F_TRAFFIC)
	{
		int slot = (int)h.targetId;

		if (slot >= 0 && slot < MAX_CARS)
		{
			CAR_DATA* cp = &car_data[slot];

			if (cp->controlType == CONTROL_TYPE_CIV_AI || cp->controlType == CONTROL_TYPE_PURSUER_AI)
			{
				if (gMpTrafficSelfContact[slot] != 0 &&
				    (gMp.frame - gMpTrafficSelfContact[slot]) <= MP_HIT_SELF_FRAMES)
				{
					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] hit: player %d bumped our traffic slot %d (our engine has it, kept)\n",
							from, slot);
				}
				else
				{
					cp->st.n.linearVelocity[0] += h.impulse[0];
					cp->st.n.linearVelocity[1] += h.impulse[1];
					cp->st.n.linearVelocity[2] += h.impulse[2];

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] hit: player %d bumped our traffic slot %d (push %ld,%ld,%ld)\n",
							from, slot, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
				}
			}
		}

		/* The host relays a traffic hit aimed at another client's band. */
		if (MpIsHost())
		{
			int owner = MpTrafficSlotOwnerRank((int)h.targetId);
			int ci = (owner >= 0) ? MpConnFindByPlayer(owner) : -1;

			if (ci >= 0 && ci != connIndex)
				MpSendConn(ci, MP_TAG_HIT, 0, p, len);
		}

		return;
	}

	if (me != NULL && h.targetId == me->id && me->carId >= 0 && me->carId < MAX_CARS)
	{
		CAR_DATA* cp = &car_data[me->carId];

		/* ONCE per contact. Our own engine resolves the same collision (both
		 * machines simulate both cars), and its rigid-body response is already in
		 * our velocity -- so adding the peer's push as well lands one collision
		 * twice. The engine's contact hook marks the frame it saw one; within
		 * MP_HIT_SELF_FRAMES of that, this push is the duplicate and is dropped
		 * (the contact is still logged, so a run can see both halves arriving).
		 *
		 * Outside that window nothing local moved us, which is the case the
		 * hand-off exists for: apply it. */
		if (from >= 0 && from < MP_MAX_PLAYERS && gMpSelfContactFrame[from] != 0 &&
			(gMp.frame - gMpSelfContactFrame[from]) <= MP_HIT_SELF_FRAMES)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] hit: player %d bumped us (push %ld,%ld,%ld; our engine has it, kept)\n",
					from, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
			return;
		}

		cp->st.n.linearVelocity[0] += h.impulse[0];
		cp->st.n.linearVelocity[1] += h.impulse[1];
		cp->st.n.linearVelocity[2] += h.impulse[2];

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] hit: player %d bumped us (push %ld,%ld,%ld)\n",
				from, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
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
static const char* gHeartbeatStr;
static const char* gTestLeaveStr;
static const char* gTestOnFootStr;
static const char* gTestCarChangeStr;
static const char* gTestCityChangeStr;
static const char* gTestCarCycleStr;
static const char* gTestFallOffStr;
static const char* gTestChatKeyStr;
static const char* gTestCarSelectStr;
static const char* gTestFrontendJoinStr;

static void MpResolveTestLevers(void)
{
	static int resolved = 0;

	if (resolved)
		return;

	resolved = 1;

	gHeartbeatStr = getenv("MP_HEARTBEAT");
	gTestLeaveStr = getenv("MP_TEST_LEAVE");
	gTestOnFootStr = getenv("MP_TEST_ONFOOT");
	gTestCarChangeStr = getenv("MP_TEST_CARCHANGE");
	gTestCityChangeStr = getenv("MP_TEST_CITYCHANGE");
	gTestCarCycleStr = getenv("MP_TEST_CARCYCLE");
	gTestFallOffStr = getenv("MP_TEST_FALLOFF");
	gTestChatKeyStr = getenv("MP_TEST_CHATKEY");
	gTestCarSelectStr = getenv("MP_TEST_CARSELECT");
	gTestFrontendJoinStr = getenv("MP_TEST_FRONTEND_JOIN");
}

/* MP_TEST_FRONTEND_JOIN: is a joining machine meant to KEEP the menus?
 *
 * Every other client lever (-join, MP_AUTOSTART=join) sets autoSession, i.e.
 * "no menus: launch as soon as we are in" -- so a client never opens the vehicle
 * select and a headless run could not answer "does a CLIENT reach the car screen
 * and its roster?". With this set the join keeps autoSession off, and the client
 * takes the route a human at the machine takes: WELCOME -> MpUiOpenCarSelect ->
 * screen 14 -> the menu -> Ride (which hands the launch back through
 * JER_EVENT_MP_FRONTEND).
 *
 * Set = any non-empty value other than "0", like the other levers. */
const char* MpTestFrontendJoin(void)
{
	MpResolveTestLevers();
	return gTestFrontendJoinStr;
}

/* MP_TEST_CHATKEY is read by the frame hook in mp.c, so it is exposed rather
 * than duplicated: one resolution point for every test lever. */
const char* MpTestChatKey(void)
{
	MpResolveTestLevers();
	return gTestChatKeyStr;
}

/* MP_TEST_CARSELECT, read once: see the launch lever in MpLaunchLocal. */
const char* MpTestCarSelect(void)
{
	MpResolveTestLevers();
	return gTestCarSelectStr;
}

static void MpHeartbeatTick(void)
{
	static unsigned long lastMs;
	const char* s = gHeartbeatStr;
	unsigned long now;
	int secs;

	if (s == NULL || gMpCtx == NULL)
		return;

	secs = atoi(s);

	if (secs <= 0)
		return;

	now = MpNowMs();

	if (lastMs != 0 && (now - lastMs) < (unsigned long)(secs * 1000))
		return;

	lastMs = now;

	gMpCtx->jer_log(gMpCtx, "[mp] heartbeat: frame %lu running %d role %d\n",
		gMp.frame, gMp.running, gMp.role);
}

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
 * car_data[0..MP_MAX_PLAYERS-1] stays with the players (player id == car slot;
 * see MpOnNetSpawn), so the bands live above them. Bands are sized by how many
 * machines share the match: two split the pool in half, four in quarters, and the
 * last machine takes the remainder so no slot is orphaned.
 *
 * RE-ASSERTED EVERY FRAME, not set once: a level load ClearMem()s the engine's
 * reservedSlots (mission.c:301) out from under us, and the engine's own cutscene
 * recorder reserves from the low end -- so we manage ONLY the traffic band, and
 * only the bits WE set, never the engine's. */
#define MP_TRAFFIC_SLOT_FIRST	MP_MAX_PLAYERS				/* 8: below this is the players' */
#define MP_TRAFFIC_SLOT_COUNT	(MAX_CARS - MP_TRAFFIC_SLOT_FIRST)	/* 12 traffic slots */

static unsigned char gMpBandReserved[MAX_CARS];	/* slots WE turned on, so we clear only ours */
static int gMpBandFirst = -1;			/* this machine's band, inclusive; -1 = none */
static int gMpBandLast = -1;

/* How many machines share the traffic pool. OUR OWN rank must always have a band,
 * so the count is never below rank+1: a client that has just joined and not yet
 * seen the roster still gets a private band rather than clamping onto the host's. */
static int MpTrafficMachineCount(void)
{
	int n = gMp.playerCount;

	if (n < gMp.localPlayerId + 1)
		n = gMp.localPlayerId + 1;

	if (n < 1)
		n = 1;

	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	return n;
}

/* The inclusive band for machine `rank` of `nMachines`. The last machine takes
 * the remainder so the whole pool is covered even when it does not divide. */
static void MpTrafficBandFor(int rank, int nMachines, int* first, int* last)
{
	int per = MP_TRAFFIC_SLOT_COUNT / nMachines;
	int f, l;

	if (per < 1)
		per = 1;

	f = MP_TRAFFIC_SLOT_FIRST + rank * per;
	l = f + per - 1;

	if (rank == nMachines - 1)
		l = MAX_CARS - 1;

	if (f > MAX_CARS - 1)
		f = MAX_CARS - 1;

	if (l > MAX_CARS - 1)
		l = MAX_CARS - 1;

	if (l < f)
		l = f;

	*first = f;
	*last = l;
}

/* Release every slot WE reserved. Called when the session ends, so the engine's
 * traffic is untouched once we are not replicating it. */
void MpTrafficBandClear(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (gMpBandReserved[i])
		{
			reservedSlots[i] = 0;
			gMpBandReserved[i] = 0;
		}
	}

	gMpBandFirst = -1;
	gMpBandLast = -1;
}

/* This machine's traffic band, or 0 when no session is live. */
int MpTrafficBand(int* first, int* last)
{
	if (gMpBandFirst < 0)
		return 0;

	if (first != NULL)
		*first = gMpBandFirst;

	if (last != NULL)
		*last = gMpBandLast;

	return 1;
}

/* Is `slot` a civilian a player could actually collide with -- ours or a mirror? */
static int MpSlotInOurBand(int slot)
{
	int f, l;

	return slot >= 0 && slot < MAX_CARS && MpTrafficBand(&f, &l) && slot >= f && slot <= l;
}

/* The machine whose band owns `slot`. Bands are cut by rank and our rank IS the
 * player id, so this is the id of the peer to send a traffic contact to. */
static int MpTrafficSlotOwnerRank(int slot)
{
	int n = MpTrafficMachineCount();
	int r;

	for (r = 0; r < n; r++)
	{
		int f, l;

		MpTrafficBandFor(r, n, &f, &l);

		if (slot >= f && slot <= l)
			return r;
	}

	return -1;
}

/* Report a contact between OUR car and a PEER's traffic car (a mirror here). The
 * impulse goes to the slot's OWNER: on this machine the car is a mirror rewritten
 * from the peer's stream every frame, so a push applied here is simply discarded.
 * One rate limit per slot, shared with the engine-contact trigger, like the players. */
static void MpTrafficContactPush(MP_PLAYER* me, int slot)
{
	CAR_DATA* mine = &car_data[me->carId];
	CAR_DATA* other = &car_data[slot];
	long px, py, pz, closing, push;
	MP_HIT h;
	int owner;

	if (gMp.frame - gMpTrafficHitFrame[slot] < MP_HIT_COOLDOWN_FRAMES)
		return;

	closing = MpContactNormal(mine, other, &px, &py, &pz);

	if (closing < MP_HIT_MIN_CLOSING)
		return;

	push = closing * MP_HIT_PUSH / 100;

	if (push > MP_HIT_MAX_PUSH)
		push = MP_HIT_MAX_PUSH;

	owner = MpTrafficSlotOwnerRank(slot);

	if (owner < 0)
		return;

	gMpTrafficHitFrame[slot] = gMp.frame;

	memset(&h, 0, sizeof(h));
	h.flags = MP_HIT_F_TRAFFIC;
	h.targetId = (uint8_t)slot;
	h.impulse[0] = (int)(px * push);
	h.impulse[1] = (int)(py * push);
	h.impulse[2] = (int)(pz * push);

	if (MpIsHost())
	{
		int ci = MpConnFindByPlayer(owner);

		if (ci >= 0)
			MpSendConn(ci, MP_TAG_HIT, 0, &h, sizeof(h));
	}
	else
		MpSendToHost(MP_TAG_HIT, 0, &h, sizeof(h));

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] hit: we bumped a player's traffic car in slot %d (push %ld,%ld,%ld) -> owner %d\n",
			slot, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2], owner);
}

/* MP_TEST_TRAFFIC_HIT=<secs> -- every N seconds, send ONE synthetic traffic-contact
 * push for the nearest PEER mirror, so the traffic-hit wire path can be exercised
 * without a physical collision (the bots avoid traffic, so a pair run never makes
 * one). The impulse is fixed and deliberately large so the owner's move is
 * unambiguous. A test lever; inert unless set. */
static void MpTestTrafficHitTick(void)
{
	static int secs = -2;
	static unsigned nextFrame = 0;
	MP_PLAYER* me = MpLocalPlayer();
	int i, best = -1, owner;
	long long bestD = -1;
	MP_HIT h;

	if (secs == -2)
	{
		const char* e = getenv("MP_TEST_TRAFFIC_HIT");
		secs = (e != NULL) ? atoi(e) : -1;
	}

	if (secs <= 0 || !gMp.running || me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	if (nextFrame == 0)
		nextFrame = gMp.frame + 30;	/* let the match settle first */

	if (gMp.frame < nextFrame)
		return;

	nextFrame = gMp.frame + (unsigned)secs * 30;

	for (i = 0; i < MAX_CARS; i++)
	{
		long long d;

		if (!gMpPeerTraffic[i])
			continue;

		d = MpDistSq64(car_data[i].hd.where.t[0] - car_data[me->carId].hd.where.t[0],
			       car_data[i].hd.where.t[2] - car_data[me->carId].hd.where.t[2]);

		if (bestD < 0 || d < bestD)
		{
			bestD = d;
			best = i;
		}
	}

	if (best < 0)
		return;

	owner = MpTrafficSlotOwnerRank(best);

	if (owner < 0)
		return;

	memset(&h, 0, sizeof(h));
	h.flags = MP_HIT_F_TRAFFIC;
	h.targetId = (uint8_t)best;
	h.impulse[2] = 3000;	/* a shove along +z, big enough to see */

	if (MpIsHost())
	{
		int ci = MpConnFindByPlayer(owner);

		if (ci >= 0)
			MpSendConn(ci, MP_TAG_HIT, 0, &h, sizeof(h));
	}
	else
		MpSendToHost(MP_TAG_HIT, 0, &h, sizeof(h));

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] TEST traffic hit: slot %d -> owner %d (impulse %ld,%ld,%ld)\n",
			best, owner, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
}

/* Once per sim frame, before anything spawns. Sets the ONE bit of state the two
 * machines have to agree on before traffic can be replicated at all: which
 * car_data slots this machine may put a civilian in. */
static void MpTrafficBandRefresh(void)
{
	int n, rank, f, l, i, changed;

	if (!gMp.running)
	{
		MpTrafficBandClear();
		return;
	}

	n = MpTrafficMachineCount();
	rank = (gMp.localPlayerId > 0) ? gMp.localPlayerId : 0;

	MpTrafficBandFor(rank, n, &f, &l);

	changed = (f != gMpBandFirst || l != gMpBandLast);

	/* Everything OUTSIDE our band is kept off our spawner: the player reserve
	 * [0..7) because a civilian there would squat on a slot a player (or a late
	 * joiner) needs, and every other machine's band because its cars get mirrored
	 * into those slots and OUR spawner must never race the peer for one.
	 *
	 * reservedSlots is the array PingInCivCar's free-slot scan consults
	 * (civ_ai.c:2058; the two collider indices are already outside that scan's
	 * bound), so this is the one bit of state that keeps the two spawners apart.
	 * We set only the bits we own and clear only those, so a level load dropping
	 * them is simply re-asserted on the next frame. */
	for (i = 0; i < MAX_CARS; i++)
	{
		int want = (i < f || i > l);

		if (want)
		{
			if (!gMpBandReserved[i])
			{
				reservedSlots[i] = 1;
				gMpBandReserved[i] = 1;
			}
		}
		else if (gMpBandReserved[i])
		{
			reservedSlots[i] = 0;
			gMpBandReserved[i] = 0;
		}
	}

	gMpBandFirst = f;
	gMpBandLast = l;

	if (changed && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] traffic band: machine %d of %d owns car_data slots %d..%d\n",
			rank, n, f, l);
}

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
#define MP_TRAFFIC_CLAIM_DEADBAND	2000	/* world units: closer than this, the lower player id wins */

static int gMpClaimCulled;		/* cars pinged out by the claim this reporting window */

/* A squared world distance must be 64-BIT: coordinates reach six figures, so the
 * 32-bit square wraps negative and a far car reads as the nearest. */
static long long MpDistSq64(int dx, int dz)
{
	long long a = dx, b = dz;

	return a * a + b * b;
}

/* The player nearest to (x,z), the lowest player id winning a tie, or -1 when no
 * player has a car to measure from. *outSq receives that player's squared distance. */
static int MpTrafficClaimOwner(int x, int z, long long* outSq)
{
	long long d[MP_MAX_PLAYERS];
	int id[MP_MAX_PLAYERS];
	int n = 0, pi, k, best = -1;
	long long dMin = 0;
	long long marginSq = (long long)MP_TRAFFIC_CLAIM_DEADBAND * MP_TRAFFIC_CLAIM_DEADBAND;

	for (pi = 0; pi < MP_MAX_PLAYERS; pi++)
	{
		MP_PLAYER* p = &gMp.players[pi];

		if (!p->active || p->carId < 0 || p->carId >= MAX_CARS)
			continue;

		d[n] = MpDistSq64(x - car_data[p->carId].hd.where.t[0],
				  z - car_data[p->carId].hd.where.t[2]);
		id[n] = p->id;
		n++;
	}

	if (n == 0)
		return -1;

	dMin = d[0];

	for (k = 1; k < n; k++)
	{
		if (d[k] < dMin)
			dMin = d[k];
	}

	/* Within the DEADBAND of the nearest, the LOWEST player id owns the car -- not an
	 * exact tie, which real positions never produce. That gives both machines the
	 * same answer AND a stable one: a car drifting a few units either way does not
	 * change who owns it, which is what stops the boundary flapping (and the
	 * spawn/cull churn it caused when the test was exact equality). */
	for (k = 0; k < n; k++)
	{
		if (d[k] <= dMin + marginSq && (best < 0 || id[k] < best))
			best = id[k];
	}

	if (outSq != NULL)
		*outSq = dMin;

	return best;
}

/* Is one of the PEER's mirrors already near (x,z)? The claim must only remove OUR
 * copy of a car the peer actually has HERE -- otherwise culling would leave a gap:
 * the machine that owns this patch has no car to show yet, so the car simply
 * vanishes until its owner's next spawn. Requiring the peer's car to be on our
 * screen first is what makes the handoff gap-free -- we stay until theirs arrives. */
static int MpPeerMirrorNear(int x, int z)
{
	long long best = -1;
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		long long d;

		if (!gMpPeerTraffic[i])
			continue;

		d = MpDistSq64(x - car_data[i].hd.where.t[0], z - car_data[i].hd.where.t[2]);

		if (best < 0 || d < best)
			best = d;
	}

	return best >= 0 && best <= (long long)MP_TRAFFIC_CLAIM_DEADBAND * MP_TRAFFIC_CLAIM_DEADBAND;
}

/* Once per sim frame: give up any civilian in OUR band that a peer's player owns.
 *
 * Note the direction of the rule: every machine KEEPS its nearer half, so both
 * machines still have traffic -- an earlier version made the higher-id machine stop
 * spawning entirely when a peer was close, which (before state is mirrored between
 * the machines) simply left it with no traffic at all. */
static void MpTrafficClaimPass(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	int first, last, i, local = gMp.localPlayerId;
	int culled = 0, census = 0;
	long long nearSq = -1;

	if (!MpTrafficBand(&first, &last))
		return;

	/* A player on foot owns no car, so the claim has nothing to measure from and
	 * every car would look like a peer's -- leave the traffic alone instead. */
	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	for (i = first; i <= last; i++)
	{
		CAR_DATA* cp = &car_data[i];
		int owner;

		if (cp->controlType != CONTROL_TYPE_CIV_AI)
			continue;

		census++;
		{
			long long dSq = MpDistSq64(cp->hd.where.t[0] - car_data[me->carId].hd.where.t[0],
						  cp->hd.where.t[2] - car_data[me->carId].hd.where.t[2]);

			if (nearSq < 0 || dSq < nearSq)
				nearSq = dSq;
		}

		owner = MpTrafficClaimOwner(cp->hd.where.t[0], cp->hd.where.t[2], NULL);

		/* Hand off ONLY when the peer's copy is already on our screen. Culling our
		 * car while the owner has not spawned theirs yet is the gap the census used to
		 * show as a car blinking out and a different one taking its place. */
		if (owner >= 0 && owner != local &&
		    MpPeerMirrorNear(cp->hd.where.t[0], cp->hd.where.t[2]))
		{
			PingOutCar(cp);
			culled++;
		}
	}

	gMpClaimCulled += culled;

	/* A CENSUS, so "there is no traffic" is a number and not an impression -- how
	 * many civilians our band actually holds right now, and how many the claim gave
	 * away this window. The engine's own spawn rate is elsewhere (JERICHO_DIAG_PINGIN
	 * in civ_ai.c); this is the half that is OURS. */
	if ((gMp.frame % 120) == 0 && gMpCtx != NULL)
	{
		gMpCtx->jer_log(gMpCtx,
			"[mp] traffic: %d civ car(s) in our band %d..%d, nearest %d, culled %d this window\n",
			census, first, last, (nearSq < 0) ? -1 : (int)sqrt((double)nearSq), gMpClaimCulled);
		gMpClaimCulled = 0;
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

	/* Re-assert the traffic slot bands FIRST, before anything spawns this frame. A
	 * level load clears the engine's reservedSlots out from under us, so this is
	 * not a one-time setup -- see MpTrafficBandRefresh. */
	MpTrafficBandRefresh();

	/* ...and then give up any civilian of ours that a peer's player now owns, so
	 * two players standing together do not both grow traffic in the same patch. */
	MpTrafficClaimPass();

	/* ...and collect any mirror its owner has stopped speaking about. */
	MpTrafficMirrorAgeTick();

	/* the test levers are read from the environment ONCE, not every frame (see
	 * MpResolveTestLevers); the ticks below then only consult the cache */
	MpResolveTestLevers();

	/* test lever: prove the sim tick is still running (MP_HEARTBEAT) */
	MpHeartbeatTick();

	/* test lever: a scripted mid-session car change (inert unless MP_TEST_CARCHANGE) */
	MpTestCarChangeTick();
	MpTestCityChangeTick();
	MpTestCarCycleTick();

	/* test lever: a synthetic traffic contact, to exercise the traffic-hit wire path
	 * without a physical collision (inert unless MP_TEST_TRAFFIC_HIT). */
	MpTestTrafficHitTick();

	/* test lever: drop our car off the map, to exercise the game-over refusal
	 * (inert unless MP_TEST_FALLOFF). */
	MpTestFallOffTick();

	/* test lever: a clean leave part-way through (inert unless MP_TEST_LEAVE), so
	 * that "a deliberate quit" and "a connection died" can be told apart in a
	 * log without a human sitting at the menu. */
	MpTestLeaveTick();

	/* test lever: get out of the car part-way through (inert unless MP_TEST_ONFOOT),
	 * so the on-foot path has a way to be exercised at all. */
	MpTestOnFootTick();

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
		MpHostSendRoster();

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

		if (gMp.config.colorOn != lastOn || gMp.config.colorR != lastR ||
		    gMp.config.colorG != lastG || gMp.config.colorB != lastB)
		{
			lastOn = gMp.config.colorOn;
			lastR = gMp.config.colorR;
			lastG = gMp.config.colorG;
			lastB = gMp.config.colorB;

			MpSendColors(0);
		}
	}

	/* OWNER-AUTHORITATIVE: every machine sends the ONE car it owns, every frame,
	 * and everyone else adopts that state. The old model had each machine simulate
	 * every car from input that arrived a round trip late and hoped a coarse resync
	 * would pull them back together -- which is exactly where the drift came from. */
	MpSendOwnCarState();

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
/* MP_TEST_LEAVE=<secs> -- leave the session cleanly that many seconds after the
 * match starts, then drop to the frontend, exactly as the pause menu's Exit
 * does. The point is the CONTRAST: a deliberate quit must read completely
 * differently in the log from a connection that died, on BOTH machines, and
 * that is not something a human can be asked to reproduce on demand.
 *
 * Reads the env into a variable rather than testing getenv() inline, like the
 * other test levers, so it cannot look like a debug switch to
 * tools/check_debug_independence.py. */
static void MpTestLeaveTick(void)
{
	static int done;
	static unsigned long startMs;
	const char* s;
	unsigned long now = MpNowMs();

	if (done || !gMp.running)
		return;

	/* Client only: the interesting case is the HOST watching a client say
	 * goodbye, and running it on both machines at once would make the two logs
	 * ambiguous about who actually left. */
	if (gMp.role != MP_ROLE_CLIENT)
		return;

	s = gTestLeaveStr;

	if (s == NULL)
		return;

	if (startMs == 0)
		startMs = now;

	if ((now - startMs) < (unsigned long)(atoi(s) * 1000))
		return;

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: leaving the session cleanly %ss in (MP_TEST_LEAVE)\n", s);

	MpLeaveSession();
	MpReturnToFrontend();
}

/* MP_TEST_ONFOOT=<secs> -- get the local player OUT of their car that many
 * seconds in, so the on-foot path can be exercised without a human at the wheel.
 *
 * Uses the engine's own way out (ChangeCarPlayerToPed), the same call the
 * car-change lever makes. The car we leave stays exactly where it is: that is the
 * designed behaviour (see MpReleaseRemoteCar -- handing a module-made car to the
 * traffic AI is a crash, not a feature).
 *
 * Separate from MP_TEST_CARCHANGE on purpose: that lever has to find a civilian
 * car of a different model before it will do anything, which makes it useless as
 * a way to test THIS. */
static void MpTestOnFootTick(void)
{
	static int done;
	static int noted;
	static unsigned long startMs;
	const char* s;
	unsigned long now = MpNowMs();
	MP_PLAYER* me;

	if (done || !gMp.running)
		return;

	s = gTestOnFootStr;

	if (s == NULL)
		return;

	/* Say ONCE that the lever was seen. Without it, a run that does not fire is
	 * ambiguous: never called, called before the match, or called and not yet due
	 * all look identical in the log. */
	if (!noted)
	{
		noted = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_ONFOOT=%s armed (frame %lu)\n",
				s, gMp.frame);
	}

	if (startMs == 0)
		startMs = MpNowMs();

	me = MpLocalPlayer();

	/* MP_DEBUG: name the wait and the car state, so "it did not fire" can be read
	 * rather than guessed at. Same inline-condition form as MpLinkTick, so nothing
	 * is conditional on the flag. */
	if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % 60) == 0)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: onfoot waited %lums of %d000ms, carId %d, running %d\n",
			now - startMs, atoi(s), me != NULL ? me->carId : -99, gMp.running);

	if ((now - startMs) < (unsigned long)(atoi(s) * 1000))
		return;

	if (me == NULL || me->carId < 0)
		return;			/* not in a car (yet): nothing to get out of */

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: getting OUT now, %ss in (MP_TEST_ONFOOT)\n", s);

	/* The engine's real leave-car path (handling.c) calls
	 * ActivatePlayerPedestrian FIRST: it creates the player's ped and links it
	 * in player[0].pPed, and ChangeCarPlayerToPed then points spoolXZ at that
	 * ped. Calling ChangeCarPlayerToPed directly leaves spoolXZ pointing at a
	 * NULL ped, and the next civ-AI pass (CivControl -> CheckPingOut) faults on
	 * it. Mirror the engine: activate the ped, then step out. */
	ActivatePlayerPedestrian(&car_data[me->carId], NULL, 0, NULL, TANNER_MODEL);
	ChangeCarPlayerToPed(0);
}

/* MP_TEST_PAUSECAR=<secs>[,<city>,<model>][;<secs>[,<city>,<model>]...] -- run the
 * pause menu's Change car path that many seconds into a match, exactly as pressing
 * Apply does. A `;`-separated LIST runs several switches, each once and in order; every
 * <secs> counts from the moment the lever armed (not from the previous switch), so
 * `30,2,1;45,3,1` switches to VEGAS 1 at 30 s and RIO 1 at 45 s. The single form is
 * the one-entry list and behaves as it always did.
 *
 * The picker itself needs a human at a menu, so without this the one part of the
 * feature that can go wrong quietly -- the swap and its replication, and (with a
 * list) whether each switch gives the previous car's slot back -- would only ever be
 * looked at by hand. <city> defaults to the session's own and <model> to the SECOND
 * car in that city's roster (the first is usually the one already being driven,
 * which would make the run say nothing). Inert unless set. */
void MpTestPauseCarTick(void)
{
	static int next, noted;
	static unsigned long startMs;
	const char* s;
	const char* e;
	const char* end;
	unsigned long now;
	int secs, city, model = -1, idx, total = 1;

	if (!gMp.running)
	{
		/* SINGLE PLAYER: the same pause-menu row is reachable in a solo game, so the
		 * lever drives it there too -- that is the only padless way to exercise the
		 * single-player swap. It does need the player to BE in a car; MpChangeCar
		 * reports that itself if they are not. */
		if (MainPlayer.playerType != PLAYER_TYPE_CAR)
			return;
	}

	s = getenv("MP_TEST_PAUSECAR");

	if (s == NULL)
		return;

	for (e = s; *e != '\0'; e++)
	{
		if (*e == ';' && e[1] != '\0')
			total++;
	}

	if (!noted)
	{
		noted = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_PAUSECAR=%s armed - %d switch(es) (frame %lu)\n",
				s, total, gMp.frame);
	}

	if (startMs == 0)
		startMs = MpNowMs();

	/* the entry to run next */
	e = s;

	for (idx = 0; idx < next && e != NULL; idx++)
	{
		e = strchr(e, ';');

		if (e != NULL)
			e++;
	}

	if (e == NULL || *e == '\0')
		return;			/* every entry has run */

	now = MpNowMs();
	secs = atoi(e);

	if ((int)((now - startMs) / 1000) < secs)
		return;

	next++;

	city = (gMp.city >= 0 && gMp.city < 4) ? gMp.city : 0;
	end = strchr(e, ';');

	{
		const char* c1 = strchr(e, ',');

		if (c1 != NULL && (end == NULL || c1 < end))
		{
			const char* c2;

			city = atoi(c1 + 1);
			c2 = strchr(c1 + 1, ',');

			if (c2 != NULL && (end == NULL || c2 < end))
				model = atoi(c2 + 1);
		}
	}

	if (model < 0)
	{
		int slots[MP_CAR_LIST_MAX];
		int models[MP_CAR_LIST_MAX];
		char list[192];
		size_t used;
		int n, k;

		if (city < 0 || city >= CITY_COUNT)
			city = 0;

		/* The SAME answer the pause picker builds its menu from (MpCarListForCity), so a
		 * padless run can still see what the menu would offer - and the two cannot drift,
		 * because it is one call. Printing the roster (not just a count) is the point: the
		 * question this lever keeps being used to answer is "which cars can players pick?". */
		n = MpCarListForCity(city, slots, models, MP_CAR_LIST_MAX);

		list[0] = 0;

		for (k = 0; k < n; k++)
		{
			used = strlen(list);

			if (used < sizeof(list) - 24)
				snprintf(list + used, sizeof(list) - used, "%s[%d]=%d",
					(k > 0) ? " " : "", slots[k], models[k]);
		}

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: PAUSECAR city %d roster: %d slot(s) - %s\n",
				city, n, (list[0] != 0) ? list : "(none)");

		/* the SECOND car in the list, as this lever always did: a change onto a different
		 * model, so a run can tell the switch happened */
		if (n > 0)
			model = models[(n > 1) ? 1 : 0];
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] test: PAUSECAR change %d of %d now to city %d model %d\n",
			next, total, city, model);

	MpChangeCar(city, model);
}

/* MP_TEST_RESTART=<secs> -- ask for the multiplayer Restart that many seconds
 * into a match, exactly as pressing Restart in the pause menu does.
 *
 * It FIRES THE ENGINE'S EVENT rather than calling the reset directly, so it can
 * only pass when the whole chain is wired: the hook in main.c, mp's claim, and
 * MpSoftRestart. Inert unless set. */
static void MpTestRestartTick(void)
{
	static int done, noted;
	static unsigned long startMs;
	const char* s;
	unsigned long now;

	if (done || !gMp.running)
		return;

	s = getenv("MP_TEST_RESTART");

	if (s == NULL)
		return;

	if (!noted)
	{
		noted = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_RESTART=%s armed (frame %lu)\n",
				s, gMp.frame);
	}

	if (startMs == 0)
		startMs = MpNowMs();

	now = MpNowMs();

	if ((int)((now - startMs) / 1000) < atoi(s))
		return;

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] test: pressing RESTART now\n");

	{
		JER_ARGS_GAME_QUIT q;

		q.code = JER_PAUSE_QUIT_RESTART;

		jer_fire(JER_EVENT_GAME_QUIT, &q);
	}
}

/* MP_TEST_FALLOFF=<secs> -- drop our car off the map that many seconds in.
 *
 * The only way to reach the game-over refusal (JER_PAUSE_GAMEOVER) without driving off a
 * cliff by hand: the engine arms the death fade when the car's y goes below -1000, so this
 * just puts it there. What proves the refusal is the pair of log lines - the engine's fade
 * arming and then our "[mp] death in a session: respawned ... game over refused" - with no
 * pause menu and no black screen after it. */
static void MpTestFallOffTick(void)
{
	static int done;
	static unsigned long startMs;
	const char* s;
	MP_PLAYER* me;

	if (done || !gMp.running)
		return;

	s = gTestFallOffStr;

	if (s == NULL)
		return;

	if (startMs == 0)
		startMs = MpNowMs();

	if (MpNowMs() - startMs < (unsigned long)atoi(s) * 1000UL)
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_FALLOFF - dropping our car off the map\n");

	car_data[me->carId].hd.where.t[1] = -2000;
}

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

	s = gTestCarChangeStr;

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

		/* Same as MP_TEST_ONFOOT: activate the ped first so ChangeCarPlayerToPed
		 * has a valid player[0].pPed to point spoolXZ at (else the next civ-AI
		 * pass faults in CivControl -> CheckPingOut). */
		me = MpLocalPlayer();

		if (me != NULL && me->carId >= 0)
			ActivatePlayerPedestrian(&car_data[me->carId], NULL, 0, NULL, TANNER_MODEL);

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

/* MP_TEST_CITYCHANGE=<secs>,<city>[,<slot>] -- the pause menu's "Respawn as this car",
 * taken by itself at <secs> seconds, for a car from a DIFFERENT city.
 *
 * MP_TEST_CARCHANGE above takes over the nearest LOCAL traffic car, and that is the wrong
 * test for the mid-match import: the peer is in the same level, so it already holds that
 * car and there is nothing to import at all. What has to be imported is a car the peer does
 * NOT hold, and a car from another city is exactly that -- so this lever drives the real
 * case, through the same entry point the menu uses (MpChangeCar), deliberately, rather than
 * through a second path that could drift away from it.
 *
 * city indexes the cities the car mods know (MpCarQueryCities), slot indexes the engine's
 * own per-city model table (carNumLookup), and both are explicit so a pair run can send each
 * seat to a different city and exercise both directions at once.
 *
 * Set = "<secs>,<city>[,<slot>]". Any other shape does nothing, so an absent or malformed
 * value can never change a car by itself. */
static void MpTestCityChangeTick(void)
{
	static int done;
	static unsigned long startMs;
	const char* s;
	const char* comma;
	extern char carNumLookup[CITY_COUNT][10];
	int secs, city, slot = 0, i, n, model;
	int cities[8];
	MP_PLAYER* me;
	unsigned long now;

	if (done)
		return;

	s = gTestCityChangeStr;

	if (s == NULL || !gMp.running)
		return;

	comma = strchr(s, ',');

	if (comma == NULL)
		return;			/* "<secs>,<city>" is the minimum */

	secs = atoi(s);
	city = atoi(comma + 1);

	comma = strchr(comma + 1, ',');

	if (comma != NULL)
	{
		slot = atoi(comma + 1);

		if (slot < 0 || slot >= 10)
			slot = 0;
	}

	if (startMs == 0)
		startMs = MpNowMs();

	now = MpNowMs();

	if ((int)((now - startMs) / 1000) < secs)
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0)
		return;			/* no car of ours yet: try again next frame */

	/* Only a city the mods know, and only an index the engine's own model table has:
	 * MpChangeCar resolves the pair against resident slots, so anything else is a failed
	 * switch reported as if it were a real one. */
	if (city < 0 || city >= CITY_COUNT)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: MP_TEST_CITYCHANGE city %d is out of range - nothing done\n", city);

		done = 1;
		return;
	}

	n = MpCarQueryCities(cities, (int)(sizeof(cities) / sizeof(cities[0])));

	for (i = 0; i < n; i++)
		if (cities[i] == city)
			break;

	if (i == n)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: MP_TEST_CITYCHANGE city %d is not one the mods know - nothing done\n",
				city);

		done = 1;
		return;
	}

	model = (int)(unsigned char)carNumLookup[city][slot];

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: MP_TEST_CITYCHANGE -> %s model %d (we are model %d)\n",
			MpCarCityName(city), model, car_data[me->carId].ap.model);

	MpChangeCar(city, model);
}

/* MP_TEST_CARCYCLE=<interval_ms>[,<passes>]
 *
 * Drive this seat through EVERY car the session can offer, in a random order, as fast as
 * asked - the stress rig for the whole car path. Three seats on one machine at 700 ms each
 * is a car change somewhere in the session every ~230 ms, far past anything a player does,
 * and it is where slot reuse, the palette rows, the texture pages and the peer folds either
 * hold or start to leak, go invisible or refuse.
 *
 * The list comes from MpCarListForCity - the SAME answer the pause picker builds its menu
 * from - so "every car" is every car this session would let a player pick, rather than a
 * guess at model numbers that the level may not hold. Each seat shuffles with its own seed
 * (its player id), so the seats walk the same list in different orders: one is importing a
 * car while another is switching away from it, which is the interesting overlap.
 *
 * Every change goes through MpChangeCar, the call the pause menu's Apply row makes, so this
 * exercises the real path and not a shortcut. Each completed pass is logged, and so is every
 * change, because the rig counts them.
 *
 * passes = 0 (the default) keeps reshuffling until the run ends. */
static void MpTestShuffle(int (*items)[2], int count, int* seed)
{
	int i;

	for (i = count - 1; i > 0; i--)
	{
		int j, a, b;

		*seed = *seed * 1103515245 + 12345;
		j = (int)(((unsigned)(*seed >> 8)) % (unsigned)(i + 1));

		if (j == i)
			continue;

		a = items[i][0];
		b = items[i][1];
		items[i][0] = items[j][0];
		items[i][1] = items[j][1];
		items[j][0] = a;
		items[j][1] = b;
	}
}

#define MP_TEST_CYCLE_MAX_CARS (MP_CAR_LIST_MAX * 4)

static void MpTestCarCycleTick(void)
{
	static int built;
	static int intervalMs = -1;
	static int passes;
	static unsigned long nextAt;
	static int idx, total, pass, seed;
	static int cars[MP_TEST_CYCLE_MAX_CARS][2];
	MP_PLAYER* me;

	if (gTestCarCycleStr == NULL || !gMp.running)
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0)
		return;

	if (intervalMs < 0)
	{
		const char* comma = strchr(gTestCarCycleStr, ',');

		intervalMs = atoi(gTestCarCycleStr);

		if (intervalMs < 50)
			intervalMs = 50;		/* below this a car cannot be built before the next ask */

		passes = (comma != NULL) ? atoi(comma + 1) : 0;
	}

	if (!built)
	{
		int city;

		built = 1;
		seed = (int)(0x2545F491u * (unsigned)(me->id + 1));	/* per seat: different orders */

		for (city = 0; city < CITY_COUNT && total < MP_TEST_CYCLE_MAX_CARS; city++)
		{
			int models[MP_CAR_LIST_MAX];
			int slots[MP_CAR_LIST_MAX];
			int count = MpCarListForCity(city, slots, models, MP_CAR_LIST_MAX);
			int i;

			for (i = 0; i < count && total < MP_TEST_CYCLE_MAX_CARS; i++)
			{
				cars[total][0] = city;
				cars[total][1] = models[i];
				total++;
			}
		}

		if (total == 0)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] test: car cycle: no car is offered - nothing to cycle\n");

			gTestCarCycleStr = NULL;
			return;
		}

		MpTestShuffle(cars, total, &seed);
		nextAt = MpNowMs();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: car cycle: %d car(s), a change every %d ms, %s\n",
				total, intervalMs, (passes > 0) ? "a fixed number of passes" : "until the run ends");
	}

	if (MpNowMs() < nextAt)
		return;

	nextAt = MpNowMs() + (unsigned long)intervalMs;

	MpChangeCar(cars[idx][0], cars[idx][1]);
	idx++;

	if (idx < total)
		return;

	pass++;
	idx = 0;
	MpTestShuffle(cars, total, &seed);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: car cycle PASS %d done - every one of the %d car(s) has been driven\n",
			pass, total);

	if (passes > 0 && pass >= passes)
		gTestCarCycleStr = NULL;		/* asked for a number of passes and they are done */
}


/* A SATURATING cast to int16, for the wire fields that are still 16-bit.
 *
 * The orientation is a quaternion whose components are normally small, but a
 * violent impact can push one past 32767 -- and a WRAPPING cast turns that into a
 * completely different orientation on the peer, which is how a crash becomes a
 * car pointing somewhere absurd. Clamp instead: a saturated extreme is at least
 * in the right direction. */
static int16_t MpSat16(long v)
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

static long MpClampAngVel(long v)
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

		if (reentry)
			MpCarQueryChosen(me->carCity, me->car, 1);
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

		me->carId = -1;
		me->car = -1;
		me->onFoot = 1;		/* OUR row: the next car we get into is a choice (above) */
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

/* The pedestrian the engine is driving for OUR player, or NULL while we are in a
 * car. The engine ties the ped to us in player[0].pPed when it puts us on foot,
 * so take it straight from there: deriving it from a padId scan used to misfire,
 * because a spawned stand-in's padId is never set and could inherit whatever the
 * pooled slot last held. */
static LPPEDESTRIAN MpLocalPed(void)
{
	if (player[0].playerCarId < 0)
		return player[0].pPed;

	return NULL;
}

/* Owner -> peers: where our on-foot player is and how fast they are moving.
 * Sent only while on foot; the car state already says "no car" (model 0xFF), so a
 * peer knows to look for this rather than guessing from a paused car. */
static void MpSendOwnPedState(void)
{
	unsigned char buf[sizeof(MP_PEDSTATE) + sizeof(MP_PEDSTATE_ENTRY)];
	MP_PEDSTATE h;
	MP_PEDSTATE_ENTRY e;
	MP_PLAYER* me = MpLocalPlayer();
	LPPEDESTRIAN p;
	int len;

	if (me == NULL || !gMp.running)
		return;

	if (me->carId >= 0)
		return;			/* in a car: the car state is the whole story */

	p = MpLocalPed();

	if (p == NULL)
	{
		/* We are on foot but the engine has no pedestrian we can find for our own
		 * player, so there is nothing to send and the other machines see nobody.
		 * That is the whole reason "I can't see other players' Tanners" can happen
		 * even when everything else is healthy, so say it (throttled) rather than
		 * returning in silence.
		 *
		 * The lookup is by padId: there is no playerPedId, and pedest.c only ties
		 * a ped to a player slot that way. */
		if (gMpCtx != NULL && (gMp.frame % 120) == 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] ped: on foot but found no pedestrian for our own player "
				"(padid %d, carId %d) -- peers will see nobody\n",
				(int)player[0].padid, me->carId);

		return;
	}

	memset(&h, 0, sizeof(h));
	memset(&e, 0, sizeof(e));

	h.count = 1;

	e.playerId = (uint8_t)me->id;
	e.flags = (uint8_t)(p->speed != 0 ? MP_PED_MOVING : 0);
	e.x = p->position.vx;
	e.y = p->position.vy;
	e.z = p->position.vz;
	e.heading = p->dir.vy & 0xFFF;
	e.speed = p->speed;

	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), &e, sizeof(e));
	len = (int)(sizeof(h) + sizeof(e));

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_PED, 0, buf, len);
	else
		MpSendToHost(MP_TAG_PED, 0, buf, len);
}

/* The owner's idea of where their on-foot player is. Anything we DO about it
 * happens in MpDriveRemotePed, on a sim frame, where the ped table is safe to
 * touch. */
static void MpHandlePedState(int connIndex, const unsigned char* p, int len)
{
	MP_PEDSTATE h;
	int i, n;

	if (len < (int)sizeof(MP_PEDSTATE))
		return;

	memcpy(&h, p, sizeof(h));

	n = h.count;

	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	if (len < (int)(sizeof(MP_PEDSTATE) + (size_t)n * sizeof(MP_PEDSTATE_ENTRY)))
		return;

	for (i = 0; i < n; i++)
	{
		MP_PEDSTATE_ENTRY e;
		MP_PLAYER* pl;

		memcpy(&e, p + sizeof(MP_PEDSTATE) + i * sizeof(e), sizeof(e));

		pl = MpGetPlayer(e.playerId);

		if (pl == NULL || pl->isLocal)
			continue;

		pl->pedX = e.x;
		pl->pedY = e.y;
		pl->pedZ = e.z;
		pl->pedHeading = (int)(e.heading & 0xFFF);
		pl->pedSpeed = e.speed;
		pl->pedLastMs = MpNowMs();
	}

	/* The host is the hub, and this needs relaying exactly as the carstate does:
	 * a client sends its pedestrian to the host alone, so without this only the
	 * host ever sees an on-foot CLIENT -- every other client sees a gap where a
	 * person should be, which reads as "I can see the host on foot but not each
	 * other". The carstate relay (MpHandleCarState) is the same rule. */
	if (MpIsHost() && connIndex >= 0)
		MpHostRelay(connIndex, MP_TAG_PED, 0, p, len);
}

/* Keep the pedestrian we are standing in for a REMOTE on-foot player in step,
 * spawning and despawning it as that player gets in and out. Every sim frame,
 * every player: building peds from a network callback would do it mid-poll. */
static void MpDriveRemotePed(MP_PLAYER* p)
{
	JerNpc* n;
	LPPEDESTRIAN ped;
	int dx, dz, err;

	if (p == NULL || !p->active || p->isLocal)
		return;

	n = (JerNpc*)p->ped;

	/* Not on foot any more -- back in a car, gone, or never heard from: take the
	 * stand-in away. A pedestrian left standing would read as a third player. */
	if (p->carId >= 0 || !p->connected || p->pedLastMs == 0)
	{
		if (n != NULL)
		{
			jer_npc_despawn(n);
			p->ped = NULL;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] ped: player %d is back in a car; stand-in removed\n", p->id);
		}

		return;
	}

	if (n == NULL)
	{
		n = jer_npc_spawn_model(TANNER_MODEL, p->pedX, p->pedZ);

		if (n == NULL)
		{
			/* The ped pool can refuse. Say so (throttled): silently having no
			 * character for a player is the confusing half of this feature. */
			if (gMpCtx != NULL && (gMp.frame % 120) == 0)
				gMpCtx->jer_log(gMpCtx,
					"[mp] ped: no free pedestrian for player %d (pool full?)\n", p->id);

			return;
		}

		p->ped = n;

		/* A stand-in is NOT a local player's ped: mark it so a padId-keyed scan (or
		 * anything else that treats padId >= 0 as "someone's character") can never
		 * mistake it for ours. The spawn path leaves padId untouched, so a pooled
		 * slot keeps whatever the previous occupant held -- which is how a remote
		 * Tanner could inherit the local player's colour. */
		((LPPEDESTRIAN)n)->padId = -1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] ped: standing in for player %d at %d,%d,%d heading %d speed %d\n",
				p->id, p->pedX, p->pedY, p->pedZ, p->pedHeading, p->pedSpeed);
	}

	ped = (LPPEDESTRIAN)n;

	if (ped == NULL)
	{
		p->ped = NULL;
		return;
	}

	/* Let the ENGINE walk it, and correct only when it has actually drifted.
	 * Pinning it every frame would stop the walk animation from ever showing --
	 * the animation is the whole reason for driving it this way -- while never
	 * correcting it would let it wander. 120 units is about a car length: large
	 * enough that a correction is unmistakable when it happens, small enough that
	 * it is rare. */
	dx = p->pedX - ped->position.vx;
	dz = p->pedZ - ped->position.vz;
	err = (int)sqrt((double)dx * (double)dx + (double)dz * (double)dz);

	if (err > 120)
	{
		jer_npc_set_world(n, p->pedX, p->pedY, p->pedZ, p->pedHeading);

		if (gMpCtx != NULL && MpDebugOn())
			gMpCtx->jer_log(gMpCtx, "[mp] ped: player %d corrected %d units\n", p->id, err);
	}

	if (p->pedSpeed != 0)
	{
		/* move_to takes a POINT, so aim a little way along the owner's heading:
		 * that is the same thing as "walk forward at this speed", and it keeps the
		 * engine turning and animating as it goes. */
		int tx = p->pedX + (int)(((long)rsin(p->pedHeading) * 300) >> 12);
		int tz = p->pedZ + (int)(((long)rcos(p->pedHeading) * 300) >> 12);

		jer_npc_move_to(n, tx, tz, p->pedSpeed);
	}
	else
	{
		jer_npc_stop(n);
		jer_npc_face(n, p->pedHeading);
	}
}

/* WHICH CAR SLOTS ARE OURS.
 *
 * Sticky: once the module has put a player in a car, that slot is a module car for
 * the rest of the session, even after the player gets out of it. That matters
 * because the car is then still in the world with nobody driving it, and the
 * engine's traffic AI treats such a car as fair game (see the top of the file). */

/* OUR CARS ARE NEVER THE TRAFFIC AI'S -- and never a free slot either.
 *
 * Our cars are built by InitPlayer, so they have no traffic-AI data. The civ-AI
 * pass (main.c: PingInCivCar, which scans for a slot whose controlType is free)
 * will take one and then walk into those fields: an access violation at
 * PingInCivCar+0x105, on both machines, part-way into an ordinary match with
 * nobody asking for anything.
 *
 * A car of ours that a player steps out of is left as CIV_AI with
 * thrustState = STOP / ctrlState = EMPTY (see MpFollowLocalCar): safe for the AI
 * AND enterable again, so that state is deliberately NOT taken back here. What
 * still has to be taken back is CONTROL_TYPE_NONE: a wrecked/abandoned car is a
 * free slot the traffic AI will recycle, which it cannot survive.
 * CONTROL_TYPE_PLAYER with nobody driving it is exactly what an unowned remote
 * car already is: it sits where it was left, and the AI ignores it.
 *
 * Every frame, in PRE_SIM -- which is BEFORE the civ-AI pass in the same frame, so
 * a car handed over this frame never reaches the AI at all. */
/* A slot that is no longer OURS. The gMpOurCars mark is sticky on purpose - the
 * traffic AI must never reach a car one of us owns - but it has to be dropped
 * when the owner leaves. MpKeepOurCarsFromTrafficAi below takes any marked slot
 * straight back off CONTROL_TYPE_NONE, which is exactly what removing a
 * departing player's car sets: without this the car flickered out and re-appeared
 * on the very next frame, looking like an identical car parked in its place. */
void MpReleaseOurCarSlot(int slot)
{
	if (slot >= 0 && slot < MAX_CARS)
		gMpOurCars[slot] = 0;
}

static void MpKeepOurCarsFromTrafficAi(void)
{
	int i;

	/* First, mark what is ours right now (and keep the mark: see gMpOurCars). */
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];
		int slot = p->carId;

		if (p->active && slot >= 0 && slot < MAX_CARS)
			gMpOurCars[slot] = 1;
	}

	for (i = 0; i < MAX_CARS; i++)
	{
		if (!gMpOurCars[i])
			continue;

		if (car_data[i].controlType == CONTROL_TYPE_NONE)
		{
			int was = car_data[i].controlType;

			car_data[i].controlType = CONTROL_TYPE_PLAYER;
			car_data[i].wheel_angle = 0;

			/* Same as the get-out path: an unowned car must not keep reading
			 * somebody's pad, or it drives off on its own. */
			car_data[i].ai.padid = &gMpQuietPad;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] car: our slot %d was controlType %d (free); "
					"taken back so the AI cannot reach it\n", i, was);
		}
	}
}

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

/* The pedestrian the ENGINE is driving for our own player, as a raw pointer,
 * for the draw hook (which cannot include pedest.h). NULL in a car. */
void* MpLocalPedPtr(void)
{
	return (void*)MpLocalPed();
}

static int MpColorLen(int count)
{
	return (int)(sizeof(MP_COLOR) + (size_t)count * sizeof(MP_COLOR_ENTRY));
}

/* whole = 1: the host's table of everyone. whole = 0: just us. */
static void MpSendColors(int whole)
{
	unsigned char buf[sizeof(MP_COLOR) + MP_MAX_PLAYERS * sizeof(MP_COLOR_ENTRY)];
	MP_COLOR h;
	int i, n = 0;

	memset(&h, 0, sizeof(h));

	if (whole)
	{
		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			MP_PLAYER* p = &gMp.players[i];
			MP_COLOR_ENTRY e;

			if (!p->active)
				continue;

			memset(&e, 0, sizeof(e));
			e.playerId = (uint8_t)p->id;
			/* The local player's own colour lives in the config, not the registry row
			 * (which is only ever filled from the wire, and is zeroed for the host).
			 * Both send paths must agree on this or the host's colour is broadcast as
			 * black while the just-us path sends the real one. */
			if (p->isLocal)
			{
				e.on = (uint8_t)(gMp.config.colorOn ? 1 : 0);
				e.r = (uint8_t)gMp.config.colorR;
				e.g = (uint8_t)gMp.config.colorG;
				e.b = (uint8_t)gMp.config.colorB;
			}
			else
			{
				e.on = (uint8_t)(p->colorOn ? 1 : 0);
				e.r = (uint8_t)p->colorR;
				e.g = (uint8_t)p->colorG;
				e.b = (uint8_t)p->colorB;
			}

			memcpy(buf + sizeof(h) + n * sizeof(e), &e, sizeof(e));
			n++;
		}
	}
	else
	{
		MP_PLAYER* me = MpLocalPlayer();
		MP_COLOR_ENTRY e;

		if (me == NULL)
			return;

		memset(&e, 0, sizeof(e));
		e.playerId = (uint8_t)me->id;
		e.on = (uint8_t)(gMp.config.colorOn ? 1 : 0);
		e.r = (uint8_t)gMp.config.colorR;
		e.g = (uint8_t)gMp.config.colorG;
		e.b = (uint8_t)gMp.config.colorB;

		memcpy(buf + sizeof(h), &e, sizeof(e));
		n = 1;
	}

	if (n <= 0)
		return;

	h.count = (uint8_t)n;
	memcpy(buf, &h, sizeof(h));

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_COLOR, 0, buf, MpColorLen(n));
	else
		MpSendToHost(MP_TAG_COLOR, 0, buf, MpColorLen(n));
}

static void MpHandleColor(int connIndex, const unsigned char* p, int len)
{
	MP_COLOR h;
	int i, n;

	(void)connIndex;

	if (len < (int)sizeof(MP_COLOR))
		return;

	memcpy(&h, p, sizeof(h));

	n = h.count;

	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	if (len < MpColorLen(n))
		return;

	for (i = 0; i < n; i++)
	{
		MP_COLOR_ENTRY e;
		MP_PLAYER* pl;

		memcpy(&e, p + sizeof(MP_COLOR) + i * sizeof(e), sizeof(e));

		pl = MpGetPlayer(e.playerId);

		if (pl == NULL)
			continue;

		/* A colour is applied to its OWNER's character. The local player's own
		 * colour lives in the config (that is what the menu edits); for everyone
		 * else it is this table. */
		if (!pl->isLocal)
		{
			pl->colorOn = e.on ? 1 : 0;
			pl->colorR = e.r;
			pl->colorG = e.g;
			pl->colorB = e.b;
		}
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
	e.modelCity = MP_CAR_CITY_SESSION;

	if (me->carId >= 0 && me->carId < MAX_CARS)
	{
		cp = &car_data[me->carId];

		/* MP_DEBUG: the ORIENTATION is now the only 16-bit field left on the wire, so
		 * it is the only one a violent impact can saturate. If this fires, the peer is
		 * seeing a CLAMPED attitude (the intended degradation) rather than a wrapped
		 * one. Log-only. */
		if (MpDebugOn() && gMpCtx != NULL)
		{
			int a, big = 0;

			for (a = 0; a < 4; a++)
			{
				if (cp->st.n.orientation[a] > 32767 || cp->st.n.orientation[a] < -32768)
					big = 1;
			}

			if (big)
			{
				gMpCtx->jer_log(gMpCtx,
					"[mp] WIRE: orientation clamped on the wire -- orient %d %d %d %d\n",
					(int)cp->st.n.orientation[0], (int)cp->st.n.orientation[1],
					(int)cp->st.n.orientation[2], (int)cp->st.n.orientation[3]);
			}
		}

		e.palette = (uint8_t)cp->ap.palette;	/* the colour WE see our car in -- we own it */

		/* ...and WHAT we are driving, as a model NUMBER plus the city that number
		 * belongs to -- NOT our resident slot index, which does not mean the same
		 * car on two machines. The peer resolves the pair to ITS own slot
		 * (MpResidentSlotForCar). */
		{
			int slot = cp->ap.model;

			if (slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS && residentCarModels[slot] >= 0)
			{
				int src = GetCarModelSourceCity(slot);

				e.model = (uint8_t)residentCarModels[slot];
				e.modelCity = (uint8_t)((src >= 0) ? src : MP_CAR_CITY_SESSION);
			}
			/* else: the slot holds no model, so there is no car to describe --
			 * e.model stays MP_CARSTATE_NO_CAR (set above) and the peers release. */
		}

		e.x = cp->hd.where.t[0];
		e.y = cp->hd.where.t[1];
		e.z = cp->hd.where.t[2];
		e.heading = cp->hd.direction;
		e.orient[0] = MpSat16(cp->st.n.orientation[0]);
		e.orient[1] = MpSat16(cp->st.n.orientation[1]);
		e.orient[2] = MpSat16(cp->st.n.orientation[2]);
		e.orient[3] = MpSat16(cp->st.n.orientation[3]);
		e.vel[0] = cp->st.n.linearVelocity[0];
		e.vel[1] = cp->st.n.linearVelocity[1];
		e.vel[2] = cp->st.n.linearVelocity[2];
		e.angVel[0] = cp->st.n.angularVelocity[0];
		e.angVel[1] = cp->st.n.angularVelocity[1];
		e.angVel[2] = cp->st.n.angularVelocity[2];
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
/* Traffic state: publish the civilians and police OUR band owns
 *
 * The band is a DISJOINT set of car_data slots, so -- unlike a player car, which
 * is matched by model -- a slot NUMBER is a shared identity here: the receiver
 * mirrors a row straight into the slot it names. One row per civilian/police car
 * we currently hold, batched into a single message. The count is capped by the
 * band (at most a handful), so this needs no distance throttling.
 *
 * A police car is carried like any other: it is the level's own police MODEL
 * (model 0, the car CarHasSiren keys off), so the siren follows from the model at
 * the receiver and nothing extra is needed here. A REMOVE row (the despawn half)
 * rides the same message. */
static void MpSendTrafficState(void)
{
	unsigned char buf[sizeof(MP_TRAFFIC) + MAX_CARS * sizeof(MP_TRAFFIC_ENTRY)];
	unsigned char live[MAX_CARS];
	MP_TRAFFIC h;
	MP_TRAFFIC_ENTRY e;
	int first, last, i, count = 0, len;

	if (!MpTrafficBand(&first, &last))
		return;

	memset(live, 0, sizeof(live));
	memset(&h, 0, sizeof(h));
	h.frame = gMp.frame;

	for (i = first; i <= last; i++)
	{
		CAR_DATA* cp = &car_data[i];
		int slot, src;

		if (cp->controlType != CONTROL_TYPE_CIV_AI &&
		    cp->controlType != CONTROL_TYPE_PURSUER_AI)
			continue;

		if (count >= MAX_CARS)
			break;

		memset(&e, 0, sizeof(e));
		e.carSlot = (uint8_t)i;

		/* WHAT it is, as a model NUMBER plus the city that number belongs to -- the
		 * peer resolves the pair to its own resident slot (MpResidentSlotForCar). */
		slot = cp->ap.model;

		if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS || residentCarModels[slot] < 0)
			continue;	/* no model -> nothing the peer could build */

		src = GetCarModelSourceCity(slot);
		e.model = (uint8_t)residentCarModels[slot];
		e.modelCity = (uint8_t)((src >= 0) ? src : MP_CAR_CITY_SESSION);
		e.palette = (uint8_t)cp->ap.palette;

		/* ...and its DAMAGE, so a bent car reads the same on both screens. */
		{
			int z;

			for (z = 0; z < 6; z++)
				e.damage[z] = cp->ap.damage[z];

			e.totalDamage = (uint16_t)cp->totalDamage;
		}

		e.x = cp->hd.where.t[0];
		e.y = cp->hd.where.t[1];
		e.z = cp->hd.where.t[2];
		e.heading = cp->hd.direction;
		e.orient[0] = MpSat16(cp->st.n.orientation[0]);
		e.orient[1] = MpSat16(cp->st.n.orientation[1]);
		e.orient[2] = MpSat16(cp->st.n.orientation[2]);
		e.orient[3] = MpSat16(cp->st.n.orientation[3]);
		e.vel[0] = cp->st.n.linearVelocity[0];
		e.vel[1] = cp->st.n.linearVelocity[1];
		e.vel[2] = cp->st.n.linearVelocity[2];

		memcpy(buf + sizeof(h) + count * sizeof(e), &e, sizeof(e));
		count++;
		live[i] = 1;
	}

	/* DESPAWN: a car we published last frame that is NOT in this frame's rows has
	 * left our band (pinged out, wrecked, taken by a player). Tell the peer to drop
	 * its mirror now, rather than leaving a ghost for its staleness backstop. */
	for (i = first; i <= last && count < MAX_CARS; i++)
	{
		if (!gMpTrafficSent[i] || live[i])
			continue;

		memset(&e, 0, sizeof(e));
		e.carSlot = (uint8_t)i;
		e.flags = MP_TRAFFIC_REMOVE;
		memcpy(buf + sizeof(h) + count * sizeof(e), &e, sizeof(e));
		count++;
	}

	memcpy(gMpTrafficSent, live, sizeof(gMpTrafficSent));

	h.count = (uint8_t)count;
	memcpy(buf, &h, sizeof(h));
	len = (int)(sizeof(h) + count * sizeof(e));

	/* The host is the hub: it broadcasts its own band, and relays each client's on
	 * (MpHandleTraffic does the relay). A client sends its band to the host. */
	if (MpIsHost())
		MpHostBroadcast(MP_TAG_TRAFFIC, 0, buf, len);
	else
		MpSendToHost(MP_TAG_TRAFFIC, 0, buf, len);
}

/* ------------------------------------------------------------------ */
/* The REMOTE player's car: keeping it on the vehicle its owner is driving.
 *
 * A player who gets out of one car and into another changes vehicles mid-session
 * (see MpFollowLocalCar). The carstate carries the model now, so we can follow
 * it; these helpers make our copy match. */

/* A remote player got OUT. The car we were driving for them is left exactly as
 * it is, standing where they left it.
 *
 * DELIBERATELY NOT handed to the traffic AI. That car is one the MOD created
 * (InitPlayer), so its civil-AI state was never set up; flipping its controlType
 * to CIV_AI hands it to the engine's traffic AI, which then steers a car with no
 * AI data -- an access violation inside CivSteerAngle (found from a dump:
 * JERICHO_dev.exe rva 0xC961). Leaving it a player car and simply never sending
 * it anything again does what was asked for ANYWAY: it stays put (the engine's
 * input fallback coasts it to a stop) instead of an AI driving it away. */
static void MpReleaseRemoteCar(MP_PLAYER* p)
{
	CAR_DATA* cp;

	if (p == NULL || p->carId < 0 || p->carId >= MAX_CARS)
		return;

	cp = &car_data[p->carId];

	if (cp->controlType == CONTROL_TYPE_PLAYER && gMpCtx != NULL)
	{
		gMpCtx->jer_log(gMpCtx,
			"[mp] player %d got out; car slot %d left standing where it was\n",
			p->id, p->carId);
	}
}

/* A city's name, for logs and menu labels. `city` is a 0..3 index or -1 for the
 * session's own city (the value the wire uses for "the level's own car data"). */
const char* MpCarCityName(int city)
{
	/* JERICHO: ask the ENGINE, do not keep a local copy.
	 *
	 * This was a hardcoded names[4] = CHICAGO/HAVANA/VEGAS/RIO, which is why the
	 * pause menu city picker could not name a Driver 1 car-data city even once
	 * carhacks started offering them: the COUNT already came from carhacks through
	 * MP_CARQ_CITIES, and the NAMES did not, so the picker cycled to a real city
	 * and displayed "the session city" instead.
	 *
	 * LevelNames is the engine's own registry, so it grows when the engine does -
	 * the same reason carselect.c now reads CITY_COUNT rather than a literal 4.
	 * A city the engine has no row for is still reported as the session city,
	 * rather than as a name that does not exist. */

	if (city < 0 || city >= CITY_COUNT || LevelNames[city] == NULL)
		return "the session city";

	return LevelNames[city];
}

/* The local resident SLOT that holds (city, model) on THIS machine, or -1 when
 * no resident slot does. `city` is -1 for the SESSION's own city (a slot whose
 * car data is the level's own) or a 0..3 city index for a cross-city import.
 *
 * A model NUMBER alone is not enough: every city ships CARMODEL_0..12 and the
 * same number is a DIFFERENT vehicle in each, so a number is matched together
 * with the city its data came from (GetCarModelSourceCity). This is what turns
 * a peer's (city, model) into a slot HERE, where the slot numbers may differ. */
int MpResidentSlotForCar(int city, int model)
{
	int levelCity = -1;
	int slot, i;

	if (model < 0)
		return -1;

	/* WHICH CITY INDEX IS THIS LEVEL'S OWN. Needed because the two ends spell "the
	 * level's own city" differently: a model the level ships has
	 * GetCarModelSourceCity(slot) < 0 here, while the wire carries the owner's city by
	 * INDEX. So the host driving its own level's car arrives as (RIO, 8) and the slot to
	 * match has src < 0 - and without this the client resolved nothing, kept its own car
	 * and logged "player 0 drives RIO model 8, but this machine draws ... (theirs is not
	 * loaded here)" for exactly the cars the level DOES hold. */
	for (i = 0; i < 4; i++)
	{
		if (LevelNames[i] != NULL && GameLevel >= 0 && LevelNames[GameLevel] != NULL &&
			strcmp(LevelNames[i], LevelNames[GameLevel]) == 0)
			levelCity = i;
	}

	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		int src;

		if (residentCarModels[slot] != model)
			continue;

		src = GetCarModelSourceCity(slot);

		if (city < 0)
		{
			if (src < 0)
				return slot;
		}
		else if (src == city || (src < 0 && city == levelCity))
		{
			return slot;		/* the same car, spelled two ways */
		}
	}

	return -1;
}

/* The last (city, model) we reported NOT holding for a remote player, so "this
 * machine does not have their car" is one line per CHANGE, not per snapshot --
 * a carstate arrives every frame. `Set` is 0 until we have logged one. */
static int sMpCarKeepCity[MP_MAX_PLAYERS];
static int sMpCarKeepModel[MP_MAX_PLAYERS];
static unsigned char sMpCarKeepSet[MP_MAX_PLAYERS];

/* Make this player's car in OUR world the vehicle their owner just got into.
 *
 * IN PLACE, on the slot we already drive for them -- ONLY the cosmetic model and
 * the colour change, so nothing about this world's car slots is disturbed.
 *
 * Do NOT move the player onto the car the owner NAMED. Slot numbers do not mean
 * the same car on two machines (traffic is not replicated), so doing that warps
 * the player into whatever unrelated car happens to sit at that index here --
 * seen as "the host teleported into the client's old car and the client ended up
 * warped in as a traffic car". The hijacked car is also owned by the LOCAL traffic
 * system, which then tries to recycle it and crashes (PingInCivCar / StepSim).
 * Matching the vehicle is the job here; matching the ENTITY needs replicated
 * traffic, which the session does not have.
 *
 * The owner names the car as (city, model) -- the pair the wire carries -- and
 * THIS machine resolves it to its own resident slot (MpResidentSlotForCar). A
 * slot number is never taken off the wire. Returns 1 if the car was changed, 0
 * when this machine cannot hold it (not resident, or its mesh is not loaded). */
/* The reusable core of a car change: re-model the car in `carSlot` as (city, model).
 *
 * `who` is the roster row doing the changing, or NULL in SINGLE PLAYER, where there
 * is no roster at all -- the pause menu's Change car row is registered
 * unconditionally, so the same row has to work in a solo game. Session-free on
 * purpose: everything session-shaped (the identity bookkeeping, the keep-gate, the
 * per-player log wording) is gated on `who`; everything that decides whether the
 * car CAN be changed is not. Returns 1 when the car was changed. */
static int MpAdoptCar(int carSlot, int city, int model, MP_PLAYER* who)
{
	CAR_DATA* cp;
	int slot;

	if (carSlot < 0 || carSlot >= MAX_CARS)
		return 0;

	cp = &car_data[carSlot];

	/* The resident slot on THIS machine that holds the vehicle the owner named.
	 * This is the whole reason the wire carries a (city, model) and not a slot:
	 * slot numbers do not mean the same car on two machines. */
	slot = MpResidentSlotForCar(city, model);

	/* Only to a slot the renderer actually HAS. Pointing ap.model at a mesh we
	 * never loaded is a crash, not a cosmetic glitch, so an unavailable slot
	 * keeps the old one and says so. */
	if (JerCarSlotUsable(slot))
	{
		int was = cp->ap.model;

		if (was == slot)
			return 0;		/* already that car */

		cp->ap.model = slot;

		/* REBUILD THE MESH. DrawCar draws the slot's poly/UV list against
		 * gTempCarVertDump[carId], and CreateDentableCar is the ONLY thing that fills
		 * that dump. Writing ap.model and stopping there draws one car's polygons
		 * over another car's vertices - that is what a garbled remote car IS.
		 *
		 * This mirrors how the engine makes a car: InitCar sets ap.carCos from the
		 * slot and finishes with CreateDentableCar (civ_ai.c:108-112, :164).
		 * ap.carCos is not cosmetic only - the collision box and the chase camera
		 * read it (bcollide.c, camera.c), so leaving it stale would follow a car with
		 * the wrong box. lowDetail = -1 matches a freshly made car.
		 *
		 * CreateDentableCar also zeroes this car's LOCAL damage (denting.c), which is
		 * expected: the session does not replicate damage. DentCar is NOT called - it
		 * applies the spawn-time "used car" look, which a model swap should not add. */
		cp->ap.carCos = &car_cosmetics[slot];
		cp->lowDetail = -1;

		CreateDentableCar(cp);

		/* Say it: the car was REBUILT, not merely re-pointed. A hot-loaded car also
		 * had its cosmetics replaced the moment they landed (JerHotLoadCarCosmetics),
		 * and this is the call that makes the car on the road use them. */
		if (gMpCtx != NULL)
		{
			if (who != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] rebuilt player %d's car on slot %d (model %d from %s): cosmetics, collision box and mesh are that car's own now"
					" [local=%d localId=%d slot=%d]\n",
					who->id, slot, model, MpCarCityName(city),
					who->isLocal, gMp.localPlayerId, carSlot);
			else
				gMpCtx->jer_log(gMpCtx,
					"[mp] rebuilt the local car on slot %d (model %d from %s): cosmetics, collision box and mesh are that car's own now (single player)\n",
					slot, model, MpCarCityName(city));
		}

		/* The roster row's identity is a model NUMBER now, with its city -- not the
		 * slot we happened to resolve it to. In single player there is no row to
		 * record it on: the car on the road IS the record. */
		if (who != NULL)
		{
			who->car = model;
			who->carIsSlot = 0;
			who->carCity = (city < 0) ? -1 : city;

			if (who->id >= 0 && who->id < MP_MAX_PLAYERS)
				sMpCarKeepSet[who->id] = 0;	/* it holds the car now; re-arm the gate */

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] player %d changed car: slot %d -> %d (%s model %d), mesh rebuilt"
					" [local=%d localId=%d row=%d]\n",
					who->id, was, slot, MpCarCityName(city), model,
					who->isLocal, gMp.localPlayerId, (int)(who - gMp.players));
		}
		else if (gMpCtx != NULL)
		{
			gMpCtx->jer_log(gMpCtx,
				"[mp] changed car (single player): slot %d -> %d (%s model %d), mesh rebuilt\n",
				was, slot, MpCarCityName(city), model);
		}

		return 1;
	}

	/* Cannot hold it: KEEP the car we have and say so ONCE per change, not every
	 * frame -- a carstate arrives every frame, so the repetition is the noise this
	 * avoids. Loading it is the hotload's job (carhacks/MP_ADAPTER.md). */
	if (who != NULL && gMpCtx != NULL && who->id >= 0 && who->id < MP_MAX_PLAYERS &&
		(!sMpCarKeepSet[who->id] || sMpCarKeepCity[who->id] != city || sMpCarKeepModel[who->id] != model))
	{
		sMpCarKeepCity[who->id] = city;
		sMpCarKeepModel[who->id] = model;
		sMpCarKeepSet[who->id] = 1;

		if (slot >= 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d drives %s model %d (slot %d here) but that mesh is not loaded; keeping slot %d\n",
				who->id, MpCarCityName(city), model, slot, cp->ap.model);
		else
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d drives %s model %d, which this machine does not hold (the hotload will load it); keeping slot %d (model %d)\n",
				who->id, MpCarCityName(city), model, cp->ap.model,
				(cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS) ? residentCarModels[cp->ap.model] : -1);
	}
	else if (who == NULL && gMpCtx != NULL)
	{
		/* Single player: a menu action, not a per-frame carstate, so there is no
		 * repetition to suppress -- say it every time. */
		if (slot >= 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): %s model %d resolves to slot %d here but that mesh is not loaded; keeping slot %d\n",
				MpCarCityName(city), model, slot, cp->ap.model);
		else
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): %s model %d is not held by this machine; keeping slot %d (model %d)\n",
				MpCarCityName(city), model, cp->ap.model,
				(cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS) ? residentCarModels[cp->ap.model] : -1);
	}

	return 0;
}

/* A roster row's car, arriving from its owner (or from the host's roster): the
 * session-shaped entry point, which is just the core aimed at that row's car. */
static int MpAdoptRemoteCar(MP_PLAYER* p, int city, int model)
{
	if (p == NULL)
		return 0;

	return MpAdoptCar(p->carId, city, model, p);
}

/* ------------------------------------------------------------------ */
/* Asking the other modules about live cars                            */
/* ------------------------------------------------------------------ */
/* mp_carquery.h is the contract: a custom JERICHO event, so this works whether
 * carhacks is compiled in beside us or loaded as a dll, and degrades to "nobody
 * knows" when it is not there at all. */

/* Which cities this session can offer. Fills `out` with 0..3 city indices and
 * returns how many were written; 0 means nobody answered, and the caller should
 * offer the session's own city alone. */
int MpCarQueryCities(int* out, int max)
{
	MP_CARQ_CITIES_ARGS a;

	if (out == NULL || max <= 0)
		return 0;

	memset(&a, 0, sizeof(a));
	a.cities = out;
	a.max = max;

	jer_fire(MP_CARQ_CITIES, &a);

	if (a.count < 0)
		return 0;

	return (a.count > max) ? max : a.count;
}

/* Ask whoever can to make (city, model) available on THIS machine and in the
 * session (carhacks' import/hotload). The holder decides -- a refusal is reported
 * to the player rather than driven into a model this machine cannot render. */
int MpCarQueryLoad(int city, int model)
{
	MP_CARQ_LOAD_ARGS a;

	memset(&a, 0, sizeof(a));
	a.city = city;
	a.model = model;

	jer_fire(MP_CARQ_LOAD, &a);

	return a.ok ? 1 : 0;
}

/* Which slots may `city` be offered in, and the model in each (mp_carquery.h). */
int MpCarQuerySlots(int city, int* slots, int* models, int max)
{
	MP_CARQ_SLOTS_ARGS a;

	if (slots == NULL || models == NULL || max <= 0)
		return 0;

	memset(&a, 0, sizeof(a));
	a.city = city;
	a.slots = slots;
	a.models = models;
	a.max = max;

	jer_fire(MP_CARQ_SLOTS, &a);

	/* count stays 0 when nobody answered -- "no better answer", not an error: the caller
	 * then keeps the list it already had. */
	if (a.count < 0)
		return 0;

	return (a.count > max) ? max : a.count;
}

/* The car list a city may be offered: what the car mods say, or failing that the engine's
 * own frontend table - see mp.h for why that table is the wrong question IN A SESSION. */
int MpCarListForCity(int city, int* slots, int* models, int max)
{
	extern int CarAvailability[CITY_COUNT][10];    /* grown with the registry */
	extern char carNumLookup[CITY_COUNT][10];
	int n, slot;

	if (slots == NULL || models == NULL || max <= 0)
		return 0;

	if (city < 0 || city >= CITY_COUNT)
		city = 0;

	n = MpCarQuerySlots(city, slots, models, max);

	if (n > 0)
		return n;

	/* Nobody knew better: the frontend table, exactly as this picker read it before there
	 * was anything to ask. */
	n = 0;

	for (slot = 0; slot < 10 && n < max; slot++)
	{
		if (CarAvailability[city][slot] == 0)
			continue;

		slots[n] = slot;
		models[n] = (int)(unsigned char)carNumLookup[city][slot];
		n++;
	}

	return n;
}

/* Tell whoever keeps the session's car identity (carhacks) how a switch ended: the local
 * player now drives (city, model) - city -1 for the level's own car - or (changed = 0) the
 * switch to it did not happen. A notice, not a question: nobody answering is fine. This is
 * what lets the identity move only after a SUCCESSFUL switch, and the car the player left
 * be given back (carhacks/net.c, chkNetReleaseSlotIfUnused). */
void MpCarQueryChosen(int city, int model, int changed)
{
	MP_CARQ_CHOSEN_ARGS a;

	if (model < 0)
		return;

	memset(&a, 0, sizeof(a));
	a.city = city;
	a.model = model;
	a.changed = changed ? 1 : 0;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] car chosen: %s model %d (%s)\n",
			MpCarCityName(city), model, changed ? "driving it now" : "the switch did not happen");

	jer_fire(MP_CARQ_CHOSEN, &a);
}

/* Change OUR OWN car, mid-match, from the pause menu.
 *
 * Replace the vehicle we are driving with another one -- the "Change car" row in
 * the Multiplayer pause page (mp.c), which is also the path a carhacks live pick
 * drives. On foot the player is put into their parked car first: "change car" from
 * the pavement has to produce a car, not a new pedestrian.
 *
 * THE SLOT IS STILL OUR OWN and only the cosmetic model changes, through the SAME
 * function a peer's car goes through (MpAdoptRemoteCar) -- so nothing about this
 * world's car slots, and so nothing about anyone else's car, is disturbed, and a
 * change we make and a change a peer makes read identically in the log.
 *
 * The choice then travels the ordinary way: our carstate carries the new
 * (city, model) from this frame on, so every peer re-models its copy of us, and a
 * client also tells the host so the host's roster names the car it is really
 * driving. Whether a machine HOLDS the car is not this function's business -- a
 * car the level does not carry is what the carhacks import is for. Returns 1 if
 * the car on the road changed (0 when we were already driving it, or there was
 * nothing to change). */
/* Why a pick could not be applied, in the player's own words.
 *
 * One place, so the on-screen notice, the log and the picker's filtering all
 * describe the SAME rule: this machine does not hold the car / it holds a slot with
 * no mesh / the car is already the one being driven. (MpAdoptCar returns 0 for the
 * last case too, which is not a refusal at all.) */
static const char* MpChangeCarRefusal(int city, int model)
{
	int slot = MpResidentSlotForCar(city, model);

	if (slot < 0)
		return "this machine does not hold that car";

	if (JerCarSlotUsable(slot))
		return "you are already driving it";

	return JerCarSlotRefusal(slot);
}

int MpChangeCar(int city, int model)
{
	MP_PLAYER* me = MpLocalPlayer();
	int changed;

	if (model < 0)
		return 0;

	/* SINGLE PLAYER. The pause menu's Change car row is registered unconditionally,
	 * so in a solo game its Apply row arrives HERE, with gMp.running false and no
	 * roster in existence. A change is then a purely local re-model of the car the
	 * local player is driving: the same picker and the same "does this machine hold
	 * it" rule as a session, with nobody to publish to. A refused pick (a car this
	 * level has no mesh for) leaves the car alone and says so, exactly as it does in
	 * a session. */
	if (!gMp.running)
	{
		int slot = MainPlayer.playerCarId;

		if (slot < 0 || slot >= MAX_CARS || MainPlayer.playerType != PLAYER_TYPE_CAR)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] change car (single player): not driving (playerCarId %d, type %d) - nothing to change\n",
					slot, (int)MainPlayer.playerType);

			MpNotify("Change car: get in a car first");
			return 0;
		}

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): we asked for %s model %d (slot %d)\n",
				MpCarCityName(city), model, slot);

		/* NOT A CAR THIS MACHINE HOLDS YET? Ask for it, exactly as the session path
		 * does below. The picker offers a city's whole roster, so choosing one IS the
		 * request that imports it -- without this a cross-city or Driver 1 pick could
		 * be selected and then always refused, and the roster would be a lie. */
		if (MpResidentSlotForCar(city, model) < 0)
			MpCarQueryLoad(city, model);

		changed = MpAdoptCar(slot, city, model, NULL);

		if (changed)
			MpNotifyf("Changed car: %s model %d", MpCarCityName(city), model);
		else
			MpNotifyf("Change car refused: %s model %d - %s", MpCarCityName(city), model,
				MpChangeCarRefusal(city, model));

		return changed;
	}

	if (me == NULL)
		return 0;

	if (me->carId < 0)
	{
		int seat = -1;
		int s;

		/* Get back into a car OF OURS -- one the module made, left parked and
		 * enterable (see MpFollowLocalCar). Never a live traffic car: taking one
		 * over and vacating it hands the traffic AI a car it cannot steer, which
		 * is the crash that note exists to prevent. */
		for (s = 0; s < MAX_CARS; s++)
		{
			if (!gMpOurCars[s] || MpGetPlayerByCar(s) != NULL)
				continue;

			if (car_data[s].controlType == CONTROL_TYPE_CIV_AI)
			{
				seat = s;
				break;
			}
		}

		if (seat < 0)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] change car: on foot and no car of ours to get back into\n");

			MpNotify("Change car: you are on foot with no car to change");
			return 0;
		}

		/* The pedestrian is a real object, not just the player's pointer to it: the
		 * engine's own get-in path (PedGetInCar -> ChangePedPlayerToCar + DestroyPedestrian
		 * + the Tanner counter) removes it, and calling only ChangePedPlayerToCar leaves
		 * the Tanner standing exactly where we got out - "you put me back in the car but
		 * did not remove my Tanner". */
		{
			LPPEDESTRIAN ped = player[0].pPed;

			ChangePedPlayerToCar(0, &car_data[seat]);
			RemovePlayerPedestrian(ped);

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] change car: got back into slot %d and put our pedestrian away (%s)\n",
					seat, (ped != NULL) ? "removed" : "none");
		}

		/* Adopt the move NOW, so the re-model below lands on the car we just got
		 * into instead of waiting a frame for the poll in MpSendOwnCarState. */
		MpFollowLocalCar();
	}

	if (me->carId < 0 || me->carId >= MAX_CARS)
		return 0;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] change car: we asked for %s model %d (slot %d)\n",
			MpCarCityName(city), model, me->carId);

	/* Not a car this machine holds? Ask whoever can to make it available (carhacks'
	 * import + hotload), which is also what tells the other machines to fold it in.
	 * Only THEN try the swap: pointing the car at a mesh that is not loaded is a
	 * crash, not a cosmetic glitch (see MpAdoptRemoteCar). */
	if (MpResidentSlotForCar(city, model) < 0)
		MpCarQueryLoad(city, model);

	changed = MpAdoptRemoteCar(me, city, model);

	/* Report the OUTCOME (MP_CARQ_CHOSEN). "Driving it" is read off the car on the road, so
	 * "already that car" counts as driving it, and the city is the one the slot's data came
	 * from (-1 = the level's own car) - the identity the other machines must draw. */
	{
		int want = MpResidentSlotForCar(city, model);
		int onIt = (want >= 0 && me->carId >= 0 && me->carId < MAX_CARS &&
			car_data[me->carId].ap.model == want);

		MpCarQueryChosen(onIt ? GetCarModelSourceCity(want) : city, model, onIt);
	}

	/* Publish the choice. A client's pick goes to the HOST alone, which
	 * republishes the roster naming it; the host does that itself. Our carstate
	 * carries the model either way. */
	if (MpIsHost())
		MpHostSendRoster();
	else
		MpSendPickedCar(model, city);

	if (changed)
		MpNotifyf("Changed car: %s model %d", MpCarCityName(city), model);
	else
		MpNotifyf("Change car refused: %s model %d - %s", MpCarCityName(city), model,
			MpChangeCarRefusal(city, model));

	/* THE INVISIBLE-CAR INSTRUMENT.
	 *
	 * A player reported cars "loading invisible" after cycling a few, and no run has
	 * reproduced it: the lever changes 12 cars across all four cities with every switch
	 * succeeding, the previous slot released each time (7/8 alternating) and the pool flat.
	 * So rather than guess at a fix, this states the ONE thing that makes a car invisible -
	 * ap.model pointing at a resident model whose mesh is not built - in the log of the
	 * change itself. DrawCar returns without drawing when gCarCleanModelPtr[] is NULL, so
	 * that NULL is exactly the invisible car, and a report can name the state rather than
	 * the symptom. Logged once per change, not per frame. */
	if (gMpCtx != NULL && me != NULL && me->carId >= 0 && me->carId < MAX_CARS)
	{
		CAR_DATA* cp = &car_data[me->carId];
		int slot = cp->ap.model;

		if (slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS)
		{
			int built = JerCarSlotUsable(slot);

			gMpCtx->jer_log(gMpCtx,
				"[mp] car status: driving model %d (resident %d, source %s); mesh %s\n",
				(int)residentCarModels[slot], slot, MpCarCityName(GetCarModelSourceCity(slot)),
				built ? "loaded" : "MISSING - this car is not drawn");
		}
		else
		{
			gMpCtx->jer_log(gMpCtx,
				"[mp] car status: driving resident %d, which is out of range - this car is not drawn\n",
				slot);
		}
	}

	return changed;
}

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

			if (car_data[s].controlType == CONTROL_TYPE_CIV_AI)
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

		start[0] = PlayerStartInfo[0]->position.vx;
		start[1] = 0;		/* the engine resolves the ground under the car */
		start[2] = PlayerStartInfo[0]->position.vz;
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

/* IN A SESSION, DYING IS A RESPAWN, NOT A GAME OVER.
 *
 * The engine asks (JER_PAUSE_GAMEOVER) just before it arms the game-over pause - the death
 * fade (gDieWithFade, set when the car drops below y = -1000) reaching its end, or a mission
 * asking for it. Answering "handled" IS the refusal: the engine leaves the pause unarmed and
 * drops the fade instead of blackening the screen, and the player is put back on the map
 * HERE, so by the time the engine asks the car is no longer dying.
 *
 * The placement is the engine's own (InitCarPhysics: the rigid-body quaternion and the
 * fixed-point fposition, NOT hd.where) for the reason the soft restart above learned the hard
 * way - writing hd.where alone leaves the body where it was, at the roll it had, and the next
 * physics step snaps the car back onto it. The camera is re-seated with it, because
 * cameraAngle is an absolute world yaw.
 *
 * The car goes back WHERE IT IS when that is still on the map, and to the level's own start
 * when it is not: "in place" cannot mean the void, and MapHeight answering with a sane height
 * is what says which of the two this is.
 *
 * Returns 1 when the game over was refused, 0 to let the engine have it - no session, or a
 * player with no car (on foot the engine's death test never fires, so a game over there is
 * not this one).
 */
int MpRespawnAfterDeath(void)
{
	MP_PLAYER* me;
	CAR_DATA* cp;
	LONGVECTOR4 start;
	VECTOR probe;
	int ground;
	int x, z, dir;

	if (!gMp.running)
		return 0;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return 0;

	cp = &car_data[me->carId];

	/* Placed on EVERY ask, because "the engine wants a game over" IS the dying signal - the
	 * car's own y is not: the engine can already have clamped where.t[1] back to the ground
	 * while the fade and the mission's game-over delay are still counting. What keeps this
	 * from running per frame is the ENGINE: it clears Mission.gameover_delay when we refuse
	 * (see JerGameOverRefused), so it asks once per death, not once per frame. */
	x = cp->hd.where.t[0];
	z = cp->hd.where.t[2];
	dir = cp->hd.direction;

	probe.vx = x;
	probe.vy = 0;
	probe.vz = z;
	probe.pad = 0;

	/* A sane ground height means this x/z is on the map. Anything else is the void. */
	ground = MapHeight(&probe);

	if (ground <= -1000)
	{
		if (PlayerStartInfo[0] == NULL)
			return 0;			/* nothing sane to fall back to - let the engine end it */

		x = PlayerStartInfo[0]->position.vx;
		z = PlayerStartInfo[0]->position.vz;
		dir = PlayerStartInfo[0]->rotation;
	}

	start[0] = x;
	start[1] = 0;				/* the engine resolves the ground under the car */
	start[2] = z;
	start[3] = 0;

	InitCarPhysics(cp, &start, dir);

	cp->hd.speed = 0;
	cp->wheel_angle = 0;

	/* REPAIRED, and handed back whole: totalDamage alone leaves the car looking caved in,
	 * because the DRAWN vertices only come back from the clean model through
	 * CreateDentableCar (the same reason the restart above needs it). */
	cp->totalDamage = 0;
	memset(cp->ap.damage, 0, sizeof(cp->ap.damage));
	CreateDentableCar(cp);
	cp->ap.needsDenting = 0;

	/* NOT DYING ANY MORE, and NOT WANTED. Repairing the CAR is not enough: the mission's
	 * death route counts the player down through tannerDeathTimer -> player[].dying, and
	 * BOTH the "your vehicle's wrecked" banner and the playersdead game over hang off that
	 * counter -- so with it left standing the game kept saying the car was wrecked and kept
	 * re-arming the game over we had just refused (which is why a refusal without this
	 * respawned a few times before it settled, instead of once). */
	tannerDeathTimer = 0;
	player[0].dying = 0;
	player[0].upsideDown = 0;

	/* NO FELONY either - a forced respawn is a fresh start, not a wanted level carried
	 * over. GetPlayerFelony picks the car's rating or the pedestrian's, so both go, or
	 * stepping out puts the cops straight back on us. */
	cp->felonyRating = 0;
	pedestrianFelony = 0;

	player[0].cameraAngle = cp->hd.direction + 1536;
	player[0].cameraCarId = cp->id;

	{
		JER_ARGS_RESET_CAR rc;

		rc.carId = me->carId;
		jer_fire(JER_EVENT_RESET_CAR, &rc);
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] death in a session: respawned at %d,%d (ground=%d) in slot %d - game over refused\n",
			cp->hd.where.t[0], cp->hd.where.t[2], ground, me->carId);

	return 1;
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

			/* ...AND mark the PLAYER as on foot.
			 *
			 * Releasing the car was not enough, and this is why neither machine
			 * could see the other on foot. Everything downstream keys off pl->carId
			 * -- not least MpDriveRemotePed, which refuses to stand a pedestrian in
			 * for a player while pl->carId >= 0. So the car went back to the world,
			 * the pedestrian was never created, and since BOTH ends made the same
			 * mistake it failed symmetrically rather than one way.
			 *
			 * carId is otherwise only assigned in the spawn paths and for our OWN
			 * player, so a peer who got out kept pointing at the car they had just
			 * left. The pose adopt below already skips a player with carId < 0,
			 * which is exactly right for someone who is on foot. */
			pl->carId = -1;
			pl->car = -1;
			pl->onFoot = 1;

			continue;
		}

		if (pl->carId < 0 || pl->carId >= MAX_CARS)
			continue;

		cp = &car_data[pl->carId];

		/* The owner changed VEHICLE: match it BEFORE adopting this pose, so the
		 * body we are about to write lands on the right car.
		 *
		 * The wire carries a model NUMBER plus its city (it used to carry our
		 * resident SLOT, which only worked while both machines' resident tables
		 * happened to agree). Resolve it to OUR slot and compare what this car
		 * ACTUALLY renders with -- not pl->car: the roster refresh (every 120
		 * frames) writes the owner's new model into pl->car long before the car
		 * itself is changed, so gating on pl->car silently agreed with the roster
		 * while the car on screen stayed the old one. */
		{
			int wantCity = (e.modelCity == MP_CAR_CITY_SESSION) ? -1 : (int)e.modelCity;
			int haveSlot = cp->ap.model;
			int haveModel = (haveSlot >= 0 && haveSlot < MAX_CAR_RESIDENT_MODELS) ? residentCarModels[haveSlot] : -1;
			int haveCity = (haveSlot >= 0 && haveSlot < MAX_CAR_RESIDENT_MODELS) ? GetCarModelSourceCity(haveSlot) : -2;

			if (haveModel != (int)e.model || haveCity != wantCity)
			{
				/* MP_DEBUG: the wire (model, city) against what we render, with the
				 * slot it resolves to HERE. A mismatch that fires every frame is the
				 * signature of a car this machine cannot hold. Throttled to once a
				 * second, because the repetition IS the signature. */
				if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % 30) == 0)
					gMpCtx->jer_log(gMpCtx,
						"[mp] MODEL: player %d drives %s model %d on the wire; we render slot %d "
						"(model %d, src %d, mesh %s); this machine holds it in slot %d\n",
						pl->id, MpCarCityName(wantCity), (int)e.model, haveSlot, haveModel, haveCity,
						JerCarSlotUsable(haveSlot) ? "present" : "NULL",
						MpResidentSlotForCar(wantCity, (int)e.model));

				if (MpAdoptRemoteCar(pl, wantCity, (int)e.model))
				{
					if (pl->carId < 0 || pl->carId >= MAX_CARS)
						continue;

					cp = &car_data[pl->carId];
				}
			}
		}

		/* OWNER-AUTHORITATIVE: this car belongs to another machine, so its owner is
		 * the truth. Adopt the WHOLE body every snapshot -- no easing, no tolerance.
		 * A contact is a separate message (MP_HIT: the owner of the MOVING car
		 * pushes its own), so there is no push to leave room for here. */
		{
			/* The HOST<->CLIENT deviation for this REMOTE car: how far our own
			 * simulation of it is from its owner's snapshot. One line per snapshot
			 * that arrives, for every remote car. A small steady value is input
			 * latency; a growing one is the two simulations drifting apart; a
			 * spiky one is the resync fighting the engine. This is the number to
			 * watch for "the cars are not where they are on the other machine".
			 * (|d| is the world-unit distance, dh the heading error in 1/4096
			 * turns.) Logged on arrival, NOT gated on our own frame counter: the
			 * peer's snapshot arrives on ITS cadence, which need not line up with
			 * ours -- gating on `frame % 30` silently logged nothing.
			 *
			 * It IS MP_DEBUG-gated, and that matters: this is a sqrt plus a
			 * 10-field format per remote car per snapshot, and a shipped run
			 * (packaged launchers do not set MP_DEBUG) must not pay for it. The
			 * math lives inside the guard so a production frame does none of it. */
			if (MpDebugOn() && gMpCtx != NULL)
			{
				long dx = (long)e.x - (long)cp->hd.where.t[0];
				long dy = (long)e.y - (long)cp->hd.where.t[1];
				long dz = (long)e.z - (long)cp->hd.where.t[2];
				long d2 = dx * dx + dy * dy + dz * dz;
				int dh = ((e.heading - cp->hd.direction + 2048) & 4095) - 2048;

				gMpCtx->jer_log(gMpCtx,
					"[mp] adopt: player %d snap %u |d|=%ld d2=%ld (dx=%ld dy=%ld dz=%ld) dh=%d pal=%d md=%d\n",
					e.playerId, (unsigned)h.frame, (long)sqrt((double)d2), d2, dx, dy, dz, dh, (int)e.palette, (int)e.model);
			}

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
						cp->st.n.angularVelocity[i] = MpClampAngVel(e.angVel[i]);

					LongQuaternion2Matrix(&q, &m);
					m.t[0] = tx;
					m.t[1] = ty;
					m.t[2] = tz;
					memcpy(&cp->hd.where, &m, sizeof(m));
				}

			}
		}

		cp->hd.direction = e.heading;
		pl->onFoot = 0;			/* driving again: the roster may seat them */
		pl->lastStateFrame = gMp.frame;	/* the fallback gate in MpOnNetInput reads this */

		/* The OWNER is the colour authority: paint its car the colour the owner
		 * sees. cp->ap.palette is exactly what the renderer hands to
		 * DrawCarObject, so this recolours the car on the very next frame.
		 *
		 * A palette index only means something on a car that IS the owner's vehicle,
		 * and this wire carries a resident SLOT, not a (city, model) - so let the
		 * modules answer whether this machine really holds this player's car. A car
		 * that only looks like the owner's gets the palette it can actually support,
		 * instead of colours that belong to a vehicle that is not here */
		if (!pl->isLocal)
		{
			int pal = (int)e.palette;
			int corrected;
			JER_ARGS_CAR_PEER_DRAW draw;

			memset(&draw, 0, sizeof(draw));
			draw.player = pl->id;
			draw.car = cp;
			draw.model = cp->ap.model;
			draw.sourceCity = GetCarModelSourceCity(cp->ap.model);
			draw.paletteIn = (int)e.palette;
			draw.paletteOut = pal;

			jer_fire(JER_EVENT_CAR_PEER_DRAW, &draw);

			if (draw.handled && draw.paletteOut >= 0)
				pal = draw.paletteOut;

			/* Log a correction when the car's palette actually changes, and once per
			 * reported value when it does not (the state stream is continuous, so an
			 * unthrottled line here would repeat every packet). */
			corrected = draw.handled && (pal != (int)e.palette);

			if (corrected || cp->ap.palette != (u_char)pal)
			{
				int fresh = corrected && pl->id >= 0 && pl->id < MP_MAX_PLAYERS &&
					(gPaletteLogged[pl->id] != (signed char)e.palette);

				if (fresh)
					gPaletteLogged[pl->id] = (signed char)e.palette;

				if ((cp->ap.palette != (u_char)pal || fresh) && gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] palette: player %d reports %d, drawn as %d%s\n",
						pl->id, (int)e.palette, pal,
						corrected ? " (corrected: not their car here)" : "");

				cp->ap.palette = (u_char)pal;
			}
		}

		/* Client-side gather: the first time we hear a peer's car, drop our
		 * own car right next to it so both players start together -- each
		 * machine's take-a-ride spawn point can be right across the map. */
		if (!MpIsHost() && !gMp.localPlaced)
		{
			MP_PLAYER* me = MpLocalPlayer();

			if (me != NULL && me->carId >= 0 && me->carId < MAX_CARS && me->carId != pl->carId)
			{
				CAR_DATA* my = &car_data[me->carId];
				MATRIX m;
				int gy;

				/* OUR x/z (the peer's spot offset along X) -- but the HEIGHT is
				 * resolved from the ground UNDER where we land, never carried from
				 * the peer's y 600 units away. A y taken at one x/z and applied at
				 * another is the single-Y trap (ARCHITECTURE §4): the car ends up
				 * above or below the road and the engine has to drag it down. This
				 * mirrors the engine's own "place a car at a position" recipe
				 * (civ_ai.c: MapHeight at the target minus the suspension offset). */
				my->hd.where.t[0] = e.x + 600;
				my->hd.where.t[1] = 0;
				my->hd.where.t[2] = e.z;
				my->hd.direction = e.heading;

				gy = MapHeight((VECTOR*)my->hd.where.t);
				if (my->ap.carCos != NULL)
					gy -= my->ap.carCos->wheelDisp[0].vy;
				my->hd.where.t[1] = gy;

				/* Rebuild the handling matrix too: setting only t[]/direction left the
				 * collision box at the OLD spot until the engine next recomputed it --
				 * and this is a teleport, so it matters. */
				_RotMatrixY(&m, (short)e.heading);
				memcpy(my->hd.where.m, m.m, sizeof(my->hd.where.m));

				gMp.localPlaced = 1;

				if (gMpCtx)
					gMpCtx->jer_log(gMpCtx,
						"[mp] gathered next to peer at %d,%d,%d (ground under us, not the peer's y)\n",
						my->hd.where.t[0], my->hd.where.t[1], my->hd.where.t[2]);
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

static int MpCreateTrafficMirror(int slot, int city, int model, int palette)
{
	CAR_DATA* cp;
	EXTRA_CIV_DATA civDat;
	LONGVECTOR4 pos;
	int resident;

	if (slot < 0 || slot >= MAX_CARS)
		return 0;

	resident = MpResidentSlotForCar(city, model);

	/* Never point ap.model at a mesh this machine does not have (DrawCar would read
	 * it) -- a car we cannot build is simply left out. */
	if (!JerCarSlotUsable(resident))
		return 0;

	cp = &car_data[slot];

	memset(&civDat, 0, sizeof(civDat));
	civDat.distAlongSegment = -5;

	memset(&pos, 0, sizeof(pos));

	InitCar(cp, 0, &pos, CONTROL_TYPE_CIV_AI, resident, palette, (char*)&civDat);

	/* NOW it is the peer's car, driven by the peer: our AI must not touch it and
	 * nothing of ours may read a pad through it. */
	cp->controlType = CONTROL_TYPE_PLAYER;
	cp->ai.padid = &gMpQuietPad;
	cp->hndType = 0;
	cp->ap.palette = palette;
	cp->lowDetail = -1;

	gMpPeerTraffic[slot] = 1;

	return 1;
}

/* Drop a mirror. Deliberately NOT PingOutCar: that decrements the civil counters
 * only for a CIV_AI car and REFUSES to remove a non-CIV_AI one while PingOutCivsOnly
 * is set (civ_ai.c), which the engine sets every frame. A mirror is a plain
 * CONTROL_TYPE_PLAYER car of ours, so it is taken out of the world directly: the
 * slot is left reserved against our own spawner, so nothing reuses it. */
static void MpRemoveTrafficMirror(int slot)
{
	if (slot < 0 || slot >= MAX_CARS || !gMpPeerTraffic[slot])
		return;

	car_data[slot].controlType = CONTROL_TYPE_NONE;
	gMpPeerTraffic[slot] = 0;
}

/* Is `slot` already the mirror of (city,model)? */
static int MpTrafficMirrorMatches(int slot, int city, int model)
{
	int resident;

	if (slot < 0 || slot >= MAX_CARS)
		return 0;

	if (!gMpPeerTraffic[slot] || car_data[slot].controlType == CONTROL_TYPE_NONE)
		return 0;

	resident = MpResidentSlotForCar(city, model);

	return resident >= 0 && car_data[slot].ap.model == resident;
}

/* Write a row's whole body onto the mirror, the same way a player car's snapshot
 * is written (hd.direction is an OUTPUT the engine re-derives, so the attitude
 * comes from the quaternion). */
static void MpApplyTrafficPose(CAR_DATA* cp, const MP_TRAFFIC_ENTRY* e)
{
	LONGQUATERNION q;
	MATRIX m;
	int i;

	for (i = 0; i < 4; i++)
		q[i] = e->orient[i];

	for (i = 0; i < 4; i++)
		cp->st.n.orientation[i] = q[i];
	for (i = 0; i < 3; i++)
		cp->st.n.linearVelocity[i] = e->vel[i];

	LongQuaternion2Matrix(&q, &m);
	m.t[0] = e->x;
	m.t[1] = e->y;
	m.t[2] = e->z;
	memcpy(&cp->hd.where, &m, sizeof(m));

	cp->hd.where.t[0] = e->x;
	cp->hd.where.t[1] = e->y;
	cp->hd.where.t[2] = e->z;
	cp->hd.direction = e->heading;
}

/* Damage parity. The owner is the authority, so the mirror copies its zone damage
 * and health. ap.damage[] is exactly what the deformation pass reads --
 * DentCarDirectional deforms from those values alone (its collision point argument
 * is unused) -- so a change is re-dented here and the mesh follows without any
 * vertices crossing the wire. */
static void MpApplyTrafficDamage(CAR_DATA* cp, const MP_TRAFFIC_ENTRY* e)
{
	VECTOR p;
	int z, changed = 0;

	for (z = 0; z < 6; z++)
	{
		if (cp->ap.damage[z] != e->damage[z])
		{
			cp->ap.damage[z] = e->damage[z];
			changed = 1;
		}
	}

	if (cp->totalDamage != e->totalDamage)
	{
		cp->totalDamage = e->totalDamage;
		changed = 1;
	}

	if (changed)
	{
		p.vx = e->x;
		p.vy = e->y;
		p.vz = e->z;
		DentCarDirectional(cp, p);

		if (MpDebugOn() && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] traffic mirror slot %d re-dented (totalDamage %u)\n",
				cp->id, (unsigned)cp->totalDamage);
	}
}

/* Take every mirror out of the world (session teardown). */
static void MpTrafficMirrorsClear(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
		MpRemoveTrafficMirror(i);

	memset(gMpTrafficSent, 0, sizeof(gMpTrafficSent));
}

/* A mirror the owner has stopped speaking about is gone: the owning machine sends
 * its whole band every frame, so silence means the car left the world (or the peer
 * dropped). The explicit REMOVE row is the fast path; this is the backstop that
 * keeps a lost packet from leaving a ghost car standing forever. */
#define MP_TRAFFIC_STALE_FRAMES	60	/* ~2 s at 30 fps */

static void MpTrafficMirrorAgeTick(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (!gMpPeerTraffic[i])
			continue;

		if ((unsigned)(gMp.frame - gMpPeerTrafficSeen[i]) > MP_TRAFFIC_STALE_FRAMES)
			MpRemoveTrafficMirror(i);
	}
}

static void MpHandleTraffic(int connIndex, const unsigned char* p, int len)
{
	const MP_TRAFFIC* h;
	const MP_TRAFFIC_ENTRY* e;
	int i, n;

	if (len < (int)sizeof(MP_TRAFFIC))
		return;

	h = (const MP_TRAFFIC*)p;
	n = h->count;

	if (n > MAX_CARS)
		n = MAX_CARS;

	if (len < (int)(sizeof(MP_TRAFFIC) + n * sizeof(MP_TRAFFIC_ENTRY)))
		n = (len - (int)sizeof(MP_TRAFFIC)) / (int)sizeof(MP_TRAFFIC_ENTRY);

	e = (const MP_TRAFFIC_ENTRY*)(p + sizeof(MP_TRAFFIC));

	for (i = 0; i < n; i++)
	{
		int slot = e[i].carSlot;
		/* The wire spells "the level's own city" as MP_CAR_CITY_SESSION; the resolver
		 * wants -1 for it (see MpHandleCarState). */
		int wantCity = (e[i].modelCity == MP_CAR_CITY_SESSION) ? -1 : (int)e[i].modelCity;

		if (slot < 0 || slot >= MAX_CARS)
			continue;

		if (e[i].flags & MP_TRAFFIC_REMOVE)
		{
			MpRemoveTrafficMirror(slot);
			continue;
		}

		if (!MpTrafficMirrorMatches(slot, wantCity, e[i].model))
		{
			MpRemoveTrafficMirror(slot);

			if (!MpCreateTrafficMirror(slot, wantCity, e[i].model, e[i].palette))
				continue;	/* cannot build their car here: leave the slot empty */
		}

		MpApplyTrafficPose(&car_data[slot], &e[i]);
		MpApplyTrafficDamage(&car_data[slot], &e[i]);
		gMpPeerTrafficSeen[slot] = gMp.frame;	/* seen now: do not age it out */
	}

	/* The host is the hub: relay a client's traffic to the other clients, exactly
	 * as it relays their carstate. */
	if (MpIsHost() && connIndex >= 0)
		MpHostRelay(connIndex, MP_TAG_TRAFFIC, 0, p, len);
}

void MpHandleMessage(int connIndex, const char* tag, const unsigned char* payload, int len)
{
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
		MpHandleInput(payload, len);
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

	if (memcmp(tag, MP_TAG_COLOR, 4) == 0)
	{
		MpHandleColor(connIndex, payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_CHAT, 4) == 0)
	{
		MpHandleChat(payload, len);
		return;
	}

	if (memcmp(tag, MP_TAG_LEAVE, 4) == 0)
	{
		MpHandleLeave(connIndex, payload, len);
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

	/* No protocol surface sends a bare 'JPSS' (the launch config rides in
	 * 'JPST'; MP_TAG_SESSION / MP_SESSION_LOBBY are reserved); unknown tags
	 * are ignored. */
}
