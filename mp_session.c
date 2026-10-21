/*
 * mp_session.c -- JERICHO Multiplayer session: bring-up, the join handshake
 * (with the mod-match lobby policy), the per-frame owner-authoritative car
 * sync and input replication, the roster, the on-foot stand-ins, chat, and the
 * send/recv dispatch that the transport (mp_net.c) calls into.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_notify.h"	/* the join toast */
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
#include "gamesnd.h"	/* CarHasSiren: whether a model has a siren, and its bank/sample */
#include "sound.h"		/* Start3DSoundVolPitch: the engine's own 3D sound call */
#include "jer_sound.h"	/* jer_sound_ensure: a voice a module may keep (never starving SFX) */
#include "system.h"	/* CITY_COUNT and LevelNames: the city registry MpCarCityName reads */
#include "denting.h"	/* CreateDentableCar: the only writer of the drawn vertex dump */
#include "civ_ai.h"	/* reservedSlots: the array PingInCivCar's free-slot scan reads */
#include "cop_ai.h"	/* requestCopCar: a cop is waiting for a slot (the cop slot hold) */
#include "felony.h"	/* felonyRating / pedestrianFelony: what a restart clears */
extern int gBootMpLevel;	/* main.c: 1 = the small multiplayer map, 0 = the full city */
extern int gBootMpArena;	/* main.c: which multiplayer map (0/1) */
extern int gWantNight;		/* glaunch.c: 1 = the night take-a-ride level variant */
#include "players.h"	/* InitPlayer: a late joiner needs a car the same way the engine makes one */
#include "handling.h"	/* LongQuaternion2Matrix: rebuild a car's matrix from its body */
#include "state.h"
#include "objcoll.h"	/* CellEmpty: the engine's own scenery test (the warp's landing check) */
#include "dr2roads.h"	/* JerRoadAt: the engine's own road network (prefer a road to land on) */
#include "dr2roads.h"	/* FindSurfaceD2: the real ground height at a point */
#include "jer_map.h"	/* jer_map_*: the region maths behind "is the ground there?" */
#include "map.h"	/* regions_across/down, MAP_REGION_SIZE, units_across_halved: map-wide join spawn */
#include "pedest.h"	/* pUsedPeds: our player's own pedestrian, when on foot */
#include "models.h"	/* JerPedImport*: the cross-level Jericho model/texture import */
#include "motion_c.h"	/* SetSkelModelPointers: the character switch */
#include "jer_npc.h"	/* jer_npc_*: the pedestrian we stand in for a REMOTE one */
#include "camera.h"	/* InitCamera: snap the camera with a placed late joiner */
#include "jer_ped_palette.h"	/* jer_ped_palette_reset: scope the suit-colour cache to a session */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>	/* getenv: the MP_TEST_JERICHO dev lever */
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
static unsigned gMpPeerTrafficSiren[MAX_CARS];	/* frame we last heard it was on duty (MP_TRAFFIC_SIREN) */
static int gMpMirrorSirenChannel = -1;		/* our locked voice for a peer's siren loop */
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

/* Hit-path instrumentation, session state like the counters above. These answer
 * "how often does the hand-off fire, and how often does a push arrive when the
 * two cars are already far apart on the receiver's screen" -- the measurement the
 * latency work is judged by. Increments are unconditional and nearly free (a few
 * per hit); only the SUMMARY line is MP_DEBUG-gated.
 *
 * MpSessionReset runs before these would otherwise be declared if they lived with
 * the collision code, so they belong here rather than beside MpHandleHit. */
static unsigned long gMpHitSent;		/* pushes we reported */
static unsigned long gMpHitRx;			/* pushes addressed to us */
static unsigned long gMpHitApplied;		/* ... applied (our engine did not have it) */
static unsigned long gMpHitKept;		/* ... dropped as the engine's duplicate */
static unsigned long gMpHitSeparated;		/* ... arrived with the cars > MP_HIT_RADIUS apart */
static unsigned long gMpHitRejected;		/* ... dropped: stale, unvalidatable or ghost */

/* The remote snapshot ring lives here, with the other session state, because
 * both the contact sender (MpContactPush) and the contact receiver
 * (MpHandleHit/MpHitFrame above the playout code in file order) read it. */
#define MP_INTERP_RING		32	/* samples kept per remote player (about 1 s) */

typedef struct MP_CAR_SNAP
{
	unsigned frame;		/* the owner's carstate frame */
	unsigned recvFrame;	/* local sim frame it arrived on (diagnostics) */
	int x, y, z;
	int heading;
	short orient[4];
	int vel[3];
	int angVel[3];
} MP_CAR_SNAP;

typedef struct MP_CAR_PLAY
{
	MP_CAR_SNAP snap[MP_INTERP_RING];
	int count;		/* valid samples; newest is at head */
	int head;
	int primed;
	int playFrame;		/* owner frame currently being displayed */
	int lastFrame;		/* newest received owner frame */
	int mode;		/* 0 = lerp, 1 = hold, 2 = extrapolate (diagnostics) */
	int playX, playY, playZ;	/* the pose actually written this frame */
} MP_CAR_PLAY;

static MP_CAR_PLAY gMpPlay[MP_MAX_PLAYERS];

/* defined below, needed by the launch path above them */
static int MpAssignedCarModel(int playerId);
static int MpPlayerCarReady(const MP_PLAYER* p);
static long MpClampAngVel(long v);	/* used by the playout writer, defined with the car sync */
static void MpPlayResetAll(void);	/* session reset clears the snapshot history */
static int MpSelfPoseAt(unsigned frame, int* x, int* y, int* z);	/* contact validation */

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

/* A LIVE join: the engine builds OUR car on the level's own start point, which is
 * where the host (and everyone before us) came in. MpPlaceSelfTick moves it to a
 * random road spot away from the other players once the level is up. */
/* Placement is tried from the FIRST frame our car exists, and retried every frame
 * (the road data around the probe may still be streaming in) for up to this many
 * frames before giving up on the level start. It used to wait a fixed 15 frames
 * and then try once, which is the visible "sits on the host, then pops away". */
#define MP_PLACE_SELF_SETTLE	1
#define MP_PLACE_SELF_TRIES	90
static int gMpPlaceSelfFrames;
static int gMpPlaceSelfTry;	/* frames spent trying to place (MpPlaceSelfTick) */
/* Map-wide join placement (full-city Take a Ride only): the ONE region streamed
 * for this join, and how many centers have been probed inside it. -1 = none
 * chosen yet. Kept across ticks so a join never thrashes the streamer. */
static int gMpWideRegion = -1;
static int gMpWideTries;
static int gMpPlaceSelfHoldY;	/* the y the car is pinned at while placement runs */
static int gMpLocalAppliedPad = -1;	/* the pad that drove OUR car, -1 = none seen yet */

/* The road search (defined with the warp/respawn helpers, used by the join spawn
 * far above them). Its gates are the warp's: JerRoadAt, then a resident and clear
 * cell, and the caller streams the winner once. */
static int MpRoadHeadingNear(int x, int z, int want);
static int MpFindRoadSpotNear(int cx, int cy, int cz, int seed,
	int* outX, int* outZ, int* outDir, int* outRing);
static int MpFindRespawnSpot(int cx, int cz, int* outX, int* outZ, int* outDir);

/* The last message received and from which connection: logged with every respawn,
 * re-place and refused write to our own car, so a log says what set it off. */
static char gMpLastRxTag[5] = "none";
static int gMpLastRxConn = -1;
static unsigned long gMpLastRxFrame;

/* Is car slot `slot` OUR car (the one the local player drives on this machine)? A
 * received message about ANOTHER player may never rebuild, re-place, adopt or remove
 * this car. Logs the refusal with the message that asked, so a future log is conclusive. */
static int MpRemoteTargetsLocalCar(int slot, const char* what, int otherId)
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

static void MpFailsafeReset(void);
static void MpFailsafeNoteCar(int slot);

void MpSessionReset(void)
{
	/* a session that is over must not stay on the board: the unregister is what
	 * takes it off, and MpMasterAdvertiseStop sends it when the socket is free
	 * (otherwise the entry lapses on the master's ttl). Idempotent, so the several
	 * "this session is over" paths can all call it. */
	MpMasterAdvertiseStop();

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
	gMp.city = 0;
	gMp.timeOfDay = -1;
	gMp.weather = -1;
	gMp.seed = 0;
	gMp.frame = 0;
	gMpHitArmFrame = 0;	/* collisions arm afresh in every match */
	memset(gMpSelfContactFrame, 0, sizeof(gMpSelfContactFrame));
	gMpHitSent = gMpHitRx = gMpHitApplied = gMpHitKept = gMpHitSeparated = gMpHitRejected = 0;
	MpPlayResetAll();	/* no snapshot history survives a match */
	gMp.running = 0;
	gMp.leaving = 0;
	gMp.modsMatched = 1;
	gMp.localPlaced = 0;
	gMp.liveJoin = 0;
	gMp.gameStarted = 0;
	gMpPlaceSelfFrames = 0;
	gMpPlaceSelfTry = 0;
	gMpWideRegion = -1;
	gMpWideTries = 0;
	gMpPlaceSelfHoldY = 0;

	/* ...and a "change car" still waiting for a spare slot is session state too: it must not
	 * outlive the session it was asked in. MpLeaveSession forgets it, but every OTHER way back
	 * to the frontend (MpReturnToFrontend) went through this function without doing so, so the
	 * wait kept counting down in the menus and fired a change against whatever car happened to
	 * be under the player there. */
	MpCarChangeWaitForget();

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
	MpRejoinCancel("hosting");

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
	MpRejoinCancel("another join");

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

	/* ...and a Change car still waiting for a spare slot: the world it was waiting on
	 * is going away, so the wait is not ours to keep. */
	MpCarChangeWaitForget();

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

	/* reserved[0] = our palette, reserved[1] = 1 "palette present" (old hosts
	 * ignore both): so the roster names our colour before our first carstate. */
	{
		extern u_char defaultPlayerPalette;

		/* the palette this car can actually wear (the host clamps too) */
		c.reserved[0] = (uint8_t)defaultPlayerPalette;
		if (gMp.localPlayerId >= 0 && MpLocalPlayer() != NULL && MpLocalPlayer()->carId >= 0 &&
		    MpLocalPlayer()->carId < MAX_CARS)
			c.reserved[0] = (uint8_t)car_data[MpLocalPlayer()->carId].ap.palette;
		c.reserved[1] = 1;
	}

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

/* Every player's link to us, for everyone's scoreboard (MP_TAG_STATS). Host only;
 * the same numbers the host's own scoreboard draws from MpPeerStats. */
static void MpHostSendStats(void)
{
	unsigned char buf[sizeof(MP_STATS) + MP_MAX_PLAYERS * sizeof(MP_STATS_ENTRY)];
	MP_STATS h;
	int id, n = 0;

	if (!MpIsHost())
		return;

	for (id = 1; id < MP_MAX_PLAYERS; id++)
	{
		MP_PEER_STATS st;
		MP_STATS_ENTRY e;

		if (!MpPeerStats(id, &st) || st.linkMs == 0)
			continue;

		memset(&e, 0, sizeof(e));
		e.playerId = (uint8_t)id;
		e.lossPct = (uint8_t)(st.lossPct < 0 ? 0 : (st.lossPct > 100 ? 100 : st.lossPct));
		e.pingMs = (uint16_t)(st.pingMs > 0xFFFF ? 0xFFFF : st.pingMs);
		e.rxBps = (uint32_t)(st.rxBytes * 1000UL / st.linkMs);
		e.txBps = (uint32_t)(st.txBytes * 1000UL / st.linkMs);
		memcpy(buf + sizeof(h) + n * sizeof(e), &e, sizeof(e));
		n++;
	}

	if (n == 0)
		return;

	memset(&h, 0, sizeof(h));
	h.count = (uint8_t)n;
	memcpy(buf, &h, sizeof(h));
	MpHostBroadcast(MP_TAG_STATS, 0, buf, (int)(sizeof(h) + n * sizeof(MP_STATS_ENTRY)));
}

/* The host's report of everyone's link. A client keeps it per player for the
 * scoreboard; the host measures its own and ignores this. */
static void MpHandleStats(const unsigned char* p, int len)
{
	MP_STATS h;
	int i, n;

	if (MpIsHost() || len < (int)sizeof(MP_STATS))
		return;

	memcpy(&h, p, sizeof(h));
	n = h.count > MP_MAX_PLAYERS ? MP_MAX_PLAYERS : h.count;

	if (len < (int)(sizeof(MP_STATS) + (size_t)n * sizeof(MP_STATS_ENTRY)))
		return;

	for (i = 0; i < n; i++)
	{
		MP_STATS_ENTRY e;
		MP_PLAYER* pl;

		memcpy(&e, p + sizeof(MP_STATS) + i * sizeof(e), sizeof(e));
		pl = MpGetPlayer(e.playerId);

		if (pl == NULL || pl->isLocal)
			continue;

		pl->netRxBps = e.rxBps;
		pl->netTxBps = e.txBps;
		pl->netLoss = e.lossPct;
		pl->netLastMs = MpNowMs();

		if (e.pingMs != 0)
			pl->pingMs = e.pingMs;
	}
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
		e->palette = (uint16_t)p->palette;
		/* Identity, not appearance: a late joiner needs the character BEFORE the
		 * first pose, and a player in a car is not sending poses at all. Use the
		 * config for ourselves (the registry row is fed from the wire). */
		e->pedModel = (uint8_t)MpPedModelEffective(
			p->isLocal ? gMp.config.pedModel : p->pedModel);

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

/* Is this CAR_DATA slot OWNED by somebody? A join may only ever build a car in a
 * slot that nobody drives here and nobody has a claim on:
 *
 *   - the local engine car (slot 0 / player[0].playerCarId) and any
 *     CONTROL_TYPE_PLAYER slot;
 *   - any car an active roster row names (MpGetPlayerByCar is the same test seen
 *     through the registry);
 *   - ANY row's leftCarSlot1 -- the body a player got OUT of and is standing
 *     next to. That body is CIV_AI + the quiet pad, so every other test calls it
 *     free, and InitPlayer on it is exactly the "put back on a road, repaired,
 *     felony cleared" reset players report when someone joins. This test must
 *     not be limited to active rows: a roster can drop a row for one poll gap
 *     while the body still belongs to that player.
 *
 * `ignoreId` is the player being seated; their own row is not a conflict. */
static int MpCarSlotOwnedByPlayer(int slot, int ignoreId)
{
	int i;

	if (slot < 0 || slot >= MAX_CARS)
		return 1;

	if (slot == 0 || slot == (int)player[0].playerCarId)
		return 1;

	if (car_data[slot].controlType == CONTROL_TYPE_PLAYER)
		return 1;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		const MP_PLAYER* p = &gMp.players[i];

		if (p->id == ignoreId)
			continue;

		if (p->active && p->carId == slot)
			return 1;

		if (p->leftCarSlot1 == slot + 1)
			return 1;
	}

	return 0;
}

/* The clearance a join must leave around every other player's car. A car is
 * about 1000 units long, so two cars this far apart cannot overlap, and a
 * joiner cannot materialise in somebody's braking distance. */
#define MP_JOIN_CLEAR	2000

/* Is (x,z) at least MP_JOIN_CLEAR from every OTHER player's car? `ignoreId` is
 * the player being seated and `ignoreSlot` the CAR_DATA being placed, if any
 * (a live joiner's own car is still on the level start while we choose its new
 * spot, and must not veto it). Slot -1 is the local engine car, checked
 * explicitly because the host-side spawn must not rely on a roster row naming
 * it. */
static int MpJoinSpotClearOfPlayers(int x, int z, int ignoreId, int ignoreSlot)
{
	long long dx, dz;
	int i;

	for (i = -1; i < MP_MAX_PLAYERS; i++)
	{
		int slot;

		if (i < 0)
		{
			slot = (int)player[0].playerCarId;
		}
		else
		{
			const MP_PLAYER* o = &gMp.players[i];

			if (!o->active || o->id == ignoreId)
				continue;

			slot = o->carId;
		}

		if (slot < 0 || slot >= MAX_CARS || slot == ignoreSlot)
			continue;

		if (car_data[slot].controlType == CONTROL_TYPE_NONE)
			continue;		/* an engine-cleared slot is not a car */

		dx = (long long)car_data[slot].hd.where.t[0] - x;
		dz = (long long)car_data[slot].hd.where.t[2] - z;

		if (dx > -MP_JOIN_CLEAR && dx < MP_JOIN_CLEAR &&
			dz > -MP_JOIN_CLEAR && dz < MP_JOIN_CLEAR)
			return 0;
	}

	return 1;
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

		/* NO PLACEHOLDER FOR A LIVE JOINER: wait until its owner has reported a
		 * pose. Building now would put the car on a near-start road (or the level
		 * start) until the first carstate corrects it -- the visible "spawns in the
		 * normal place, then teleports". A returning player already has a pose from
		 * earlier in the match, so this only delays a brand-new body. */
		if (gMp.running && !p->joinedPose)
			continue;

		/* The smallest CAR_DATA slot no player is already driving. Counting the
		 * rows was only correct while the cars occupied a contiguous [0, n): after
		 * a player LEAVES, its slot is free but the count still points at a slot
		 * that is taken. Slot 0 is the local player's, and the same scan skips it
		 * because the local row drives it. */
		/* A free PLAYER slot: no player row owns it, and it is not a car someone is
		 * driving on this machine. Slot 0 and our own driven car are never handed out:
		 * InitPlayer on them rebuilt OUR car as a fresh one on a road near the level
		 * start (repaired, felony 0) the moment a peer joined -- when our own row did
		 * not (yet, or any more) name the car we sit in. */
		slot = -1;
		for (k = 1; k < MAX_CARS && k < MAX_PLAYERS; k++)
		{
			/* a car somebody got out of (stopped, empty, quiet pad): only if
			 * nothing else is free - see the second pass */
			if (car_data[k].controlType == CONTROL_TYPE_CIV_AI && car_data[k].ai.padid == &gMpQuietPad)
				continue;

			if (MpCarSlotOwnedByPlayer(k, id))
				continue;

			slot = k;
			break;
		}

		for (k = 1; slot < 0 && k < MAX_CARS && k < MAX_PLAYERS; k++)
		{
			if (car_data[k].controlType == CONTROL_TYPE_CIV_AI && car_data[k].ai.padid == &gMpQuietPad &&
				!MpCarSlotOwnedByPlayer(k, id))
				slot = k;
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
			/* A RANDOM ROAD POINT near the level start, facing along the road.
			 *
			 * The engine's own step for player 2 is 600 units in +x (main.c), and
			 * following it for every id put player 15 at +9000 -- past the edge of a
			 * multiplayer ARENA, where placement resolves the ground to nothing and
			 * the car drops into the void. A grid fixed the void; it did not stop
			 * sixteen players appearing in one tidy lay-by. So use the same ring
			 * search the warp and the respawn already trust (MpFindRoadSpotNear),
			 * asking for a RANDOM road point on the nearest ring that has road, and
			 * face the car along the road it landed on: a car dropped in at an
			 * arbitrary angle is one the driver has to sort out before driving.
			 *
			 * The grid stays as the FALLBACK -- a join must never fail to place just
			 * because no road was in reach (an arena's start can be a car park). id 0
			 * is never here: the engine built its own record for the local player. */
			int col = 0, row = 0, dx = 0, dz = 0;
			int rx = 0, rz = 0, rdir = -1;
			int onRoad = 0;
			int baseX = PlayerStartInfo[0]->position.vx;
			int baseZ = PlayerStartInfo[0]->position.vz;

			/* AT THE POSE ITS OWNER ANNOUNCED. This is the live-joiner case: the
			 * first carstate arrived before this machine assigned a slot, so build
			 * the body where it actually is instead of at a near-start road and then
			 * moving it. The y stays the engine's (resolved from the ground under
			 * x/z, main.c's own convention); the owner's next sample carries its
			 * exact y, so a region this machine has not streamed cannot keep it in
			 * the air. */
			if (p->joinedPose)
			{
				PlayerStartInfo[slot]->position.vx = p->joinPoseX;
				PlayerStartInfo[slot]->position.vz = p->joinPoseZ;
				PlayerStartInfo[slot]->rotation = (u_short)(p->joinPoseHeading & 0xfff);
				onRoad = 2;		/* 2 = the owner's announced pose */
			}
			else if (id > 0)
			{
				int tries;

				/* The level-start region may not be resident any more: the host can
				 * have driven away while this joiner was loading. A search there sees
				 * no road, and InitPlayer then resolves vy over no ground -- the
				 * "joiner in the air" copy. Centre on the host's LIVE x/z instead;
				 * only the x/z is a search centre, never the car's height (that is
				 * still the engine's, from the ground under the chosen spot). */
				if (!jer_map_region_resident(jer_map_region_of(baseX, baseZ)) &&
					player[0].playerCarId >= 0 && player[0].playerCarId < MAX_CARS)
				{
					baseX = car_data[player[0].playerCarId].hd.where.t[0];
					baseZ = car_data[player[0].playerCarId].hd.where.t[2];

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] spawn: level start region %d is not resident - searching around the host at %d,%d\n",
							jer_map_region_of(PlayerStartInfo[0]->position.vx,
								PlayerStartInfo[0]->position.vz), baseX, baseZ);
				}

				/* No spooling here on purpose: MpWarpSpotOk already refuses a spot
				 * whose ground is not resident, and moving the streamer while a join
				 * is being placed is how players end up in the void. Near the level
				 * start the region is resident by definition.
				 *
				 * CLEAR OF EVERY OTHER CAR. The ring search stops at the first ring
				 * with a road, which can be a short hop from the host's own car --
				 * the "late joiner on top of the host" report. Retry around centres
				 * pushed one clearance-step out in all four directions, so the first
				 * ring the search can return is already clear. */
				for (tries = 0; tries < 4 && !onRoad; tries++)
				{
					int sx = 0, sz = 0, sdir = -1;
					int cx = baseX;
					int cz = baseZ;
					int step = MP_JOIN_CLEAR + 1200;

					if (tries == 0)
						cx += step;
					else if (tries == 1)
						cx -= step;
					else if (tries == 2)
						cz += step;
					else
						cz -= step;

					if (!MpFindRoadSpotNear(cx, 0, cz, p->id + tries * 64, &sx, &sz, &sdir, NULL))
						continue;

					if (!MpJoinSpotClearOfPlayers(sx, sz, id, -1))
						continue;

					rx = sx;
					rz = sz;
					rdir = sdir;
					onRoad = 1;
				}
			}

			if (onRoad == 1)
			{
				PlayerStartInfo[slot]->position.vx = rx;
				PlayerStartInfo[slot]->position.vz = rz;
				/* vy stays 0 -- the engine's convention, not an oversight: placement
				 * resolves the ground under the car. */
				PlayerStartInfo[slot]->rotation = (u_short)
					((rdir >= 0) ? rdir : PlayerStartInfo[0]->rotation);
			}
			else if (onRoad == 0)
			{
				int cell = id - 1;

				col = cell % 4;
				row = cell / 4;

				dx = col * 600 - 900;	/* across: -900, -300, +300, +900 */
				dz = (row + 1) * 600;	/* back:   +600, +1200, +1800, +2400 */

				PlayerStartInfo[slot]->position.vx = PlayerStartInfo[0]->position.vx + dx;
				PlayerStartInfo[slot]->position.vz = PlayerStartInfo[0]->position.vz + dz;
				PlayerStartInfo[slot]->rotation = PlayerStartInfo[0]->rotation;
			}
			/* onRoad == 2: the announced pose is already written above; it must
			 * NOT be replaced by the road/grid blocks (they would write rx/rz,
			 * which are unset on this path -- that was the origin spawn). */

			if (gMpCtx != NULL)
			{
				if (onRoad == 2)
					gMpCtx->jer_log(gMpCtx,
						"[mp] spawn: player %d -> slot %d at its announced pose %d,%d heading %d (no placeholder at the level start)\n",
						id, slot, PlayerStartInfo[slot]->position.vx,
						PlayerStartInfo[slot]->position.vz, PlayerStartInfo[slot]->rotation);
				else if (onRoad)
					gMpCtx->jer_log(gMpCtx,
						"[mp] spawn: player %d -> slot %d on a road at %d,%d facing %d (%s), clear of every other car by %d\n",
						id, slot, PlayerStartInfo[slot]->position.vx,
						PlayerStartInfo[slot]->position.vz, PlayerStartInfo[slot]->rotation,
						(rdir >= 0) ? "along the road" : "level heading, no road angle",
						MP_JOIN_CLEAR);
				else
					gMpCtx->jer_log(gMpCtx,
						"[mp] spawn: player %d -> slot %d at the level's own start %d,%d (+%d,%d grid %d,%d), no clear road in reach - grid fallback NOT checked against other cars, no y\n",
						id, slot, PlayerStartInfo[slot]->position.vx,
						PlayerStartInfo[slot]->position.vz, dx, dz, col, row);
			}
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

		if (MpRemoteTargetsLocalCar(slot, "spawn (InitPlayer)", id))
			continue;

		/* BELT AND BRACES, and the answer to the open reset report: the scan above
		 * already refuses owned slots, but a join must never reach InitPlayer on a
		 * body that belongs to someone even if a future scan regresses. Refuse and
		 * say which slot, so a log is conclusive about what was not touched. */
		if (MpCarSlotOwnedByPlayer(slot, id))
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] spawn: refused slot %d for player %d - car is owned (controlType %d, our car %d, named by a player row %d)\n",
					slot, id, (int)car_data[slot].controlType, (int)player[0].playerCarId,
					MpGetPlayerByCar(slot) != NULL ? 1 : 0);
			continue;
		}

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

		/* The body exists now: the next carstate should record a FRESH pose, so a
		 * later re-seat is not anchored to the pose this build used. */
		p->joinedPose = 0;
		/* The recorded "car they left" (MP_PLAYER.leftCarSlot1) is only a seat to
		 * REUSE, never a second body: now that this player has a fresh car from the
		 * spawn path, forget it. Otherwise a later carstate could reclaim a body
		 * that has nothing to do with the car just built. */
		p->leftCarSlot1 = 0;
		p->leftCarModel = -1;
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

	/* A roster is the HOST's view of the match and nobody else's. Accepted only by
	 * a client: a host that took one from a peer would adopt whatever the peer
	 * wrote -- names, ids, and who is flagged as host. */
	if (gMp.role != MP_ROLE_CLIENT)
		return;

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
		MP_PLAYER* pl;

		e->name[sizeof(e->name) - 1] = '\0';	/* wire string: never trust it terminated */
		pl = MpGetPlayer(e->id);

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
		pl->palette = (int)e->palette;
		/* The roster is the late-join snapshot: it names the character the owner
		 * chose, so a joiner can draw the right ped before JPPD arrives. Stored as
		 * the EFFECTIVE model for this level; the owner's choice stays in its row
		 * for when the level changes back. */
		pl->pedModel = MpPedModelEffective((int)e->pedModel);

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
	gBootMpArena = arena & 1;
}

/* The host's SESSION RULES, packed for the wire. ONE packed byte (MP_RULES_*),
 * carried three ways: MP_WELCOME.rules for a client that joins a running match,
 * the free bits of MP_SESSION.spare for the launch, and MP_BEACON.rules so a
 * LAN browser can name the mode of a server it has not joined. These two
 * functions and those three sites are the only readers and writers,
 * deliberately -- the map shape already showed how easy it is for two carriers
 * to drift. */
static uint32_t MpRulesFromConfig(void)
{
	return MP_RULES_PACK(gMp.config.cops, gMp.config.invuln, gMp.config.copDifficulty,
		gMp.config.immune);
}

/* What the HOST should publish right now: the rules the running session was
 * launched with once there is one, otherwise the lobby defaults from the menu.
 * The difference matters -- a host that edits the menu mid-match must not
 * retro-fit new rules onto the players already in it.
 *
 * Not static: mp_net.c's beacon is the third carrier of this byte (the browser
 * reads it to name a server's mode), and a second copy of the decision here is
 * exactly the drift the comment above warns about. */
uint32_t MpRulesForPeer(void)
{
	if (gMp.ruleSet)
		return MP_RULES_PACK(gMp.ruleCops, gMp.ruleInvuln, gMp.ruleDiff, gMp.ruleImmune);

	return MpRulesFromConfig();
}

/* Adopt a packed rule byte and remember it as in force. `how` names the carrier,
 * because "the rules are not what I set" is otherwise unattributable. */
static void MpRulesAdopt(uint32_t packed, const char* how)
{
	int cops = MP_RULES_COPS(packed);
	int invuln = MP_RULES_INVULN(packed);
	int diff = MP_RULES_DIFF(packed);
	int immune = MP_RULES_IMMUNE(packed);

	if (diff > MP_COP_DIFF_MAX)
		diff = MP_COP_DIFF_MAX;

	gMp.ruleCops = cops;
	gMp.ruleInvuln = invuln;
	gMp.ruleImmune = immune;
	gMp.ruleDiff = diff;
	gMp.ruleSet = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] session rules from %s: cops=%s cop difficulty=%d immune=%s invulnerable=%s\n",
			how, cops ? "on" : "off", diff, immune ? "on" : "off", invuln ? "on" : "off");
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

	/* ...and list it on the master too, when one is configured. This is the whole
	 * difference between a host a stranger can find and one only a friend who was
	 * told the address can reach. It costs a POST and a small heartbeat, never
	 * blocks, and a master that is down changes nothing here. */
	MpMasterAdvertiseStart(gMp.config.hostName, gMp.config.port,
		gMp.city, gMp.gamemode, (int)MpRulesForPeer());
	gMp.seed = 0x5eed1318u;		/* shared run seed */

	memset(&st, 0, sizeof(st));
	st.session.gamemode = (uint8_t)gMp.gamemode;
	st.session.city = (uint8_t)gMp.city;
	st.session.timeOfDay = (uint8_t)(gMp.timeOfDay < 0 ? 0 : gMp.timeOfDay);
	st.session.weather = (uint8_t)(gMp.weather < 0 ? 0 : gMp.weather);
	st.session.seed = gMp.seed;
	st.session.numPlayers = (uint8_t)gMp.playerCount;
	st.session.state = MP_SESSION_STARTING;
	/* The map SHAPE rides in spare (there are no two free bytes): 1 = a
	 * multiplayer map, and which arena. It must travel HERE and not only in
	 * MpSendWelcome, because a client that connected while the host was still
	 * choosing - i.e. before this START - was welcomed with whatever shape the
	 * host had at that moment.
	 *
	 * The SESSION RULES ride beside it in spare's free bits, and for the same
	 * reason: this is the moment they are decided. Snapshot the menu's settings
	 * into the session FIRST (MpRulesAdopt), so a host who edits the menu
	 * mid-match cannot retro-fit new rules onto the players already in it. */
	{
		uint32_t rules = MpRulesFromConfig();

		MpRulesAdopt(rules, "the host's launch");

		st.session.spare = (uint16_t)((gBootMpLevel ? 1 : 0) |
			((uint16_t)rules << 1) |
			((gBootMpArena & 1) << 8));
	}

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
	unsigned char buf[sizeof(MP_HELLO) + MP_MAX_MODS * sizeof(MP_MOD_INFO) + 4 + MP_PLAYER_ID_LEN
			  + 5 + MP_HELLO_VER_MAX];
	MP_HELLO h;
	int n, len;

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

	/* the player id block after the manifest (mp_proto.h, MP_HELLO_ID_TAG) */
	len = (int)(sizeof(h) + (size_t)n * sizeof(MP_MOD_INFO));
	memcpy(buf + len, MP_HELLO_ID_TAG, 4);
	memcpy(buf + len + 4, gMp.config.playerId, MP_PLAYER_ID_LEN);
	len += 4 + MP_PLAYER_ID_LEN;

	/* the build block (mp_proto.h, MP_HELLO_VER_TAG): display only */
	{
		char ver[MP_HELLO_VER_MAX + 1];
		int vl;

		MpVersionLabel(ver, sizeof(ver));
		vl = (int)strlen(ver);
		memcpy(buf + len, MP_HELLO_VER_TAG, 4);
		buf[len + 4] = (unsigned char)vl;
		memcpy(buf + len + 5, ver, (size_t)vl);
		len += 5 + vl;
	}

	MpSendToHost(MP_TAG_HELLO, MP_FLAG_RELIABLE, buf, len);

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
			/* strncmp, not strcmp: cm[] is the PEER's mod list (the wire `mods`);
			 * hm[] above is our own MpBuildManifest. cm's id/version are fixed char
			 * fields in a struct copied off the wire, so a peer that does not terminate
			 * one would make strcmp walk past its struct. The field sizes bound the read
			 * instead -- and bounding both operands is what keeps the comparison exact
			 * for a well-formed peer. */
			if (strncmp(cm[i].id, hm[j].id, MP_MOD_ID_MAX) == 0)
			{
				if (strict == MP_MODCHECK_EXACT && strncmp(cm[i].version, hm[j].version, MP_MOD_VER_MAX) != 0)
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
			/* strncmp, not strcmp: cm[] is the PEER's mod list (the wire `mods`);
			 * hm[] above is our own MpBuildManifest. cm's id/version are fixed char
			 * fields in a struct copied off the wire, so a peer that does not terminate
			 * one would make strcmp walk past its struct. The field sizes bound the read
			 * instead -- and bounding both operands is what keeps the comparison exact
			 * for a well-formed peer. */
			if (strncmp(cm[i].id, hm[j].id, MP_MOD_ID_MAX) == 0)
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
	w.rules = (uint8_t)MpRulesForPeer();	/* the session rules a joiner must adopt */
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
	if (c.reserved[1] == 1)
		pl->palette = (int)c.reserved[0];	/* the colour they will drive in */

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
	char joinVer[MP_HELLO_VER_MAX + 1] = "";

	if (len < (int)sizeof(MP_HELLO))
	{
		MpConnClose(connIndex);
		return;
	}

	memcpy(&h, p, sizeof(h));
	h.playerName[sizeof(h.playerName) - 1] = '\0';	/* wire string: never trust it terminated */
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
	 * itself, because the string is `git describe --tags --match 'v*' --always --dirty` and
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

			/* the optional player id block after the manifest (mp_proto.h) */
			{
				int at = (int)(sizeof(MP_HELLO) + (size_t)n * sizeof(MP_MOD_INFO));

				pl->hasPlayerId = 0;

				if (len >= at + 4 + MP_PLAYER_ID_LEN && memcmp(p + at, MP_HELLO_ID_TAG, 4) == 0)
				{
					memcpy(pl->playerId, p + at + 4, MP_PLAYER_ID_LEN);
					pl->hasPlayerId = 1;
					at += 4 + MP_PLAYER_ID_LEN;
				}

				/* ...and the optional build block after it */
				if (len >= at + 5 && memcmp(p + at, MP_HELLO_VER_TAG, 4) == 0)
				{
					int vl = p[at + 4];

					if (vl > MP_HELLO_VER_MAX)
						vl = MP_HELLO_VER_MAX;
					if (len >= at + 5 + vl)
					{
						int k;

						memcpy(joinVer, p + at + 5, (size_t)vl);
						joinVer[vl] = '\0';
						for (k = 0; k < vl; k++)	/* wire text: printable only */
							if (joinVer[k] < 32 || joinVer[k] > 126)
								joinVer[k] = '?';
					}
				}
			}

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

	/* The join TOAST: who, on what build, with which mods -- so "it works on my
	 * machine" is answerable at a glance. The mods digest is computed HERE from
	 * the manifest the client sent (MpModsDigest), so it needs nothing new on the
	 * wire; the version needs the optional JPVR block (an older client: "?"). */
	{
		char toast[128];

		snprintf(toast, sizeof(toast), "%s joined (%s%s, mods %06x)", h.playerName,
			joinVer[0] && joinVer[0] >= '0' && joinVer[0] <= '9' ? "v" : "",
			joinVer[0] ? joinVer : "?", MpModsDigest(mods, n));
		MpConsoleLine("%s", toast);
		jer_notify(toast, 1, 6);
	}

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] player %d '%s' joined (%d/%d)%s\n",
			id, h.playerName, gMp.playerCount, MP_MAX_PLAYERS,
			matched ? "" : " [mods differ]");
}

/* Chat: the host is the echo point, so a line is announced
 * exactly once on every peer. Clients send to the host, which republishes to
 * everyone (including the sender); the host announces its own line directly. */
/* "!login secret", "/kick 2": a chat COMMAND, the same test the dedicated server's
 * commands plugin uses ('!' or '/' then a non-space, non-'!'/'/' character). Only a
 * dedicated server runs commands; a game host has none, so it must never relay such a
 * line -- it may carry a password meant for a server. */
static int MpChatIsCommand(const char* t)
{
	return t != NULL && (t[0] == '!' || t[0] == '/') && t[1] != '\0'
		&& t[1] != ' ' && t[1] != '!' && t[1] != '/';
}

void MpSendChat(const char* text)
{
	MP_CHAT c;

	if (!MpIsActive() || text == NULL || text[0] == '\0')
		return;

	memset(&c, 0, sizeof(c));
	c.playerId = (uint8_t)(gMp.localPlayerId < 0 ? 0 : gMp.localPlayerId);
	snprintf(c.text, sizeof(c.text), "%s", text);

	if (MpIsHost() && MpChatIsCommand(c.text))
	{
		MpConsoleLine("chat commands only work on a dedicated server; not sent");
		return;
	}

	if (MpIsHost())
	{
		MpConsoleChat(gMp.config.playerName, gMp.config.colorOn,
			gMp.config.colorR, gMp.config.colorG, gMp.config.colorB, c.text, NULL);
		MpHostBroadcast(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
	}
	else
	{
		MpSendToHost(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
	}
}

static void MpHandleChat(int connIndex, const unsigned char* p, int len)
{
	MP_CHAT c;
	MP_PLAYER* pl;
	int from;

	if (len < (int)sizeof(MP_CHAT))
		return;

	memcpy(&c, p, sizeof(c));
	c.text[sizeof(c.text) - 1] = '\0';

	/* The id in the frame says whose name a line is announced under, and the host is
	 * the echo point -- so the host is the one place that can say who really spoke.
	 * Without this a peer could post under any player's name (and the host would
	 * relay it under that name to everybody). */
	from = MpConnPlayerId(connIndex);

	if (MpIsHost() && from >= 0)
	{
		c.playerId = (uint8_t)from;
		memset(c.reserved, 0, sizeof(c.reserved));	/* a peer cannot mark itself [Admin] */
	}

	/* A peer's "!login ..." reached a game host, which runs no commands: drop it
	 * unseen. Printing or relaying it would show a password to every player. */
	if (MpIsHost() && MpChatIsCommand(c.text))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] dropped a chat command from player %d (not relayed)\n", (int)c.playerId);
		return;
	}

	pl = MpGetPlayer(c.playerId);

	/* A server that put a colour in the frame is speaking (a dedicated server's
	 * motd): draw the whole line in that ink. Two gates, both cheap: all-zero means
	 * an ordinary chat line, and the ink is honoured only for a speaker with NO
	 * player row -- the server itself. A player's own line therefore cannot dress
	 * itself up in a colour it did not choose through the normal colour path. */
	{
		const unsigned char* ink =
			(pl == NULL && (c.reserved[0] | c.reserved[1] | c.reserved[2])) ? c.reserved : NULL;

		/* One line in the log as well: "the server said something, in this ink" is
		 * exactly what a support log has to carry when a player reports that the
		 * greeting did not appear. */
		if (ink != NULL && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] server voice: '%s' in #%02X%02X%02X\n",
				c.text, ink[0], ink[1], ink[2]);
		else if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] chat from %s: '%s'\n",
				pl != NULL ? pl->name : "SERVER", c.text);

		if (pl != NULL)
		{
			char tagged[MP_NAME_MAX + 8];
			int admin = (c.reserved[0] == MP_CHAT_MARK_ADMIN && c.reserved[1] == 0 && c.reserved[2] == 0);

			if (admin)
				snprintf(tagged, sizeof(tagged), "[Admin] %s", pl->name);

			MpConsoleChat(admin ? tagged : pl->name, pl->colorOn, pl->colorR, pl->colorG, pl->colorB, c.text, ink);
		}
		else
			MpConsoleChat(NULL, 0, 0, 0, 0, c.text, ink);
	}

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

/* The host removing a player. The only kick path: send the message AND keep
 * the teardown in one place by letting the kicked client drive it. Only the
 * host may kick, and only a non-zero player id (the host is id 0). */
void MpKickPlayer(int playerId)
{
	MP_KICK k;
	int conn;

	if (!MpIsHost())
		return;

	if (playerId <= 0)
		return;

	conn = MpConnFindByPlayer(playerId);

	if (conn < 0)
		return;

	memset(&k, 0, sizeof(k));
	k.playerId = (uint8_t)playerId;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] kicking player %d (conn %d)\n", playerId, conn);

	MpSendConn(conn, MP_TAG_KICK, MP_FLAG_RELIABLE, &k, sizeof(k));

	/* The removal on THIS side is the kicked client's own LEAVE (MpLeaveSession
	 * replies with one), exactly as for any other leaver, which is what keeps the
	 * roster/car cleanup in one place. A wedged client that never replies is reaped
	 * by the ordinary 30 s liveness check. (Half-closing here instead was tried and
	 * reverted: the FIN raced the KICK, so the client saw a bare connection loss
	 * and never printed "kicked by the host".) */
}

/* AUTO-REJOIN after a server city change. A dedicated server's !vote switches
 * city by kicking everyone with MP_KICK_FLAG_REJOIN set; we remember the address
 * and our car, take the ordinary leave path, then re-join through the ordinary
 * join (MpBeginJoinAsync, as an auto session so no menus are needed) and, once
 * the new level is up, put our old car back through Change car. Client only. */
static const char* MpChangeCarRefusal(int city, int model);

#define MP_REJOIN_FIRST_MS	2500
#define MP_REJOIN_EVERY_MS	4000
#define MP_REJOIN_WINDOW_MS	20000

static struct
{
	int active;		/* waiting to (re)try the join */
	int settling;		/* joined: waiting for the level to put the car back */
	int joining;		/* inside our own MpBeginJoinAsync (not a cancel) */
	int prevAuto;		/* gMp.autoSession before we forced it */
	char host[64];
	int port;
	unsigned long nextMs, deadlineMs;
	int carCity, carModel;	/* -1 model = nothing to restore */
} gMpRejoin;

int MpRejoinActive(void)
{
	return gMpRejoin.active;
}

static void MpRejoinEnd(void)
{
	if (gMpRejoin.active || gMpRejoin.settling)
		gMp.autoSession = gMpRejoin.prevAuto;

	gMpRejoin.active = 0;
	gMpRejoin.settling = 0;
	gMpRejoin.carModel = -1;
}

void MpRejoinCancel(const char* why)
{
	if (gMpRejoin.joining || (!gMpRejoin.active && !gMpRejoin.settling))
		return;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] rejoin cancelled: %s\n", why ? why : "?");

	/* Esc while a connect is in flight: drop it, as a refused join would. */
	if (gMpRejoin.active && gMp.role == MP_ROLE_CLIENT && !gMp.running)
	{
		MpClientDisconnect();
		gMp.role = MP_ROLE_NONE;
		MpJoinStateSet(MP_JOIN_IDLE);
	}

	MpRejoinEnd();
}

static void MpRejoinArm(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	const char* host = MpJoinTarget();

	memset(&gMpRejoin, 0, sizeof(gMpRejoin));
	gMpRejoin.carModel = -1;

	if (host == NULL || host[0] == '\0')
	{
		jer_error("The server is changing city - please rejoin");
		return;
	}

	snprintf(gMpRejoin.host, sizeof(gMpRejoin.host), "%s", host);
	gMpRejoin.port = MpJoinTargetPort();

	/* Our car as Change car names it: an ABSOLUTE city (a -1 "session city"
	 * would mean the NEW city after the switch) plus the model. */
	if (me != NULL && me->car >= 0 && !me->carIsSlot)
	{
		gMpRejoin.carCity = (me->carCity < 0) ? gMp.city : me->carCity;
		gMpRejoin.carModel = me->car;
	}
	else if (me != NULL && me->carIsSlot && me->carId >= 0 && me->carId < MAX_CARS)
	{
		/* A car-select SLOT (the usual pick) is per city, so it would name a
		 * different car in the new one: remember the model on the road instead.
		 * A slot is always the session city's own car. */
		gMpRejoin.carCity = gMp.city;
		gMpRejoin.carModel = car_data[me->carId].ap.model;
	}

	gMpRejoin.prevAuto = gMp.autoSession;
	gMpRejoin.nextMs = MpNowMs() + MP_REJOIN_FIRST_MS;
	gMpRejoin.deadlineMs = gMpRejoin.nextMs + MP_REJOIN_WINDOW_MS;
	gMpRejoin.active = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] server changing city: rejoining %s:%d (car city %d model %d)\n",
			gMpRejoin.host, gMpRejoin.port, gMpRejoin.carCity, gMpRejoin.carModel);

	jer_error("Server changing city - reconnecting... (Esc cancels)");
}

void MpRejoinTick(void)
{
	unsigned long now;

	if (gMpRejoin.settling)
	{
		MP_PLAYER* me;

		if (gMp.role != MP_ROLE_CLIENT)
		{
			MpRejoinEnd();	/* the session went away before the level came up */
			return;
		}

		me = MpLocalPlayer();

		if (!gMp.running || me == NULL || me->carId < 0)
			return;

		if (gMpRejoin.carModel >= 0)
		{
			int city = gMpRejoin.carCity, model = gMpRejoin.carModel;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] rejoin: restoring car %s model %d\n",
					MpCarCityName(city), model);

			/* A refusal is logged by Change car itself and leaves the default car. */
			if (!MpChangeCar(city, model) && gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] rejoin: car not restored now (%s) - default car kept unless the retry lands\n",
					MpChangeCarRefusal(city, model));
		}

		MpRejoinEnd();
		return;
	}

	if (!gMpRejoin.active)
		return;

	now = MpNowMs();

	if (gMp.role == MP_ROLE_CLIENT)
	{
		if (MpJoinState() == MP_JOIN_READY)
		{
			gMpRejoin.active = 0;
			gMpRejoin.settling = 1;
		}
		return;	/* a connect is in flight */
	}

	if (gMp.role != MP_ROLE_NONE || (long)(now - gMpRejoin.nextMs) < 0)
		return;

	if ((long)(now - gMpRejoin.deadlineMs) >= 0)
	{
		jer_error("Could not reconnect to %s:%d", gMpRejoin.host, gMpRejoin.port);
		MpRejoinEnd();
		return;
	}

	gMpRejoin.nextMs = now + MP_REJOIN_EVERY_MS;
	gMp.autoSession = 1;	/* no menus: launch into the new city on WELCOME */
	gMpRejoin.joining = 1;
	MpBeginJoinAsync(gMpRejoin.host, gMpRejoin.port);
	gMpRejoin.joining = 0;
}

/* MP_TAG_KICK -- the host removed us. Take the SAME clean-leave path a
 * deliberate quit uses (MpLeaveSession sends our LEAVE back, which the host
 * turns into the connection drop), rather than tearing down on our own: that
 * way every machine's roster row and our car leave the world exactly as they
 * do for any other leaver. */
static void MpHandleKick(int connIndex, const unsigned char* p, int len)
{
	MP_KICK k;

	(void)connIndex;

	/* Only the HOST kicks, so a KICK is only meaningful on a CLIENT. Acting on one
	 * received as the host (or with no session up) would let a peer remove us. */
	if (gMp.role != MP_ROLE_CLIENT)
		return;

	if (len >= (int)sizeof(MP_KICK))
		memcpy(&k, p, sizeof(k));
	else
		memset(&k, 0, sizeof(k));

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] kicked by the host (player %d)\n", (int)k.playerId);

	if (k.reserved[0] & MP_KICK_FLAG_REJOIN)
	{
		MpRejoinArm();
		MpLeaveSession();
		MpReturnToFrontend();
		return;
	}

	if (len >= (int)sizeof(MP_KICK) + MP_KICK_TEXT_LEN)
	{
		char text[MP_KICK_TEXT_LEN];
		int i;

		memcpy(text, p + sizeof(MP_KICK), sizeof(text));
		text[sizeof(text) - 1] = '\0';	/* wire string: never trust it terminated */

		for (i = 0; text[i]; i++)	/* no format/control characters reach the UI */
			if ((unsigned char)text[i] < 32 || text[i] == '%')
				text[i] = ' ';

		if (text[0])
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] kick message: %s\n", text);
			jer_error("%s", text);
		}
		else
			jer_error("You were kicked by the host");
	}
	else
		jer_error("You were kicked by the host");

	MpLeaveSession();
	MpReturnToFrontend();
}

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

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] player option: car repaired (slot %d)\n", me->carId);

	MpNotify("Car repaired");
}

/* Clear OUR wanted level. GetPlayerFelony picks the car's rating OR the
 * pedestrian's, so clear both -- leaving the other set puts the cops straight
 * back on us the moment we step out. */
void MpSelfClearFelony(void)
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

	MpNotify("Wanted level cleared");
}

static void MpHandleWelcome(const unsigned char* p, int len)
{
	MP_WELCOME w;

	/* host -> client only: a host that adopted a peer's welcome would take its own
	 * player id, session rules and map shape from whoever sent it. */
	if (gMp.role != MP_ROLE_CLIENT)
		return;

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

	/* The host's session rules (what this match is being played under). The
	 * welcome is the only way a client that joins a match IN PROGRESS ever
	 * learns them - MP_START went out at launch, before it was here. */
	MpRulesAdopt(w.rules, "the host's welcome");
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

		/* MpAddPlayer answers NULL when the player registry is full, so the car
		 * fields must be written only once we know we have a row -- these used to
		 * be written first, which is a dereference of NULL on a full roster. */
		if (me != NULL)
		{
			extern u_char defaultPlayerPalette;

			me->car = gMp.config.car;
			me->carIsSlot = gMp.config.carIsSlot;
			me->carCity = gMp.config.carCity;
			me->palette = defaultPlayerPalette;

			me->carId = 0;		/* our own car is engine slot 0 */
		}
		else if (gMpCtx != NULL)
		{
			gMpCtx->jer_log(gMpCtx,
				"[mp] WELCOME for player %d but the player registry is full\n", w.playerId);
		}
	}

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] accepted as player %d (matched=%d gamemode=%d city=%d)\n",
			w.playerId, gMp.modsMatched, w.gamemode, w.city);

	/* SAY SO ON SCREEN. modsMatched is the host's comparison of OUR mod manifest against
	 * its own, so it catches exactly the pair we deliberately allow -- a dev build joining
	 * a release build, or simply a different modlist. The protocol and the level are the
	 * same either way and the match goes ahead (the mod-check policy is off by default),
	 * but the two machines do NOT have the same cars and content, and a player who cannot
	 * tell that apart will blame the connection for whatever the mismatch does next. */
	if (!gMp.modsMatched)
		MpNotify("This host is on a different mod set - you can still play, but cars and content may not match");

	MpJoinStateSet(MP_JOIN_READY);	/* the "Connecting..." row goes away */

	/* the match is already live: adopt the host's config and launch so we
	 * spawn into the running game (the LAN browser only lists live games). */
	if (w.running)
	{
		gMp.running = 1;
		gMp.subGame = w.subGame;

		/* WE JOINED A MATCH ALREADY IN PROGRESS. That is a different placement
		 * problem from both machines launching together: the level places our car
		 * on its own start, which is where the host came in, and the old
		 * client-side gather would then drop us beside the first peer it heard
		 * from. Mark the join so the gather is skipped and MpPlaceSelfTick owns
		 * the spawn. Cleared by MpSessionReset. */
		gMp.liveJoin = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] live join: not gathering beside a peer - our car will be placed on a clear road\n");

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

		gMpPlaceSelfTry = 0;
		gMpPlaceSelfFrames = MP_PLACE_SELF_SETTLE;	/* our car starts on the level start = on the host: move it */

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

	/* host -> client only. Received as a host this used to tear the session down
	 * (role -> NONE, join -> FAILED), so one frame from any connected peer could
	 * eject the host to the frontend. */
	if (gMp.role != MP_ROLE_CLIENT)
		return;

	if (len < (int)sizeof(MP_REJECT))
		return;

	memcpy(&r, p, sizeof(r));
	r.text[sizeof(r.text) - 1] = '\0';	/* wire string: never trust it terminated */

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
	h.name[sizeof(h.name) - 1] = '\0';	/* wire string: never trust it terminated */

	if (len < (int)(sizeof(MP_CHANNEL) + (size_t)h.len))
		return;

	peer = MpIsHost() ? MpConnPlayerId(connIndex) : 0;
	if (peer < 0)
		peer = 0;

	/* A peer picks BOTH the channel name and the payload, so the bridge vets them
	 * before anything is relayed or delivered: a name this machine never
	 * registered has no handler waiting for it, and a payload bigger than
	 * jer_net_send's own ceiling is not something a sender could have produced.
	 * Dropped here rather than in the module, because a module can only defend
	 * itself against names it expected to hear. */
	if (!MpBridgeAccept(h.name, (int)h.len))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] channel: dropped '%s' (%d bytes) from player %d\n",
				h.name, (int)h.len, peer);

		return;
	}

	if (MpIsHost())
		/* relay exactly the message -- not whatever the sender appended after it */
		MpHostBroadcast(MP_TAG_CHANNEL, h.reliable ? MP_FLAG_RELIABLE : 0, p,
			(int)(sizeof(MP_CHANNEL) + (size_t)h.len));

	MpBridgeDeliver(h.name, peer, p + sizeof(MP_CHANNEL), (int)h.len);
}

static void MpHandleStart(const unsigned char* p, int len)
{
	/* host -> client only. A START received as a host is a peer choosing to launch
	 * (or relaunch) THIS machine's level, with a city, weather and rules of its
	 * choosing -- so it is dropped before even the auto-session effect below. */
	if (gMp.role != MP_ROLE_CLIENT)
		return;

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

	/* Adopt the host's map SHAPE before the launch below, or this machine loads
	 * whatever its own -mp said - the exact drift the host-as-authority rule
	 * exists to prevent. MpLaunchLocal derives gSubGameNumber from these. */
	gBootMpLevel = st.session.spare & 1;
	gBootMpArena = (st.session.spare >> 8) & 1;

	/* The host's session rules, in spare's free bits beside the shape (bit 0 is
	 * the shape, so they are shifted up by one). Adopted BEFORE the level loads
	 * below, so the first frame of the match is already under them. */
	MpRulesAdopt((uint32_t)((st.session.spare >> 1) & 0xff), "the host's launch");

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] host started: mode %d city %d time %d weather %d\n",
			gMp.gamemode, gMp.city, gMp.timeOfDay, gMp.weather);

	/* A HUMAN JOINER PICKS THEIR CAR HERE.
	 *
	 * Joining a host that has not started yet is the ordinary LAN case: the client
	 * waits in the lobby, the host presses Start, and this is the moment the
	 * session becomes real. It used to launch straight into the level, so the
	 * joiner drove whatever mp.ini said and never saw the stock car select --
	 * while a joiner arriving at an ALREADY RUNNING match did see it, because
	 * MpHandleWelcome opens it there. One session, two different answers to
	 * "which car am I driving".
	 *
	 * The car select's Select is the START this module claims (mp.c), and that
	 * claim launches us through MpClientLaunch, so everything adopted above is
	 * exactly what it reads. The LEVEL is named first because the screen lists a
	 * city's cars: the same ordering the live-match join uses.
	 *
	 * An AUTO session (-join / MP_AUTOSTART) has nobody to press anything, so it
	 * still launches on the spot -- that is every headless rig. */
	if (!gMp.autoSession)
	{
		GameLevel = gMp.city;

		if (gMp.timeOfDay >= 0)
			wantedTimeOfDay = gMp.timeOfDay;
		if (gMp.weather >= 0)
			wantedWeather = gMp.weather;

		if (gMpCtx)
			gMpCtx->jer_log(gMpCtx, "[mp] host started - pick a car\n");

		MpUiOpenCarSelect();
		return;
	}

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

		/* ONLY x/z/rotation come from the level-start record. This is NOT a live
		 * car: the hook fires from InitGameVariables, which has just rebuilt slot
		 * 0's record from levelstartpos (main.c), and the engine's own spawn loop
		 * below resolves the ground from the record's x/z. The height stays the
		 * engine's convention (0, main.c), never a y read from a car -- a y taken
		 * at one x/z and applied at another is the "late joiner above the host"
		 * report this path used to cause. */
		PlayerStartInfo[slot]->position.vy = 0;
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
static void MpMirrorSirenTick(void);
static int  MpSlotInOurBand(int slot);
static int  MpTrafficSlotOwnerRank(int slot);
static void MpTrafficContactPush(MP_PLAYER* me, int slot);
static long long MpDistSq64(int dx, int dz);
static void MpTestTrafficHitTick(void);
static void MpTestFelonyTick(void);
static void MpDriveRemotePed(MP_PLAYER* p);
static void MpKeepOurCarsFromTrafficAi(void);
static void MpSendColors(int whole);
static void MpTestCarChangeTick(void);
static void MpTestCityChangeTick(void);
static void MpTestCarCycleTick(void);
static void MpTestLeaveTick(void);
static void MpTestKickTick(void);
static void MpTestOnFootTick(void);
static int MpEnsureJerichoImported(void);
static int MpEnsureJerichoInstalled(void);
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

/* Sender binding for a per-player row.
 *
 * Every state message names players by id, and an id on the wire is just a
 * number. Without this check any peer could speak for any other player: send
 * a row naming someone else and the hub would store it, then republish it as
 * that player's own word.
 *
 * The rule differs by side because the TOPOLOGY does. The host is the hub: it
 * receives one peer's own rows (so a row must name that peer) and rebroadcasts
 * a merged set to everyone. A client only ever has ONE inbound connection --
 * the host -- and its merged set legitimately carries every player, so there
 * is nobody to bind against: `from` is the host, not the row's owner.
 *
 * `from` is -1 before a HELLO, which is exactly the window in which a stranger
 * should not be able to name anybody at all. */
static int MpRowOwnerOk(int from, int playerId)
{
	if (playerId < 0 || playerId >= MP_MAX_PLAYERS)
		return 0;

	if (!MpIsHost())
		return 1;	/* the host already validated it, and it is our only peer */

	return from >= 0 && playerId == from;
}

static void MpHandleInput(int connIndex, const unsigned char* p, int len)
{
	MP_INPUT h;
	int n, i, from;

	if (len < (int)sizeof(MP_INPUT))
		return;

	memcpy(&h, p, sizeof(h));

	from = MpConnPlayerId(connIndex);

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

		/* ...and a peer may only speak for ITSELF. Dropped rather than clamped:
		 * a row that names someone else is not a mistake, it is the spoof. */
		if (!MpRowOwnerOk(from, (int)r.playerId))
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] input: dropped a row from player %d claiming player %d\n",
					from, (int)r.playerId);

			continue;
		}

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

/* A push may only act on a RECENT pose of the target's own timeline. Twenty
 * frames is 667 ms at 30 Hz: enough for a 250 ms one-way link plus the 100 ms
 * playout buffer and a host relay, short enough that a hit from long ago cannot
 * move anybody. */
#define MP_HIT_MAX_AGE_FRAMES	20
#define MP_HIT_RX_RADIUS	MP_HIT_RADIUS	/* the same body scale, measured at ARRIVAL */

/* The largest impulse a WELL-BEHAVED sender can produce, per axis: the normal is
 * MP_FIXEDH-scaled and the push is capped at MP_HIT_MAX_PUSH. The wire carries an
 * int32, so without this ceiling a peer could add +/-2^31 to a car's velocity at
 * any distance -- see MpHandleHit. */
#define MP_HIT_IMPULSE_MAX	(MP_FIXEDH * MP_HIT_MAX_PUSH)

/* The hit counters go to the log once per this many sim frames. Every 300 frames
 * is 10 s at the engine's 30 Hz tick; the plan called this a per-minute line, but
 * mp_localpair runs are commonly 40 s, so a 60 s line would never appear in
 * exactly the runs that need it. */
#define MP_HIT_SUMMARY_FRAMES	300


/* Contacts arm afresh in every match (see gMpHitArmFrame at the top of this file
 * for why the arm frame is session state and not a function local). */
static int MpHitArmed(void)
{
	if (gMpHitArmFrame == 0)
		gMpHitArmFrame = gMp.frame + MP_HIT_ARM_FRAMES;

	return (gMp.frame >= gMpHitArmFrame);
}

/* How far the sender's view of us may be from where we recorded ourselves at the
 * same frame. When the sample arrived it is EXACT (the sender displays a received
 * sample), so this is mostly room for the interpolation error of a lost sample;
 * half a car body per side makes it scale with the vehicles. */
static int MpHitPoseTolerance(const CAR_DATA* target, const MP_PLAYER* src)
{
	int tol = 300;

	if (target != NULL && target->ap.carCos != NULL)
		tol += target->ap.carCos->colBox.vz / 8;

	if (src != NULL && src->carId >= 0 && src->carId < MAX_CARS &&
	    car_data[src->carId].ap.carCos != NULL)
		tol += car_data[src->carId].ap.carCos->colBox.vz / 8;

	return tol;
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

	/* Name WHERE and WHEN in the target's own timeline. The playout state is
	 * what we actually collided with; if this machine has not played the target
	 * yet, leave 0 and the receiver will refuse an unvalidatable push. */
	{
		MP_CAR_PLAY* tp = &gMpPlay[pl->id];

		if (tp->primed && tp->playFrame != 0)
		{
			h.targetFrame = (uint32_t)tp->playFrame;
			h.tgtPos[0] = tp->playX;
			h.tgtPos[1] = tp->playY;
			h.tgtPos[2] = tp->playZ;
		}
		else
		{
			h.tgtPos[0] = other->hd.where.t[0];
			h.tgtPos[1] = other->hd.where.t[1];
			h.tgtPos[2] = other->hd.where.t[2];
		}
	}

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

	gMpHitSent++;

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

		/* The probe is the FALLBACK for a snapshot gap. While the owner's samples
		 * are arriving, the engine's own contact is the trigger -- and a
		 * distance-only test at this radius fires on near misses (measured in
		 * the docs at ~68 probe hits against 3-5 engine contacts), so most
		 * "phantom bumps" came from here. A gap is exactly the case the probe
		 * exists for, so gate on the newest SAMPLE's arrival, not lastStateFrame
		 * (which the apply path refreshes every frame even while holding). */
		{
			MP_CAR_PLAY* bp = &gMpPlay[pl->id];

			if (bp->count > 0 &&
			    (unsigned)(gMp.frame - bp->snap[bp->head].recvFrame) <= 2)
				continue;
		}

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
	int from, i;

	if (len < (int)sizeof(h))
		return;

	memcpy(&h, p, sizeof(h));

	/* Clamp BEFORE anything acts on it. Nothing here checked the magnitude, and a
	 * contact is reported by the peer that felt it -- so one message could have
	 * flung any car it named across the map, from any distance. Clamped rather than
	 * dropped so an over-large value still behaves like the hardest legal hit. */
	for (i = 0; i < 3; i++)
	{
		if (h.impulse[i] > MP_HIT_IMPULSE_MAX)
			h.impulse[i] = MP_HIT_IMPULSE_MAX;
		else if (h.impulse[i] < -MP_HIT_IMPULSE_MAX)
			h.impulse[i] = -MP_HIT_IMPULSE_MAX;
	}

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
		MP_PLAYER* src = MpGetPlayer(from);
		long d = -1;

		/* How far apart the two cars are ON THIS MACHINE when the push lands.
		 * Without a contact timestamp on the wire this is the only honest
		 * "was this hit stale?" measurement available to the receiver -- and it
		 * is the one the latency work moves: before interpolation/validation a
		 * delayed push commonly arrives with the cars already separated. */
		if (src != NULL && src->carId >= 0 && src->carId < MAX_CARS)
		{
			CAR_DATA* sc = &car_data[src->carId];
			long long dx = (long long)sc->hd.where.t[0] - (long long)cp->hd.where.t[0];
			long long dy = (long long)sc->hd.where.t[1] - (long long)cp->hd.where.t[1];
			long long dz = (long long)sc->hd.where.t[2] - (long long)cp->hd.where.t[2];

			d = (long)sqrt((double)(dx * dx + dy * dy + dz * dz));
		}

		gMpHitRx++;

		if (d > MP_HIT_RADIUS)
			gMpHitSeparated++;

		if (MpDebugOn() && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] hit rx: player %d |d|=%ld srcAge=%lu tf=%u push=%ld,%ld,%ld\n",
				from, d,
				(src != NULL && src->lastStateFrame != 0)
					? (unsigned long)(gMp.frame - src->lastStateFrame) : 0UL,
				(unsigned)h.targetFrame,
				(long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);

		/* VALIDATE BEFORE APPLYING. targetFrame is a frame of OUR OWN carstate,
		 * so we can ask two questions no arrival-time check could answer:
		 *  - is this recent enough to act on at all?
		 *  - were we actually where the sender says, when it says?
		 * A push that fails either is a ghost: the sender collided with a pose
		 * this machine had already left, and applying it now is the car that
		 * bounces off nothing. */
		{
			int age = (h.targetFrame != 0)
				? (int)((unsigned)gMp.frame - h.targetFrame) : 9999;
			int hx = 0, hy = 0, hz = 0;
			long poseD = -1;
			const char* why = NULL;

			if (h.targetFrame == 0)
				why = "no timeline";
			else if (age < 0 || age > MP_HIT_MAX_AGE_FRAMES)
				why = "stale";
			else if (!MpSelfPoseAt(h.targetFrame, &hx, &hy, &hz))
				why = "frame not in history";
			else
			{
				long long pdx = (long long)hx - h.tgtPos[0];
				long long pdy = (long long)hy - h.tgtPos[1];
				long long pdz = (long long)hz - h.tgtPos[2];

				poseD = (long)sqrt((double)(pdx * pdx + pdy * pdy + pdz * pdz));

				if (poseD > MpHitPoseTolerance(cp, src))
					why = "pose mismatch";
			}

			/* A consistent timestamp is still not enough: by the time the push
			 * lands the cars may have finished separating. Applying it then is
			 * exactly the phantom bump, so a late push also has to find us
			 * still near the other car. */
			if (why == NULL && age > MP_HIT_SELF_FRAMES && d > MP_HIT_RX_RADIUS)
				why = "cars separated";

			if (why != NULL)
			{
				gMpHitRejected++;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] hit: player %d bumped us (push %ld,%ld,%ld) "
						"ghost hit rejected (%s: age=%d |d|=%ld pose=%ld)\n",
						from, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2],
						why, age, d, poseD);
				return;
			}
		}

		/* ONCE per contact. Our own engine resolves the same collision (both
		 * machines simulate both cars), and its rigid-body response is already in
		 * our velocity -- so adding the peer's push as well lands one collision
		 * twice. Compare the frame the engine saw it against targetFrame, both in
		 * OUR numbering: the old arrival-frame window is wrong across latency,
		 * because the push arrives many frames after the collision it describes. */
		if (from >= 0 && from < MP_MAX_PLAYERS && gMpSelfContactFrame[from] != 0 &&
			(int)h.targetFrame - (int)gMpSelfContactFrame[from] <= MP_HIT_SELF_FRAMES &&
			(int)gMpSelfContactFrame[from] - (int)h.targetFrame <= MP_HIT_SELF_FRAMES)
		{
			gMpHitKept++;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] hit: player %d bumped us (push %ld,%ld,%ld; our engine has it, kept)\n",
					from, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
			return;
		}

		gMpHitApplied++;

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

/* The hit counters, once per MP_HIT_SUMMARY_FRAMES. Counter-only: the increments
 * are unconditional and nearly free, this line is what MP_DEBUG pays for. */
static void MpHitSummary(void)
{
	if (gMp.frame == 0 || (gMp.frame % MP_HIT_SUMMARY_FRAMES) != 0)
		return;

	if (gMpHitSent == 0 && gMpHitRx == 0)
		return;

	if (MpDebugOn() && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] hit summary: sent=%lu rx=%lu applied=%lu kept=%lu separated=%lu rejected=%lu\n",
			gMpHitSent, gMpHitRx, gMpHitApplied, gMpHitKept, gMpHitSeparated, gMpHitRejected);
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
static const char* gTestKickStr;
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
	gTestKickStr = getenv("MP_TEST_KICK");
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
#define MP_TRAFFIC_SLOT_FIRST	JER_SLOT_TRAFFIC_FIRST			/* 20: above the player and managed pools */
#define MP_TRAFFIC_SLOT_COUNT	JER_SLOT_TRAFFIC_COUNT			/* 12 traffic slots, fixed */

static unsigned char gMpBandReserved[MAX_CARS];	/* slots WE turned on, so we clear only ours */
/* THE COP SLOT HOLD. Civilians and cops share this machine's band, and in
 * single player the engine never runs the pool dry (19 free slots against a
 * civilian cap of 13), so a cop request always finds a slot. A band is at most
 * 12 slots and the cap does not shrink with it, so traffic fills every slot and
 * a cop has to wait for a civilian to drive out of range -- which, for players
 * staying in one area, is a long time. While the session has cops on and fewer
 * than maxCopCars are out, the LAST free slot of the band is held back from the
 * civilian spawner (reservedSlots, like the band), and released on the frame a
 * cop is requested so the cop takes it. -1 = holding nothing. */
static int gMpCopHold = -1;

static void MpCopHoldRelease(void)
{
	if (gMpCopHold >= 0 && !gMpBandReserved[gMpCopHold])
		reservedSlots[gMpCopHold] = 0;

	gMpCopHold = -1;
}

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
 * the remainder so the whole pool is covered even when it does not divide.
 *
 * More machines than slots means there is no private slot to hand out: the machine
 * gets an EMPTY band (first = last = -1) and spawns no traffic of its own, living
 * off its peers' mirrors instead. The old code clamped every extra machine onto
 * the pool's LAST slot, so two machines spawned into one slot and then mirrored
 * each other's car into it. */
static void MpTrafficBandFor(int rank, int nMachines, int* first, int* last)
{
	int per, f, l;

	if (rank < 0 || rank >= MP_TRAFFIC_SLOT_COUNT || nMachines > MP_TRAFFIC_SLOT_COUNT)
	{
		*first = -1;
		*last = -1;
		return;
	}

	per = MP_TRAFFIC_SLOT_COUNT / nMachines;

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

	MpCopHoldRelease();

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

/* MP_TEST_FELONY=<level> -- keep a WANTED LEVEL up, so the ENGINE spawns police
 * cars and they pursue (inert unless set).
 *
 * Why this exists: the siren path only runs for a cop that is actually chasing
 * somebody, and a cop car turns up in a pair run "after a while" if it turns up
 * at all -- which makes the one behaviour worth checking unreproducible. Raising
 * the felony is the engine's OWN trigger, so the cars that appear are the ones a
 * player would get (PURSUER_AI, on duty), not a module-spawned stand-in that
 * could pass while the real path stays broken. It is re-asserted every 30 frames
 * because the engine decays it once nobody is being chased. */
static void MpTestFelonyTick(void)
{
	static int level = -2;
	static unsigned nextFrame = 0;
	MP_PLAYER* me = MpLocalPlayer();

	if (level == -2)
	{
		const char* e = getenv("MP_TEST_FELONY");
		level = (e != NULL) ? atoi(e) : -1;
	}

	if (level < 0 || !gMp.running || me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	if (nextFrame != 0 && gMp.frame < nextFrame)
		return;

	nextFrame = gMp.frame + 30;

	car_data[me->carId].felonyRating = (u_char)(level > 4 ? 4 : level);
	pedestrianFelony = car_data[me->carId].felonyRating;
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

	/* Everything OUTSIDE our band is kept off our spawner: the player and
	 * JERICHO-managed reserve [0..JER_SLOT_TRAFFIC_FIRST) because a civilian there
	 * would squat on a slot a player (or a managed vehicle, or a late joiner)
	 * needs, and every other machine's band because its cars get mirrored into
	 * those slots and OUR spawner must never race the peer for one.
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

	/* The cop slot hold (see gMpCopHold): re-decided every frame, before the
	 * spawner runs. Only with the session's cops on, so a no-cops session and
	 * single player keep every slot for traffic. */
	MpCopHoldRelease();

	if (f >= 0 && gMp.ruleSet && gMp.ruleCops && CopsAllowed &&
	    numCopCars < maxCopCars && !requestCopCar)
	{
		int freeCount = 0, lastFree = -1;

		for (i = f; i <= l; i++)
		{
			if (car_data[i].controlType == CONTROL_TYPE_NONE && !reservedSlots[i])
			{
				freeCount++;
				lastFree = i;
			}
		}

		if (freeCount == 1)
		{
			reservedSlots[lastFree] = 1;
			gMpCopHold = lastFree;
		}
	}

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
			continue;	/* PURSUER_AI (a cop in pursuit) is never handed off */

		/* Nor a patrolling cop: a cop off duty is a CIV_AI car with CONTROL_FLAG_COP
		 * (dr2types.h). It is OUR cop, there for OUR player; the peer's copy of the
		 * area is not chasing us, so handing it off deleted our police. */
		if (cp->controlFlags & CONTROL_FLAG_COP)
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
#define MP_INTERP_DELAY		3	/* playout delay, in 30 Hz frames (100 ms) */
#define MP_INTERP_EXTRAP_MAX	3	/* never extrapolate further than this */
#define MP_INTERP_RESYNC	8	/* cursor this far behind: jump to newest - DELAY */
#define MP_INTERP_SNAP_DIST	6000	/* one-sample jump: a teleport, not motion */
#define MP_INTERP_DEBUG_FRAMES	60	/* MP_DEBUG state line every 2 s */

/* MP_CAR_SNAP / MP_CAR_PLAY / gMpPlay are declared at the top of the file: the
 * contact path reads the playout state before this code in file order. */

/* Signed distance between two frame numbers. The counters are 32-bit on this
 * platform and a match lasts minutes, so a plain subtraction is exact; the
 * casts just keep the wrap arithmetic out of the compiler's way. */
static int MpFrameAge(unsigned newer, unsigned older)
{
	return (int)((unsigned)newer - (unsigned)older);
}

static void MpPlayClear(int id)
{
	if (id >= 0 && id < MP_MAX_PLAYERS)
		memset(&gMpPlay[id], 0, sizeof(gMpPlay[id]));
}

/* ------------------------------------------------------------------ */
/* Our own pose history                                                */
/*                                                                     */
/* A push names the frame of OUR carstate it collided with. To decide   */
/* whether we were really where it says, we need where we were at that  */
/* frame -- recorded as the pose we sent, because that is exactly what  */
/* the sender could have been displaying. 64 frames is just over two    */
/* seconds at 30 Hz, comfortably more than the validity window.         */
/* ------------------------------------------------------------------ */
#define MP_SELF_HISTORY	64

typedef struct MP_SELF_POSE
{
	unsigned frame;
	int x, y, z;
} MP_SELF_POSE;

static MP_SELF_POSE gMpSelfPose[MP_SELF_HISTORY];

static void MpPlayResetAll(void)
{
	memset(gMpPlay, 0, sizeof(gMpPlay));
	memset(gMpSelfPose, 0, sizeof(gMpSelfPose));
}

static void MpRecordSelfPose(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	MP_SELF_POSE* h;

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	h = &gMpSelfPose[gMp.frame % MP_SELF_HISTORY];
	h->frame = (unsigned)gMp.frame;
	h->x = car_data[me->carId].hd.where.t[0];
	h->y = car_data[me->carId].hd.where.t[1];
	h->z = car_data[me->carId].hd.where.t[2];
}

static int MpSelfPoseAt(unsigned frame, int* x, int* y, int* z)
{
	const MP_SELF_POSE* h = &gMpSelfPose[frame % MP_SELF_HISTORY];

	if (h->frame != frame)
		return 0;

	*x = h->x;
	*y = h->y;
	*z = h->z;
	return 1;
}

/* Store one sample. Older/duplicate frames are dropped: TCP is ordered, but a
 * relayed snapshot can still arrive after a newer one from another path. */
static void MpPlayPush(int id, unsigned frame, const MP_CARSTATE_ENTRY* e)
{
	MP_CAR_PLAY* p;
	MP_CAR_SNAP* s;
	int i;

	if (id < 0 || id >= MP_MAX_PLAYERS || e == NULL)
		return;

	p = &gMpPlay[id];

	if (p->count > 0 && MpFrameAge(frame, (unsigned)p->lastFrame) <= 0)
		return;

	/* A jump this large in one sample is not motion: a respawn, a car change
	 * or a snapshot gap. Do not slide the body across the map -- start a fresh
	 * timeline, exactly as the old adopt did by simply writing the new pose. */
	if (p->count > 0)
	{
		MP_CAR_SNAP* newest = &p->snap[p->head];
		long long dx = (long long)e->x - newest->x;
		long long dy = (long long)e->y - newest->y;
		long long dz = (long long)e->z - newest->z;

		if (dx * dx + dy * dy + dz * dz >
		    (long long)MP_INTERP_SNAP_DIST * MP_INTERP_SNAP_DIST)
		{
			MpPlayClear(id);
			p = &gMpPlay[id];
		}
	}

	p->head = (p->head + 1) % MP_INTERP_RING;
	s = &p->snap[p->head];

	s->frame = frame;
	s->recvFrame = gMp.frame;
	s->x = e->x;
	s->y = e->y;
	s->z = e->z;
	s->heading = e->heading;

	for (i = 0; i < 4; i++)
		s->orient[i] = e->orient[i];
	for (i = 0; i < 3; i++)
		s->vel[i] = e->vel[i];
	for (i = 0; i < 3; i++)
		s->angVel[i] = e->angVel[i];

	if (p->count < MP_INTERP_RING)
		p->count++;

	p->lastFrame = (int)frame;

	if (!p->primed)
	{
		/* Start 3 frames in the past. With fewer samples than that the apply
		 * step holds the oldest one until the cursor reaches it, which is the
		 * 100 ms of delay this whole scheme is built on. */
		p->playFrame = (int)frame - MP_INTERP_DELAY;
		p->primed = 1;
	}
}

static void MpSnapUse(const MP_CAR_SNAP* s, int* x, int* y, int* z,
	int* heading, short orient[4], int vel[3], int angVel[3])
{
	int i;

	*x = s->x;
	*y = s->y;
	*z = s->z;
	*heading = s->heading;

	for (i = 0; i < 4; i++)
		orient[i] = s->orient[i];
	for (i = 0; i < 3; i++)
		vel[i] = s->vel[i];
	for (i = 0; i < 3; i++)
		angVel[i] = s->angVel[i];
}

static int MpLerpAngle(int a, int b, int num, int den)
{
	int d = ((b - a + 2048) & 4095) - 2048;

	return (a + d * num / den) & 4095;
}

/* Write a remote car's rigid body. The same fields the old adopt wrote: the
 * engine re-derives hd.direction from the quaternion, so the orientation and
 * both velocities have to travel with the position. */
static void MpPlayWriteBody(CAR_DATA* cp, int x, int y, int z, int heading,
	const short orient[4], const int vel[3], const int angVel[3])
{
	LONGQUATERNION q;
	MATRIX m;
	int i;

	cp->controlType = CONTROL_TYPE_PLAYER;

	for (i = 0; i < 4; i++)
		q[i] = orient[i];
	for (i = 0; i < 4; i++)
		cp->st.n.orientation[i] = q[i];
	for (i = 0; i < 3; i++)
		cp->st.n.linearVelocity[i] = vel[i];
	for (i = 0; i < 3; i++)
		cp->st.n.angularVelocity[i] = MpClampAngVel(angVel[i]);

	LongQuaternion2Matrix(&q, &m);
	m.t[0] = x;
	m.t[1] = y;
	m.t[2] = z;
	memcpy(&cp->hd.where, &m, sizeof(m));

	cp->hd.direction = heading;
}

/* Once per sim frame, before the world steps: place every remote car at the
 * pose the playout cursor names. This is the ONLY writer of a remote car's
 * body now, so the engine and the snapshot stream cannot fight over it. */
static void MpApplyRemoteCarPoses(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* pl = &gMp.players[i];
		MP_CAR_PLAY* p;
		MP_CAR_SNAP* s0 = NULL;	/* first sample at or older than the cursor */
		MP_CAR_SNAP* s1 = NULL;	/* first sample at or newer than the cursor */
		const MP_CAR_SNAP* newest;
		CAR_DATA* cp;
		int x, y, z, heading;
		int vel[3], angVel[3];
		short orient[4];
		int k;

		if (!pl->active || pl->isLocal || pl->carId < 0 || pl->carId >= MAX_CARS)
			continue;

		p = &gMpPlay[pl->id];

		if (p->count == 0)
			continue;

		cp = &car_data[pl->carId];

		if (!p->primed)
		{
			p->playFrame = p->lastFrame - MP_INTERP_DELAY;
			p->primed = 1;
		}
		else
		{
			p->playFrame++;
		}

		/* A long stall runs the cursor ahead of what we have. Clamp it, and
		 * re-anchor when the data that finally arrives is far newer than it. */
		if (p->playFrame > p->lastFrame + MP_INTERP_EXTRAP_MAX)
			p->playFrame = p->lastFrame + MP_INTERP_EXTRAP_MAX;

		if (MpFrameAge((unsigned)p->lastFrame, (unsigned)p->playFrame) > MP_INTERP_RESYNC)
			p->playFrame = p->lastFrame - MP_INTERP_DELAY;

		/* Walk newest -> oldest to bracket the cursor. */
		for (k = 0; k < p->count; k++)
		{
			MP_CAR_SNAP* s = &p->snap[(p->head - k + MP_INTERP_RING) % MP_INTERP_RING];

			if (MpFrameAge(s->frame, (unsigned)p->playFrame) >= 0)
				s1 = s;		/* at or newer than the cursor */
			else
			{
				s0 = s;		/* the first one older than it */
				break;
			}
		}

		newest = &p->snap[p->head];

		if (s0 != NULL && s1 != NULL)
		{
			int den = MpFrameAge(s1->frame, s0->frame);
			int num = MpFrameAge((unsigned)p->playFrame, s0->frame);
			long long dot = 0;
			int sign;
			int j;

			if (den <= 0)
				den = 1;
			if (num < 0)
				num = 0;
			if (num > den)
				num = den;

			x = (int)((long long)s0->x * (den - num) / den + (long long)s1->x * num / den);
			y = (int)((long long)s0->y * (den - num) / den + (long long)s1->y * num / den);
			z = (int)((long long)s0->z * (den - num) / den + (long long)s1->z * num / den);
			heading = MpLerpAngle(s0->heading, s1->heading, num, den);

			/* Normalised lerp with a sign correction: q and -q are the same
			 * attitude, and lerping without it takes the long way round and
			 * can flip the car. */
			for (j = 0; j < 4; j++)
				dot += (long long)s0->orient[j] * s1->orient[j];

			sign = (dot < 0) ? -1 : 1;

			for (j = 0; j < 4; j++)
			{
				orient[j] = (short)(((long long)s0->orient[j] * (den - num) +
					(long long)s1->orient[j] * sign * num) / den);
			}

			for (j = 0; j < 3; j++)
				vel[j] = s1->vel[j];
			for (j = 0; j < 3; j++)
				angVel[j] = s1->angVel[j];

			p->mode = 0;
		}
		else if (s1 != NULL)
		{
			/* Before the oldest sample: show it (the spawn case). */
			MpSnapUse(s1, &x, &y, &z, &heading, orient, vel, angVel);
			p->mode = 1;
		}
		else
		{
			/* Beyond the newest sample: extrapolate on its velocity, capped. */
			int dt = MpFrameAge((unsigned)p->playFrame, newest->frame);

			if (dt < 0)
				dt = 0;
			if (dt > MP_INTERP_EXTRAP_MAX)
				dt = MP_INTERP_EXTRAP_MAX;

			MpSnapUse(newest, &x, &y, &z, &heading, orient, vel, angVel);

			x += (int)((long long)vel[0] * dt / MP_FIXEDH);
			y += (int)((long long)vel[1] * dt / MP_FIXEDH);
			z += (int)((long long)vel[2] * dt / MP_FIXEDH);

			p->mode = 2;
		}

		MpPlayWriteBody(cp, x, y, z, heading, orient, vel, angVel);

		/* Remember the pose the sender is actually looking at: a contact report
		 * names this frame, and MpContactPush sends this position with it. */
		p->playX = x;
		p->playY = y;
		p->playZ = z;

		pl->onFoot = 0;
		pl->lastStateFrame = gMp.frame;	/* the fallback gate in MpOnNetInput reads this */

		if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % MP_INTERP_DEBUG_FRAMES) == 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] interp: player %d depth=%d lag=%d mode=%s\n",
				pl->id, p->count, MpFrameAge((unsigned)p->lastFrame, (unsigned)p->playFrame),
				(p->mode == 0) ? "lerp" : (p->mode == 1) ? "hold" : "extrap");
	}
}

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

/* MP_TEST_KICK=<secs> -- the HOST kicks its first client that many seconds in,
 * through the same MpKickPlayer the pause menu will call, so the wire path and
 * the kicked client's teardown are checkable from the log. Host only: running
 * it on both machines would make the two logs ambiguous. */
static void MpTestKickTick(void)
{
	static int done;
	static unsigned long startMs;
	const char* s;
	unsigned long now = MpNowMs();

	if (done || !gMp.running)
		return;

	if (gMp.role != MP_ROLE_HOST)
		return;

	s = gTestKickStr;

	if (s == NULL)
		return;

	if (startMs == 0)
		startMs = now;

	if ((now - startMs) < (unsigned long)(atoi(s) * 1000))
		return;

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: host kicking player 1 %ss in (MP_TEST_KICK)\n", s);

	MpKickPlayer(1);
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
	static int passLanded;	/* of this pass, how many actually landed */
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

		/* JERICHO: the row capacity is the bound, but a bank row that is not
		 * registered is not a city - a configured bank with no files leaves a hole,
		 * and cycling a car out of it would build nothing on any peer. */
		for (city = 0; city < CITY_COUNT && total < MP_TEST_CYCLE_MAX_CARS; city++)
		{
			int models[MP_CAR_LIST_MAX];
			int slots[MP_CAR_LIST_MAX];
			int count;
			int i;

			if (!JerCityRegistered(city))
				continue;

			count = MpCarListForCity(city, slots, models, MP_CAR_LIST_MAX);

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

	/* The outcome is what the rig is FOR, and it is not a given: a change can be waiting
	 * for a spare slot (MpChangeCar remembers it and retries) or refused outright. So
	 * say which it was. An earlier version told the rig every car had been driven
	 * because it assumed the call always landed -- the rig then compared THAT against
	 * a list of asks, and a run that changed nothing still read as full coverage. */
	if (MpChangeCar(cars[idx][0], cars[idx][1]))
	{
		passLanded++;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] test: car cycle: drove %s model %d\n",
				MpCarCityName(cars[idx][0]), cars[idx][1]);
	}

	idx++;

	if (idx < total)
		return;

	pass++;
	idx = 0;
	MpTestShuffle(cars, total, &seed);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: car cycle PASS %d done - %d of the %d car(s) landed as asked\n",
			pass, passLanded, total);

	passLanded = 0;

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

const char* MpPedModelName(int model)
{
	switch (model)
	{
	case MP_PED_MODEL_TANNER:	return "Tanner";
	case MP_PED_MODEL_JERICHO:	return "Jericho";
	case MP_PED_MODEL_COP:		return "Cop";
	case MP_PED_MODEL_CIVILIAN:	return "Civilian";
	default:			return "?";
	}
}

int MpPedModelAvailable(int model)
{
	int i;

	switch (model)
	{
	case MP_PED_MODEL_TANNER:
		return 1;

	case MP_PED_MODEL_JERICHO:
		for (i = 0; i < 6; i++)
		{
			if (pmJerichoModels[i] == NULL)
				return 0;
		}

		/* Native Havana models are already resident; imported models are ready
		 * only when their pages and palette are in place. */
		return (GameLevel == 1) ? 1 : JerPedImportReady();

	default:
		return 0;
	}
}

static int sMpJerPedInstalledLevel = -1;

/* UI-facing: may this character be OFFERED here? Jericho is selectable even
 * before it is resident, because selecting it is what triggers the import. */
int MpPedModelSelectable(int model)
{
	if (model == MP_PED_MODEL_TANNER)
		return 1;

	if (model == MP_PED_MODEL_JERICHO)
		return MpPedModelAvailable(model) || JerPedBakedAvailable() || JerPedImportProbe(1);

	return 0;
}

int MpPedModelEffective(int model)
{
	if (!MpPedModelAvailable(model))
		return MP_PED_MODEL_TANNER;

	/* Jericho's models can be resident while its palette pairs have not been
	 * appended yet (native Havana). Do not draw a half-installed character. */
	if (model == MP_PED_MODEL_JERICHO && sMpJerPedInstalledLevel != GameLevel)
		return MP_PED_MODEL_TANNER;

	return model;
}

int MpPedModelSelected(void)
{
	return gMp.config.pedModel;
}

/* The skeleton a character uses. Only the two skeleton families are selectable
 * today. */
static int MpPedTypeForModel(int model)
{
	return (MpPedModelEffective(model) == MP_PED_MODEL_JERICHO) ? OTHER_MODEL : TANNER_MODEL;
}

/* Put the local player's active palette choice back on the selector. The draw
 * (MpOnPedDraw) recomputes it every frame; doing it here means the frame that
 * follows a skeleton swap is already right. */
static void MpSelectLocalPalette(void)
{
	if (!gMp.config.colorOn)
	{
		jer_ped_palette_select(-1);
		return;
	}

	jer_ped_palette_select(
		jer_ped_palette_team_colour(
			jer_colour_make(gMp.config.colorR, gMp.config.colorG, gMp.config.colorB),
			MP_COLOR_STRENGTH));
}

/* Import Jericho on demand. One refusal per level is remembered so a stored
 * Jericho on a machine without Havana's data does not retry every frame; an
 * explicit re-selection clears the flag. */
static int sMpJerPedImportFailed;
static int sMpJerPedImportLevel = -1;

/* Make the resident six models usable for drawing: append their palette pairs,
 * rebuild the Jericho vertex list and rebind the skeleton. One success per
 * level; a failure leaves MpPedModelEffective on Tanner. */
static int MpEnsureJerichoInstalled(void)
{
	if (!MpPedModelAvailable(MP_PED_MODEL_JERICHO))
		return 0;

	if (sMpJerPedInstalledLevel == GameLevel)
		return 1;

	if (!JerPedInstallModels())
		return 0;

	sMpJerPedInstalledLevel = GameLevel;
	return 1;
}

static int MpEnsureJerichoImported(void)
{
	if (MpPedModelAvailable(MP_PED_MODEL_JERICHO))
		return MpEnsureJerichoInstalled();

	if (sMpJerPedImportLevel != GameLevel)
	{
		sMpJerPedImportLevel = GameLevel;
		sMpJerPedImportFailed = 0;
		sMpJerPedInstalledLevel = -1;
	}

	if (sMpJerPedImportFailed)
		return 0;

	/* The baked JMPPED.LEV is self-contained and is the shipping path; the live
	 * Havana import remains as a diagnostic fallback. */
	if (JerPedBakedAvailable())
	{
		if (JerPedImportBaked(GameLevel) && MpEnsureJerichoInstalled())
			return 1;

		JerPedImportReset();
	}

	if (!JerPedImportProbe(1) || !JerPedImportBuild(1) || !MpEnsureJerichoInstalled())
	{
		sMpJerPedImportFailed = 1;
		JerPedImportReset();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] character: Jericho import failed on level %d - staying on Tanner\n",
				GameLevel);

		return 0;
	}

	return MpPedModelAvailable(MP_PED_MODEL_JERICHO);
}

/* Apply the chosen character to OUR pedestrian, if we have one. While in a car
 * there is no ped; the next MpSendOwnPedState on foot calls this again, which is
 * exactly "apply when you next get out". */
static void MpApplyLocalPedModel(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	LPPEDESTRIAN ped;
	int model, type, gen;
	static int sLocalSkelGen = -1;

	if (me == NULL || me->carId >= 0)
		return;

	ped = MpLocalPed();

	if (ped == NULL)
		return;

	/* A stored Jericho that was chosen before the level loaded is imported here,
	 * on the first frame the player actually has a pedestrian. */
	if (gMp.config.pedModel == MP_PED_MODEL_JERICHO)
	{
		MpEnsureJerichoImported();
		MpEnsureJerichoInstalled();
	}

	model = MpPedModelEffective(gMp.config.pedModel);
	type = MpPedTypeForModel(model);
	gen = JerPedSkeletonGeneration();

	if (ped->pedType == type && sLocalSkelGen == gen)
		return;

	ped->pedType = (char)type;

	/* THE SAME PATH InitTanner AND THE MOTION CODE USE. This rebinds Skel[].pModel
	 * and bodyShiftValue together, so the bones, the vertex lists
	 * (cJerichoVNumbers) and the draw code stay one consistent skeleton. */
	SetSkelModelPointers(type);

	MpSelectLocalPalette();
	sLocalSkelGen = gen;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] character: local player is now %s (pedType %d)\n",
			MpPedModelName(model), type);
}

/* Owner -> peers: persist the choice, apply it if we are on foot, and tell
 * everyone. A client's JPCL row goes to the host, which republishes the table;
 * the host sends the table itself. JPPD carries the same id on the next pose, so
 * an on-foot swap is visible on the frame that changes it. */
int MpSetPedModel(int model)
{
	MP_PLAYER* me;
	if (model < 0 || model >= MP_PED_MODEL_COUNT)
		return 0;

	if (!MpPedModelSelectable(model))
	{
		MpNotify(model == MP_PED_MODEL_JERICHO
			? "Jericho is unavailable (Havana data is not installed)"
			: "That character is not available");

		return 0;
	}

	/* An explicit pick is also a retry: a previous import failure must not keep
	 * Jericho refused for the rest of the level. */
	sMpJerPedImportLevel = GameLevel;
	sMpJerPedImportFailed = 0;

	if (model == MP_PED_MODEL_JERICHO && !MpPedModelAvailable(MP_PED_MODEL_JERICHO))
	{
		/* At the frontend there is no level to pin pages into. Persist the choice;
		 * MpApplyLocalPedModel imports it on the first on-foot frame. */
		if (gMp.running && !MpEnsureJerichoImported())
		{
			MpNotify("Jericho could not be imported - staying on the current character");

			return 0;
		}
	}

	if (gMp.running && model == MP_PED_MODEL_JERICHO && !MpEnsureJerichoInstalled())
	{
		MpNotify("Jericho could not be prepared - staying on the current character");
		return 0;
	}

	gMp.config.pedModel = model;
	MpConfigSave();

	me = MpLocalPlayer();

	if (me != NULL)
		me->pedModel = model;

	if (gMp.running)
	{
		MpApplyLocalPedModel();
		MpSendColors(0);	/* identity travels with colour */
		MpSendOwnPedState();	/* and with the pose, when there is one */
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] character: chose %s\n", MpPedModelName(model));

	return 1;
}

/* Headless verification lever: import Jericho at level start, regardless of the
 * player being in a car or a session. Inert unless MP_TEST_JERICHO is set. */
void MpTestJerichoImport(void)
{
	const char* s = getenv("MP_TEST_JERICHO");
	int ok;

	if (s == NULL || s[0] == '0' || GameLevel < 0)
		return;

	ok = MpEnsureJerichoImported() && MpEnsureJerichoInstalled();

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_JERICHO import %s (level %d)\n",
			ok ? "succeeded" : "FAILED", GameLevel);
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

	/* We may have got out of a car since the choice was made (or the level may
	 * have changed under a stored Havana-only pick). This is the "apply the next
	 * time they exit" point. */
	MpApplyLocalPedModel();

	memset(&h, 0, sizeof(h));
	memset(&e, 0, sizeof(e));

	h.count = 1;

	e.playerId = (uint8_t)me->id;
	e.flags = (uint8_t)(p->speed != 0 ? MP_PED_MOVING : 0);
	e.pedModel = (uint8_t)MpPedModelEffective(gMp.config.pedModel);
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
	int i, n, forged = 0;

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

		/* A pose may only describe its SENDER (the dedi enforces the same rule):
		 * otherwise one client could walk another player's Tanner around. */
		if (MpIsHost() && connIndex >= 0 && e.playerId != MpConnPlayerId(connIndex))
		{
			forged = 1;
			continue;
		}

		pl->pedX = e.x;
		pl->pedY = e.y;
		pl->pedZ = e.z;
		pl->pedHeading = (int)(e.heading & 0xFFF);
		pl->pedSpeed = e.speed;
		pl->pedModel = MpPedModelEffective((int)e.pedModel);
		pl->pedLastMs = MpNowMs();
	}

	/* The host is the hub, and this needs relaying exactly as the carstate does:
	 * a client sends its pedestrian to the host alone, so without this only the
	 * host ever sees an on-foot CLIENT -- every other client sees a gap where a
	 * person should be, which reads as "I can see the host on foot but not each
	 * other". The carstate relay (MpHandleCarState) is the same rule. */
	if (MpIsHost() && connIndex >= 0 && !forged)
		MpHostRelay(connIndex, MP_TAG_PED, 0, p, len);
}

/* Keep the pedestrian we are standing in for a REMOTE on-foot player in step,
 * spawning and despawning it as that player gets in and out. Every sim frame,
 * every player: building peds from a network callback would do it mid-poll. */
static void MpDriveRemotePed(MP_PLAYER* p)
{
	JerNpc* n;
	LPPEDESTRIAN ped;
	int dx, dz, err, gen;

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

	/* A remote owner may have chosen Jericho. Import it here, on the machine
	 * that has to draw the stand-in, before choosing the ped type. A failure
	 * falls back to Tanner through MpPedModelEffective and is remembered for the
	 * level, so a peer without Havana's data does not retry every frame. */
	if (p->pedModel == MP_PED_MODEL_JERICHO && !MpPedModelAvailable(MP_PED_MODEL_JERICHO))
		MpEnsureJerichoImported();

	if (p->pedModel == MP_PED_MODEL_JERICHO)
		MpEnsureJerichoInstalled();

	gen = JerPedSkeletonGeneration();

	if (n != NULL)
	{
		int want = MpPedTypeForModel(p->pedModel);

		if (((LPPEDESTRIAN)n)->pedType != want || p->pedSkelGen != gen)
		{
			/* The owner changed character. Rebuild the stand-in rather than
			 * mutating a ped whose motion/vertex state belongs to the old model:
			 * the spawn path is the same one a fresh player takes, and the
			 * skeleton is rebound by the draw code on its first frame. */
			jer_npc_despawn(n);
			p->ped = NULL;
			n = NULL;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] ped: player %d changed character to %s; stand-in rebuilt\n",
					p->id, MpPedModelName(p->pedModel));
		}
	}

	if (n == NULL)
	{
		n = jer_npc_spawn_model(MpPedTypeForModel(p->pedModel), p->pedX, p->pedZ);

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
		p->pedSkelGen = gen;

		/* A stand-in is NOT a local player's ped: mark it so a padId-keyed scan (or
		 * anything else that treats padId >= 0 as "someone's character") can never
		 * mistake it for ours -- and so AnimatePed (jer_npc_tick) writes the unused
		 * player slot, never the local player's. jer_npc_spawn now stamps -1 on every
		 * module-spawned ped, so this is belt-and-braces rather than the only guard:
		 * it was the only one when a pooled slot could inherit the local player's
		 * padId (which is how a remote Tanner once took the player's colour). */
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

	/* Mirror the owner's gait directly: their heading, their speed (negative =
	 * backing up), and the idle/walk/run clip that goes with it. The old
	 * move_to aimed at a point 300 units along the owner's heading but measured
	 * from the OWNER, so a lagging stand-in flipped to face the far side and the
	 * clip/reverse flag were never set -- the "stuck walking backwards" Tanner.
	 * A pose older than 400 ms means the owner stopped sending: stand idle
	 * rather than walk on forever. */
	{
		int spd = p->pedSpeed;

		if (MpNowMs() - p->pedLastMs > 400)
			spd = 0;

		jer_npc_set_gait(n, p->pedHeading, spd);
	}

	/* NOW animate + integrate it. Setting the intent is not enough: this ped is
	 * not CIVILIAN, so neither the ambient pedestrian updater (ControlPedestrians)
	 * nor the player-ped loop runs its state function -- and those are the only
	 * callers of AnimatePed. Nothing advanced its animation or moved it, so it
	 * stood still until the err > 120 catch-up above snapped it onto the owner:
	 * that is what "Tanner teleports, no walk animation" was. jer_npc_tick runs
	 * the pass a pedestrian's own state function would, so the stand-in walks with
	 * its legs moving. */
	jer_npc_tick(n);

	/* Evidence line: the stand-in's OWN position (what AnimatePed just integrated)
	 * next to the owner's. A walking stand-in TRACKS the owner; a stuck one stays
	 * put and the "corrected" line above fires repeatedly. */
	if (gMpCtx != NULL && MpDebugOn())
	{
		static unsigned long lastPedMs;

		if ((MpNowMs() - lastPedMs) > 2000)
		{
			lastPedMs = MpNowMs();
			gMpCtx->jer_log(gMpCtx,
				"[mp] ped: player %d stand-in at %d,%d,%d (owner %d,%d,%d) speed %d\n",
				p->id, ped->position.vx, ped->position.vy, ped->position.vz,
				p->pedX, p->pedY, p->pedZ, p->pedSpeed);
		}
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

			/* A slot the ENGINE cleared (PingOutCar -> ClearMem then controlType
			 * NONE) has no cosmetics and no model left. Asking it back as a player
			 * car made a LIVE CAR_DATA with ap.carCos == NULL: SetUpCivCollFlags
			 * reads cp->ap.carCos->colBox.vz (offset 0xD8) for every car in the
			 * world, so the next frame dereferenced NULL. The car is gone - give
			 * the mark up and leave the slot free for the traffic spawner. */
			if (car_data[i].ap.carCos == NULL ||
				car_data[i].ap.model < 0 ||
				car_data[i].ap.model >= MAX_CAR_RESIDENT_MODELS ||
				!JerCarSlotUsable(car_data[i].ap.model))
			{
				gMpOurCars[i] = 0;
				continue;
			}

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
				e.pedModel = (uint8_t)MpPedModelEffective(gMp.config.pedModel);
			}
			else
			{
				e.on = (uint8_t)(p->colorOn ? 1 : 0);
				e.r = (uint8_t)p->colorR;
				e.g = (uint8_t)p->colorG;
				e.b = (uint8_t)p->colorB;
				e.pedModel = (uint8_t)MpPedModelEffective(p->pedModel);
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
		e.pedModel = (uint8_t)MpPedModelEffective(gMp.config.pedModel);

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
	int i, n, from;

	if (len < (int)sizeof(MP_COLOR))
		return;

	memcpy(&h, p, sizeof(h));

	from = MpConnPlayerId(connIndex);

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

		/* A colour is a player's own, so the same binding as their input row:
		 * without it any peer could repaint somebody else on every screen. */
		if (!MpRowOwnerOk(from, (int)e.playerId))
			continue;

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
			pl->pedModel = MpPedModelEffective((int)e.pedModel);
		}
	}
}

/* THE RELOAD FAILSAFE.
 *
 * A car reload (a change, "Reload all cars", the verifier's repair) releases the
 * resident slot the local car is drawn from and rebuilds it. For the frames in
 * between, ap.model names a slot with no mesh: the chase camera has no car to sit
 * behind and spins, and the player falls through the map while the world keeps
 * stepping. The proper car comes back on its own; what is missing is a body to
 * stand in for it.
 *
 * So: if the car we are MEANT to drive is not loadable right now, put the host
 * city's car 1 (the first civilian its own level ships) under the player and keep
 * the session told the real choice. When the wanted slot becomes usable, put it
 * back. This is a stopgap, not a car: it is local-only, it logs both transitions,
 * and it never changes what the player's carstate says they drive. */
static int sMpWantSlot = -1;
static int sMpWantCity = -2;	/* -2 = not known yet, -1 = the host city */
static int sMpWantModel = -1;
static int sMpStandIn;
static int sMpStandInCar[MAX_CARS];	/* per car object: drawn as a grey stand-in */
static int sMpReadySlot[MAX_CARS];	/* readiness stand-in: wanted slot + 1, 0 = none */
static int sMpReadyHost[MAX_CARS];	/* the host slot the grey body sits on */
static u_char sMpReadyPalette[MAX_CARS];	/* palette to put back on the real car */
static int sMpCamReseat;
static void MpCarFailsafeTick(void);

static void MpFailsafeReset(void)
{
	sMpWantSlot = -1;
	sMpWantCity = -2;
	sMpWantModel = -1;
	sMpStandIn = 0;
	sMpCamReseat = 0;
	memset(sMpStandInCar, 0, sizeof(sMpStandInCar));
	memset(sMpReadySlot, 0, sizeof(sMpReadySlot));
	memset(sMpReadyHost, 0, sizeof(sMpReadyHost));
	memset(sMpReadyPalette, 0, sizeof(sMpReadyPalette));
}

static void MpFailsafeNoteCar(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	sMpWantSlot = slot;
	sMpWantModel = residentCarModels[slot];
	sMpWantCity = GetCarModelSourceCity(slot);
	sMpStandIn = 0;
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

	/* NO WORLD OF OURS YET. A joining client reaches `running` a few frames before
	 * its level is up, and StepSim (and so this send) can still be running on the
	 * frontend/attract world. car_data[0] there is either zeroed or the demo's car
	 * at the origin, and a body from it put a pose at 0,0 on the wire with a model
	 * that is not ours -- peers built the joiner at the origin, in the air, before
	 * its real pose arrived. Nothing about our car is meaningful until we are in
	 * the match. `gameStarted` is set by JER_EVENT_GAME_START (MpOnGameStart), when
	 * the level's cars really exist; the frontend check is the same refusal for the
	 * window before it. */
	if (gInFrontend || !gMp.gameStarted)
		return;

	/* The engine may have moved us into another car (or out of one) since the
	 * last frame -- adopt it BEFORE reading the pose, so this very frame already
	 * carries the change and the peers never see the old vehicle again. */
	MpFollowLocalCar();
	MpCarFailsafeTick();

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

	cp = NULL;

	if (me->carId >= 0 && me->carId < MAX_CARS)
	{
		cp = &car_data[me->carId];

		/* NOT A CAR YET. WELCOME sets our row's carId to engine slot 0 before the
		 * level exists, and the frontend/loading world has that CAR_DATA zeroed
		 * (controlType NONE, no model). A body sent from it carried a pose of
		 * 0,0 with a model that is not ours -- peers built the joiner at the world
		 * origin, in the air, before its real pose ever arrived. Keep the NO_CAR
		 * default until the engine has actually given us a car. */
		if (player[0].playerType != PLAYER_TYPE_CAR ||
			cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL ||
			cp->ap.model < 0 || cp->ap.model >= MAX_CAR_RESIDENT_MODELS ||
			!JerCarSlotUsable(cp->ap.model))
		{
			cp = NULL;
		}
	}

	if (cp != NULL)
	{
		/* Say it when this is the body we last got OUT of: the peers released it on
		 * the NO_CAR row and can re-seat us on it instead of building a second
		 * vehicle (MpHandleCarState). Kept on every frame while we drive that same
		 * body, so one lost packet cannot reopen the window. Entering a DIFFERENT
		 * car leaves leftCarSlot1 pointing elsewhere, so the bit stays clear and
		 * the peers keep the parked body and build the new one, as before. */
		if (me->leftCarSlot1 > 0 && me->carId == me->leftCarSlot1 - 1)
			e.flags |= MP_CARSTATE_SAME_BODY;

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

			/* THE FAILSAFE'S BODY IS NOT THE CHOICE. While the wanted car is being
			 * rebuilt, the local car wears the host city's car 1 so the camera has
			 * something to follow; the session must still be told what the player
			 * chose, or peers would see them switch to that stand-in and back. */
			if (sMpStandIn && sMpWantModel >= 0)
			{
				e.model = (uint8_t)sMpWantModel;
				e.modelCity = (uint8_t)((sMpWantCity >= 0) ? sMpWantCity : MP_CAR_CITY_SESSION);
			}
			else if (slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS && residentCarModels[slot] >= 0)
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

		/* ON DUTY: the siren and lightbar state, decided HERE because this is the
		 * only machine that can decide it -- the car is ours, driven by our AI, and
		 * `controlType == PURSUER_AI` is exactly what the engine checks before it
		 * lights a cop car. The receiver builds a posed mirror, not an AI pursuer,
		 * so its own test answers "no" for every peer car whatever the cop is doing;
		 * it has to be told. The condition is the engine's own (cars.c). */
		if (cp->controlType == CONTROL_TYPE_PURSUER_AI &&
		    cp->ai.p.dying < 75 && CarHasSiren(cp->ap.model) != 0)
			e.flags |= MP_TRAFFIC_SIREN;

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

	/* The body stops being ours: its sample timeline must go with it. */
	MpPlayClear(p->id);

	cp = &car_data[p->carId];

	if (cp->controlType == CONTROL_TYPE_PLAYER)
	{
		/* Leave OUR copy as the stopped, empty civ car the owner's own engine
		 * made it (the local get-out path does the same). Left as
		 * CONTROL_TYPE_PLAYER it could never be entered by anyone else here
		 * (TannerCanEnterCar takes CIV_AI only), and it kept a player slot
		 * the late-join seat scan skips. */
		cp->controlType = CONTROL_TYPE_CIV_AI;
		cp->ai.c.thrustState = CIV_AI_THRUST_STOP;
		cp->ai.c.ctrlState = CIV_AI_CTRL_EMPTY;
		cp->wheel_angle = 0;
		cp->ai.padid = &gMpQuietPad;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d got out; car slot %d left standing where it was (enterable)\n",
				p->id, p->carId);
	}
}

/* Some OTHER player (or us) is now in this slot: it belongs to them. */
static int MpCarSlotTakenByOther(const MP_PLAYER* leaver, int slot)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		const MP_PLAYER* o = &gMp.players[i];

		if (o != leaver && o->active && o->carId == slot)
			return 1;
	}

	return 0;
}

/* A player is leaving (LEAVE, timeout, or dropped from the roster): take their
 * stand-in Tanner away, and the car they abandoned on foot, unless another
 * player has commandeered it since -- then it is that player's car and stays.
 * Their CURRENT car is handled by MpRemovePlayer (same ownership check). */
void MpCleanupLeaverWorld(MP_PLAYER* p)
{
	int slot;

	if (p == NULL || p->isLocal)
		return;

	if (p->ped != NULL)
	{
		jer_npc_despawn((JerNpc*)p->ped);
		p->ped = NULL;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] ped: player %d left; stand-in removed\n", p->id);
	}

	slot = p->leftCarSlot1 - 1;

	if (slot >= 0 && slot < MAX_CARS && slot != p->carId &&
		car_data[slot].controlType != CONTROL_TYPE_NONE &&
		car_data[slot].ap.model == p->leftCarModel &&
		!MpCarSlotTakenByOther(p, slot))
	{
		MpReleaseOurCarSlot(slot);
		car_data[slot].controlType = CONTROL_TYPE_NONE;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] removed player %d's abandoned car (slot %d)\n", p->id, slot);
	}

	p->leftCarSlot1 = 0;
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

/* The MODE a server plays, named from its GAMEMODE id (MP_GAMEMODE_*, carried by
 * MP_BEACON / MP_WELCOME / the master listing). Session rules (cops on/off, etc.)
 * are settings OF a mode and never rename it: Take a Ride with cops off is still
 * Take a Ride. An id this build does not know reads "-". Keep it in step with the
 * tools' mode_name() (mp_test.py). */
const char* MpModeName(int gamemode)
{
	switch (gamemode)
	{
	case MP_GAMEMODE_TAKEADRIDE:
		return "TAKE A RIDE";
	default:
		return "-";
	}
}

/* The city name for a ROW of somebody else's server: the engine's own name, or
 * "-" for a city the engine has no row for.
 *
 * Deliberately NOT MpCarCityName: that answers "the session city" for a missing
 * row, which is right for a local picker (where the player's own city is the
 * alternative) and meaningless in a list of servers that are not ours. */
const char* MpCityLabel(int city)
{
	if (city < 0 || city >= CITY_COUNT || LevelNames[city] == NULL)
		return "-";

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

/* The host city's own car 1 (or, failing that, its first civilian), in a resident
 * slot that is really loaded. This is the body the reload failsafe puts under the
 * player while the chosen car is being rebuilt. */
static int MpHostCarOneSlot(void)
{
	int i;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		if (residentCarModels[i] == 1 && GetCarModelSourceCity(i) < 0 && JerCarSlotUsable(i))
			return i;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		if (residentCarModels[i] >= 0 && GetCarModelSourceCity(i) < 0 && JerCarSlotUsable(i))
			return i;

	return -1;
}

/* Put the host city's car 1 under a car object that has no mesh, and mark it so
 * the draw-colour hook paints the body solid grey. `fresh` says the flag was not
 * already set, i.e. this is the transition to log. */
static int MpCarStandIn(int carSlot)
{
	extern u_char defaultPlayerPalette;
	CAR_DATA* cp;
	int hostSlot;
	int fresh;

	if (carSlot < 0 || carSlot >= MAX_CARS)
		return 0;

	hostSlot = MpHostCarOneSlot();

	if (hostSlot < 0)
		return 0;

	cp = &car_data[carSlot];

	fresh = !sMpStandInCar[carSlot];
	sMpStandInCar[carSlot] = 1;

	if (cp->ap.model != hostSlot || !JerCarSlotUsable(cp->ap.model))
	{
		cp->ap.model = hostSlot;
		cp->ap.carCos = &car_cosmetics[hostSlot];
		cp->lowDetail = -1;
		cp->ap.palette = (u_char)CarSlotClampPalette(hostSlot, (int)defaultPlayerPalette);
		CreateDentableCar(cp);
	}

	/* The chase camera must follow a body that exists. If this is OUR car, re-seat
	 * it on the stand-in; a camera left on the released slot is the spin. */
	if (carSlot == player[0].playerCarId)
	{
		player[0].cameraCarId = cp->id;
		player[0].cameraAngle = cp->hd.direction + 1536;
	}

	return fresh;
}

static void MpCarClearStandIn(int carSlot)
{
	if (carSlot >= 0 && carSlot < MAX_CARS)
		sMpStandInCar[carSlot] = 0;
}

int MpCarIsStandIn(int carSlot)
{
	return (carSlot >= 0 && carSlot < MAX_CARS) ? sMpStandInCar[carSlot] : 0;
}

/* JERICHO: the readiness stand-in, for SINGLE-PLAYER (and for any frame in which the
 * session's own reload failsafe is not driving).
 *
 * The engine's slot decision (JerCarSlotState) now refuses an imported slot until every set
 * its model names has a live pin and a mapped palette row. That is deliberately stricter
 * than "a mesh exists", and a car already on the road can briefly be in that state while a
 * switch/reload lands. In a session, MpCarFailsafeTick already covers this and reports the
 * real choice over the wire; OUTSIDE a session nothing did, so the draw gate simply skipped
 * the car and the player saw nothing. Use the SAME proven stand-in here: the host city's
 * car 1, repointed and tinted solid grey by the draw-colour hook, until the real slot is
 * drawable - then repoint back and rebuild its dump.
 *
 * The engine draw path itself never mutates cp->ap.model: only this module does, exactly as
 * the session failsafe already does, so carhacks/mp identity keeps reading the real choice
 * from the remembered slot rather than from the grey body. */
void MpCarReadinessTick(void)
{
	extern u_char defaultPlayerPalette;
	CAR_DATA* cp;
	int carId, stored, want, host, city;

	if (gMp.running || gInFrontend)
		return;			/* the session failsafe owns MP; no car in the frontend */

	if (player[0].playerType != PLAYER_TYPE_CAR)
		return;			/* on foot: do not steal the parked car or the camera */

	carId = player[0].playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	cp = &car_data[carId];
	stored = sMpReadySlot[carId];

	if (stored > 0)
	{
		want = stored - 1;
		host = sMpReadyHost[carId];

		if (cp->ap.model != host)
		{
			/* The car object was repointed since we put the stand-in on: this is no
			 * longer our body, so forget the memory rather than fight it. */
			sMpReadySlot[carId] = 0;
			MpCarClearStandIn(carId);
			return;
		}

		if (!JerCarSlotDrawable(want))
			return;		/* still not drawable: keep the grey body */

		sMpReadySlot[carId] = 0;
		MpCarClearStandIn(carId);

		cp->ap.model = want;
		cp->ap.carCos = &car_cosmetics[want];
		cp->lowDetail = -1;
		cp->ap.palette = (u_char)CarSlotClampPalette(want, (int)sMpReadyPalette[carId]);
		CreateDentableCar(cp);

		printInfo("[mp] car readiness: slot %d (%s model %d) is drawable - leaving the host city's car 1 stand-in\n",
			want, MpCarCityName(GetCarModelSourceCity(want)), residentCarModels[want]);

		return;
	}

	want = cp->ap.model;

	if (want < 0 || want >= MAX_CAR_RESIDENT_MODELS)
		return;

	city = GetCarModelSourceCity(want);

	if (city < 0)
		return;			/* the level's own car: no import sets to wait for */

	if (JerCarSlotDrawable(want))
		return;

	sMpReadyPalette[carId] = (u_char)cp->ap.palette;

	if (MpCarStandIn(carId))
	{
		sMpReadySlot[carId] = want + 1;
		sMpReadyHost[carId] = cp->ap.model;	/* MpCarStandIn repointed it */

		printInfo("[mp] car readiness: slot %d (%s model %d) is not drawable yet - standing in with the host city's car 1, solid grey\n",
			want, MpCarCityName(city), residentCarModels[want]);
	}
}

static void MpCarFailsafeTick(void)
{
	extern u_char defaultPlayerPalette;
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;
	int want, hostSlot;

	if (!gMp.running || me == NULL || !me->isLocal || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	cp = &car_data[me->carId];

	/* THE WOUND CHECK: never let the chase camera target a car that has no body. If
	 * the player's car or the camera's car is cleared/released, re-seat the camera on
	 * our car this frame - a camera left on a released slot is the spin. */
	if (player[0].playerType == PLAYER_TYPE_CAR &&
		(player[0].cameraCarId < 0 || player[0].cameraCarId >= MAX_CARS ||
		 car_data[player[0].cameraCarId].controlType == CONTROL_TYPE_NONE ||
		 !JerCarSlotUsable(car_data[player[0].cameraCarId].ap.model)))
	{
		player[0].cameraCarId = me->carId;
		player[0].cameraAngle = cp->hd.direction + 1536;

		if (!sMpCamReseat && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] camera: re-seated on our car (slot %d) - its target had no vehicle\n",
				me->carId);

		sMpCamReseat = 1;
	}
	else
		sMpCamReseat = 0;

	/* Learn the choice from a healthy car once (a spawn or a car we got into);
	 * a change sets it explicitly in MpAdoptCar, even when that load fails. */
	if (!sMpStandIn && sMpWantModel < 0 &&
		cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS &&
		JerCarSlotUsable(cp->ap.model))
	{
		sMpWantSlot = cp->ap.model;
		sMpWantModel = residentCarModels[sMpWantSlot];
		sMpWantCity = GetCarModelSourceCity(sMpWantSlot);
	}

	if (sMpWantModel < 0)
		return;

	want = MpResidentSlotForCar(sMpWantCity, sMpWantModel);

	if (want >= 0 && want < MAX_CAR_RESIDENT_MODELS && JerCarSlotUsable(want))
	{
		if (sMpStandIn)
		{
			sMpStandIn = 0;
			sMpWantSlot = want;
			MpCarClearStandIn(me->carId);

			cp->ap.model = want;
			cp->ap.carCos = &car_cosmetics[want];
			cp->lowDetail = -1;
			cp->ap.palette = (u_char)CarSlotClampPalette(want, (int)defaultPlayerPalette);
			CreateDentableCar(cp);

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] car reload: %s model %d is loaded - leaving the host city's car 1 stand-in\n",
					MpCarCityName(sMpWantCity), sMpWantModel);
		}
		else
			sMpWantSlot = want;

		return;
	}

	hostSlot = MpHostCarOneSlot();

	if (hostSlot < 0)
		return;

	if (!sMpStandIn)
	{
		sMpStandIn = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] car reload: %s model %d is not loaded yet (slot %d) - standing in with the host city's car 1, solid grey (resident slot %d) until it does\n",
				MpCarCityName(sMpWantCity), sMpWantModel, want, hostSlot);
	}

	MpCarStandIn(me->carId);
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
/* The model each car's dentable verts were last built from. A slot reused IN PLACE
 * (carhacks' retiring-slot handover) keeps the same slot NUMBER while its MODEL changes,
 * so `was == slot` alone must not skip the rebuild: that would draw the new model's
 * polygons over the old model's vertices (the garbled-car bug). -1 = never rebuilt. */
static int gMpAdoptModel[MAX_CARS];
static int gMpAdoptModelInit;

static int MpAdoptCar(int carSlot, int city, int model, MP_PLAYER* who)
{
	CAR_DATA* cp;
	int slot;

	if (carSlot < 0 || carSlot >= MAX_CARS)
		return 0;

	if (who != NULL && who->isLocal)
	{
		/* Remember the choice even when this load cannot happen yet: the reload
		 * failsafe needs to know which car to put back afterwards. */
		sMpWantCity = city;
		sMpWantModel = model;
		sMpWantSlot = MpResidentSlotForCar(city, model);
	}

	if (!gMpAdoptModelInit)
	{
		int i;

		for (i = 0; i < MAX_CARS; i++)
			gMpAdoptModel[i] = -1;

		gMpAdoptModelInit = 1;
	}

	if (who != NULL && !who->isLocal && MpRemoteTargetsLocalCar(carSlot, "car adopt/rebuild", who->id))
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

		MpCarClearStandIn(carSlot);

		if (was == slot && gMpAdoptModel[carSlot] == model)
		{
			/* Already that car, and its verts match. OUR car may still want a new
			 * palette ("Respawn as this car" after only the palette changed): repaint
			 * it; the carstate's ap.palette carries it to the peers. */
			extern u_char defaultPlayerPalette;
			int want;

			if (who != NULL && !who->isLocal)
				return 0;

			want = CarSlotClampPalette(slot, (int)defaultPlayerPalette);

			if (want == (int)cp->ap.palette)
				return 0;

			cp->ap.palette = (u_char)want;

			if (who != NULL)
				who->palette = want;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] same car on slot %d: repainted in palette %d\n", slot, want);

			return 1;
		}

		cp->ap.model = slot;

		/* only a palette the NEW model has: ours is the player's choice
		 * (defaultPlayerPalette), a peer's the one they last named; out of range
		 * -> the model's own colours. The carstate then sends what is drawn. */
		{
			extern u_char defaultPlayerPalette;
			int want = (who == NULL || who->isLocal) ? (int)defaultPlayerPalette
								 : who->palette;

			cp->ap.palette = (u_char)CarSlotClampPalette(slot, want);
		}

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
		gMpAdoptModel[carSlot] = model;

		/* OUR car: rebind its engine and SIREN sounds to the new model, exactly as
		 * getting in does (players.c ChangePedPlayerToCar -> StartPlayerCarSounds).
		 * The siren is a looping channel (player*3+2) that is only STARTED there, and
		 * the per-frame control just unmutes it while the horn is held -- so changing
		 * from a car with no siren into a police car left that channel never started:
		 * light bar on, siren silent until you got out and back in. Going the other
		 * way, stop the old siren loop so it cannot hang on. */
		if (carSlot == player[0].playerCarId && player[0].playerType == PLAYER_TYPE_CAR)
		{
			if (CarHasSiren(slot) == 0)
				StopChannel(2);

			StartPlayerCarSounds(0, slot, (VECTOR*)cp->hd.where.t);
		}

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

	/* Cannot hold it: put the host city's car 1 under it, painted solid grey, until
	 * the owner's model is resident. The old body was the previous model, which on a
	 * release/reload is exactly the slot that has just lost its mesh - the spinning
	 * camera. Say so ONCE per change, not every frame -- a carstate arrives every
	 * frame, so the repetition is the noise this avoids. */
	if (MpCarStandIn(carSlot) && gMpCtx != NULL && (who == NULL || !who->isLocal))
		gMpCtx->jer_log(gMpCtx,
			"[mp] car load: %s model %d is not loaded yet - using the host city's car 1, solid grey, on slot %d until it does\n",
			MpCarCityName(city), model, carSlot);

	if (who != NULL && gMpCtx != NULL && who->id >= 0 && who->id < MP_MAX_PLAYERS &&
		(!sMpCarKeepSet[who->id] || sMpCarKeepCity[who->id] != city || sMpCarKeepModel[who->id] != model))
	{
		sMpCarKeepCity[who->id] = city;
		sMpCarKeepModel[who->id] = model;
		sMpCarKeepSet[who->id] = 1;

		if (slot >= 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d drives %s model %d (slot %d here) but that mesh is not loaded; the host city's car 1 stands in on slot %d\n",
				who->id, MpCarCityName(city), model, slot, carSlot);
		else
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d drives %s model %d, which this machine does not hold (the hotload will load it); the host city's car 1 stands in on slot %d\n",
				who->id, MpCarCityName(city), model, carSlot);
	}
	else if (who == NULL && gMpCtx != NULL)
	{
		/* Single player: a menu action, not a per-frame carstate, so there is no
		 * repetition to suppress -- say it every time. */
		if (slot >= 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): %s model %d resolves to slot %d here but that mesh is not loaded; the host city's car 1 stands in on slot %d\n",
				MpCarCityName(city), model, slot, carSlot);
		else
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): %s model %d is not held by this machine; the host city's car 1 stands in on slot %d\n",
				MpCarCityName(city), model, carSlot);
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

/* ------------------------------------------------------------------ */
/* A CHANGE CAR THAT HAD TO WAIT                                        */
/*                                                                      */
/* MP_CARQ_LOAD can legitimately answer "not yet". The pool is six      */
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
#define MP_CHANGE_WAIT_FRAMES	90	/* ~3.5 s at 25 fps */
#define MP_CHANGE_RETRY_EVERY	5	/* retry every N frames, not every frame */

static struct
{
	int active;
	int city;
	int model;
	int framesLeft;
} gMpCarChangeWait;

static int MpChangeCarRun(int city, int model, int isRetry);

int MpChangeCar(int city, int model)
{
	return MpChangeCarRun(city, model, 0);
}

/* Forget a wait that is no longer relevant (level reset, leaving the session). */
void MpCarChangeWaitForget(void)
{
	gMpCarChangeWait.active = 0;
}

/* One retry a frame, from the frame hook (the world is up there, which is what makes a
 * level's resident slots mean anything). The wait owns framesLeft, so MpChangeCarRun
 * only ever CLEARS the wait -- it never re-arms one on a retry. */
void MpCarChangeWaitTick(void)
{
	if (!gMpCarChangeWait.active)
		return;

	if (--gMpCarChangeWait.framesLeft <= 0)
	{
		int city = gMpCarChangeWait.city;
		int model = gMpCarChangeWait.model;

		gMpCarChangeWait.active = 0;

		/* The wait ran out. The refusal is a LOG line now: the on-screen notice that
		 * used to say it here went with the rest of mp's bottom-left debug block, and
		 * the REASON is the part worth keeping. */
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] change car refused: %s model %d - %s\n",
				MpCarCityName(city), model, MpChangeCarRefusal(city, model));

		return;
	}

	/* One retry a FRAME is too eager: the pool frees in frames, not microseconds, and
	 * each attempt costs a log line and a walk of the set. Measured: an unthrottled
	 * retry turned a run's handful of "no room" lines into 500+ of them. */
	if ((gMpCarChangeWait.framesLeft % MP_CHANGE_RETRY_EVERY) != 0)
		return;

	MpChangeCarRun(gMpCarChangeWait.city, gMpCarChangeWait.model, 1);
}

static int MpChangeCarRun(int city, int model, int isRetry)
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

			return 0;
		}

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): we asked for %s model %d (slot %d)\n",
				MpCarCityName(city), model, slot);

		/* NOT A CAR THIS MACHINE HOLDS YET? Ask for it, exactly as the session path
		 * does below. The picker offers a city's whole roster, so choosing one IS the
		 * request that imports it -- without this a cross-city or Driver 1 pick could
		 * be selected and then always refused, and the roster would be a lie.
		 *
		 * The ANSWER is used now: "not yet" means wait (see the block above the
		 * function), never adopt. */
		if (MpResidentSlotForCar(city, model) < 0 && !MpCarQueryLoad(city, model))
		{
			if (!isRetry)
			{
				gMpCarChangeWait.active = 1;
				gMpCarChangeWait.city = city;
				gMpCarChangeWait.model = model;
				gMpCarChangeWait.framesLeft = MP_CHANGE_WAIT_FRAMES;
			}

			return 0;
		}

		gMpCarChangeWait.active = 0;	/* the load landed (or was never needed) */

		changed = MpAdoptCar(slot, city, model, NULL);

		/* A successful change is already in the log (the "we asked for" line above,
		 * and MpAdoptCar's own). A refusal's REASON was the useful part of the notice
		 * that used to be on screen, so it goes to the log instead. */
		if (!changed && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] change car refused: %s model %d - %s\n",
				MpCarCityName(city), model, MpChangeCarRefusal(city, model));

		return changed;
	}

	if (me == NULL)
		return 0;

	if (me->carId < 0)
	{
		int seat = -1;
		int s;

		/* PREFER THE CAR WE ACTUALLY GOT OUT OF. It is the body this machine last
		 * parked, and the one the peers still hold for us (their leftCarSlot1), so
		 * getting back into it makes the carstate carry MP_CARSTATE_SAME_BODY and
		 * they re-seat us on it. Seating ourselves in some OTHER parked car would
		 * read as a genuinely different vehicle and they would build a second one,
		 * leaving this one standing. Same usability tests as the scan below. */
		if (me->leftCarSlot1 > 0)
		{
			int left = me->leftCarSlot1 - 1;

			if (left >= 0 && left < MAX_CARS && gMpOurCars[left] &&
				MpGetPlayerByCar(left) == NULL &&
				car_data[left].controlType == CONTROL_TYPE_CIV_AI &&
				car_data[left].ap.carCos != NULL &&
				JerCarSlotUsable(car_data[left].ap.model))
				seat = left;
		}

		/* Otherwise: get back into any car OF OURS -- one the module made, left
		 * parked and enterable (see MpFollowLocalCar). Never a live traffic car:
		 * taking one over and vacating it hands the traffic AI a car it cannot
		 * steer, which is the crash that note exists to prevent. */
		for (s = 0; seat < 0 && s < MAX_CARS; s++)
		{
			if (!gMpOurCars[s] || MpGetPlayerByCar(s) != NULL)
				continue;

			/* The destination has to be a car we can actually put the player in:
			 * a live CIV_AI body, with its own cosmetics and a resident model whose
			 * mesh is built. A slot the engine cleared (PingOutCar) is not a car any
			 * more, and entering it would move the local identity onto a CAR_DATA
			 * whose ap.carCos is NULL - the SetUpCivCollFlags 0xD8 crash. */
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
			if (gMpCtx != NULL)
			{
				int left = me->leftCarSlot1 - 1;

				/* Say WHY, not just that it failed: this is the one place a player
				 * finds out their parked car is not usable any more, and the four
				 * reasons are indistinguishable from the outside. */
				if (left >= 0 && left < MAX_CARS)
					gMpCtx->jer_log(gMpCtx,
						"[mp] change car: on foot and no car of ours to get back into "
						"(left slot %d: ours %d, taken %d, type %d, cosmetics %s, "
						"model %d, usable %d)\n",
						left, (int)gMpOurCars[left], MpGetPlayerByCar(left) != NULL,
						(int)car_data[left].controlType,
						(car_data[left].ap.carCos != NULL) ? "present" : "NULL",
						(int)car_data[left].ap.model,
						JerCarSlotUsable(car_data[left].ap.model));
				else
					gMpCtx->jer_log(gMpCtx,
						"[mp] change car: on foot and no car of ours to get back into\n");
			}

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

	/* NOT logged on a retry: a retry is not a new ask, and the carstress rig counts
	 * changes from this exact line (measured: an unthrottled retry inflated its
	 * "changes" figure 7x and made the coverage number meaningless). */
	if (gMpCtx != NULL && !isRetry)
		gMpCtx->jer_log(gMpCtx, "[mp] change car: we asked for %s model %d (slot %d)\n",
			MpCarCityName(city), model, me->carId);

	/* Not a car this machine holds? Ask whoever can to make it available (carhacks'
	 * import + hotload), which is also what tells the other machines to fold it in.
	 * Only THEN try the swap: pointing the car at a mesh that is not loaded is a
	 * crash, not a cosmetic glitch (see MpAdoptRemoteCar).
	 *
	 * So the answer is used: "not yet" (a spare slot busy for a frame or two) is a
	 * wait, not a refusal, and it must NOT fall through to the swap. */
	if (MpResidentSlotForCar(city, model) < 0 && !MpCarQueryLoad(city, model))
	{
		if (!isRetry)
		{
			gMpCarChangeWait.active = 1;
			gMpCarChangeWait.city = city;
			gMpCarChangeWait.model = model;
			gMpCarChangeWait.framesLeft = MP_CHANGE_WAIT_FRAMES;
		}

		return 0;
	}

	gMpCarChangeWait.active = 0;	/* the load landed (or was never needed) */

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

	/* The change itself is in the log (MpAdoptRemoteCar's "rebuilt player N's car"
	 * line and MpCarQueryChosen's "car chosen"). Only a REFUSAL carried text worth
	 * keeping, so its reason goes to the log rather than the screen. */
	if (!changed && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] change car refused: %s model %d - %s\n",
			MpCarCityName(city), model, MpChangeCarRefusal(city, model));

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

/* Place a car at (x,z) on the GROUND there, the way the engine's own InitCar does
 * (civ_ai.c: MapHeight at the target minus the suspension offset). A bare
 * InitCarPhysics with y=0 puts the car at world zero, which on a road that sits
 * at y=2048 is 2048 units BELOW it - the car then falls through the map. That is
 * the "respawn, then keep falling" loop: every placement repeated the drop.
 *
 * 0 is also MapHeight's answer for ground that is not resident, so the caller has
 * to have made the spot real first (MpWarpStreamTo) - the same rule the warp and
 * the join spawn already follow. Returns 0 when there is no ground to stand on. */
static int MpPlaceOnGround(CAR_DATA* cp, int x, int z, int dir)
{
	LONGVECTOR4 start;
	VECTOR probe;
	int gy;

	if (cp == NULL || cp->ap.carCos == NULL)
		return 0;

	probe.vx = x;
	probe.vy = 0;
	probe.vz = z;
	probe.pad = 0;

	gy = MapHeight(&probe);

	if (gy <= -1000)
		return 0;		/* the void: nothing to stand on */

	start[0] = x;
	start[1] = gy - cp->ap.carCos->wheelDisp[0].vy;
	start[2] = z;
	start[3] = 0;

	InitCarPhysics(cp, &start, dir);
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
	VECTOR probe;
	int ground;
	int x, z, dir;

	if (!gMp.running)
		return 0;

	me = MpLocalPlayer();

	if (me == NULL)
		return 0;

	/* ON FOOT: busted by a cop (the "You've been caught" route), drowned or hit by
	 * a train. The engine's game over there tore the session down -- the menu
	 * opened over a world the camera no longer had a target in, and the player
	 * had to rejoin. Refuse it the same way: not dying, not wanted, and off the
	 * water if that is what was killing us. */
	if (me->carId < 0 && player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
	{
		PEDESTRIAN* ped = player[0].pPed;

		tannerDeathTimer = 0;
		player[0].dying = 0;
		pedestrianFelony = 0;

		if (ped != NULL)
		{
			int surf = GetSurfaceIndex((VECTOR*)&ped->position);

			if ((surf == -26 || surf == -23) && PlayerStartInfo[0] != NULL)
			{
				ped->position.vx = PlayerStartInfo[0]->position.vx;
				ped->position.vz = PlayerStartInfo[0]->position.vz;
				ped->position.vy = -62 - MapHeight((VECTOR*)&ped->position);	/* standing height, as pedest.c places him */
			}
		}

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] death on foot in a session - game over refused\n");

		return 1;
	}

	if (me->carId < 0 || me->carId >= MAX_CARS)
		return 0;

	cp = &car_data[me->carId];

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] respawn reason: engine game over (car) - slot %d at %d,%d,%d damage %d/%d dying %d upsideDown %d felony %d; last rx %s conn %d frame %lu (now %lu)\n",
			me->carId, cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2],
			cp->totalDamage, MaxPlayerDamage[0], (int)player[0].dying, (int)player[0].upsideDown,
			(int)cp->felonyRating, gMpLastRxTag, gMpLastRxConn, gMpLastRxFrame, gMp.frame);

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

	/* Face ALONG THE ROAD: a wreck usually ends at an angle (or upside down
	 * across the lanes), and respawning it that way left the driver to sort it out. */
	{
		int fx, fz, fdir = -1;

		if (MpFindRespawnSpot(x, z, &fx, &fz, &fdir))
		{
			x = fx;
			z = fz;
			if (fdir >= 0)
				dir = fdir;
		}
	}
	dir = MpRoadHeadingNear(x, z, dir & 0xfff);

	/* THE PLACEMENT RESOLVES THE GROUND. y=0 is not "the engine will sort it out":
	 * a road at y=2048 left the car two thousand units below it, falling. If the
	 * chosen spot is somehow void, fall back to the level start rather than drop
	 * the player. */
	if (!MpPlaceOnGround(cp, x, z, dir))
	{
		if (PlayerStartInfo[0] == NULL)
			return 0;

		x = PlayerStartInfo[0]->position.vx;
		z = PlayerStartInfo[0]->position.vz;
		dir = PlayerStartInfo[0]->rotation;

		if (!MpPlaceOnGround(cp, x, z, dir))
			return 0;
	}
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
#define MP_WARP_NEAR	300
#define MP_WARP_FAR	2000
#define MP_WARP_CLEAR	300

/* Can this world point be LANDED on, and if not, can it be made so?
 *
 * Three questions in this order, and the order is the whole point:
 *
 *   1. does the REGION hold data (jer_map_region_has_data)? 0 means a void
 *      forever - there is nothing to stream in - so never land there.
 *   2. is it RESIDENT? If not, `spool` asks jer_map_spool_to to make it so
 *      (synchronous on PC) and this answers "not yet".
 *   3. only now the ground and the scenery.
 *
 * Asking the GROUND first is how a car lands in the void: an unstreamed
 * destination has MapHeight answer 0, and 0 is also a perfectly good height, so
 * `MapHeight > -1000` says "fine" about ground that is not there. Worse, the
 * spool has to be for THE POINT BEING TESTED - a candidate a couple of thousand
 * units from the one that was streamed is very likely a different region. */
static int MpWarpSpotOk(int x, int z)
{
	VECTOR p;
	int region;

	if (!jer_map_ready())
		return 0;

	region = jer_map_region_of(x, z);

	if (!jer_map_region_has_data(region))
		return 0;	/* a void forever: there is nothing to stream in */

	if (!jer_map_region_resident(region))
		return 0;

	p.vx = x;
	p.vy = 0;
	p.vz = z;
	p.pad = 0;

	if (!CellEmpty(&p, MP_WARP_CLEAR))
		return 0;

	return MapHeight(&p) > -1000;
}

/* Make (x,z) landable if it can be, and say whether it now is.
 *
 * The streamer follows MainPlayer.spoolXZ, so it is AIMED FIRST and the stream is
 * then asked for -- the sequence the hop path and antfarm both use, and the
 * reason a bare jer_map_spool_to can quietly leave the window somewhere else.
 * The helper is skipped when the region is already resident, because in that case
 * it is not free (it still runs the load-area checks). The VECTOR must outlive
 * the call, hence the static.
 *
 * Refusing is a real answer: a spot whose region has no data can never be
 * streamed, and landing there is the void. */
static int MpWarpStreamTo(int x, int z)
{
	static VECTOR sWarpSpool;
	VECTOR* oldSpoolXZ;
	int region, ok;

	if (MpWarpSpotOk(x, z))
		return 1;

	if (!jer_map_ready())
		return 0;

	region = jer_map_region_of(x, z);

	if (!jer_map_region_has_data(region))
		return 0;

	/* HAND THE STREAMER BACK WHEN THE CALL ENDS. jer_map_spool_to reads
	 * MainPlayer.spoolXZ to decide where to stream, and the caller's pointer is the
	 * engine's own (the player car's hd.where.t, or the pedestrian's position).
	 * Leaving it on a module static -- which this used to do -- strands the
	 * streamer at the destination for the rest of the match: the car drives away
	 * and the world stops following it. Only the spool call itself needs the
	 * static, because ControlMap must read the destination while it runs. */
	oldSpoolXZ = MainPlayer.spoolXZ;

	sWarpSpool.vx = x;
	sWarpSpool.vy = 0;
	sWarpSpool.vz = z;
	sWarpSpool.pad = 0;

	MainPlayer.spoolXZ = &sWarpSpool;

	jer_map_spool_to(x, z);

	MainPlayer.spoolXZ = oldSpoolXZ;

	ok = MpWarpSpotOk(x, z);	/* synchronous on PC, so readable now */

	return ok;
}

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
#define MP_ROAD_RING_STEP	600	/* units between rings */
#define MP_ROAD_RINGS		12	/* 12 rings = 7200 units out */
#define MP_ROAD_PROBES		12	/* points probed around each ring */
#define MP_ROAD_AXIS_PROBE	500	/* how far along a road the heading test looks */

/* The heading a car should face to be pointing ALONG the road at (x,z).
 *
 * Two answers, best first:
 *   1. the SEGMENT's own angle (a straight carries one in the same 0..4095 space
 *      hd.direction uses; a curve has only its end tangents, so it gets `start`);
 *   2. failing that, ask the ROAD ITSELF which way it runs: a direction with road
 *      AHEAD and road BEHIND is the axis of the road through the point.
 *
 * (2) exists because (1) frequently cannot answer: measured on CHICAGO, RoadInCell
 * says "no road" at a spawn point where JerRoadAt says there is road, so every
 * joiner fell through. The first fallback was "the search probe came in radially,
 * so face tangentially" -- which is right only when the road runs past the point,
 * and 90 degrees wrong when it runs away from it (reported as "some spawn at a
 * right angle to the intended direction"). Probing the road is not a guess. */
static int MpRoadHeadingAt(int x, int y, int z, int* outDir)
{
	VECTOR p;
	DRIVER2_ROAD_INFO info;
	int surf, i;

	p.vx = x;
	p.vy = y;
	p.vz = z;
	p.pad = 0;

	/* THE PLANE IS PICKED AT THIS HEIGHT, so ask the ground rather than trusting a
	 * caller that only knows x/z (the join spawn passes 0). */
	p.vy = MapHeight(&p);

	surf = RoadInCell(&p);

	if (surf >= 0 && IS_ROAD_SURFACE(surf))
	{
		memset(&info, 0, sizeof(info));

		if (GetSurfaceRoadInfo(&info, surf))
		{
			if (IS_STRAIGHT_SURFACE(surf) && info.straight != NULL)
			{
				*outDir = info.straight->angle & 0xfff;
				return 1;
			}

			if (IS_CURVED_SURFACE(surf) && info.curve != NULL)
			{
				*outDir = info.curve->start & 0xfff;
				return 1;
			}
		}
	}

	/* Which way does the road run? Road on both sides = that is the axis. */
	{
		int probe = MP_ROAD_AXIS_PROBE;

		for (i = 0; i < 8; i++)
		{
			int ang = (i * 4096) / 8;
			int fx = (int)(((long)rsin(ang) * probe) >> 12);
			int fz = (int)(((long)rcos(ang) * probe) >> 12);

			if (JerRoadAt(x + fx, y, z + fz) && JerRoadAt(x - fx, y, z - fz))
			{
				*outDir = ang;
				return 1;
			}
		}
	}

	return 0;
}

/* A landable road point near (cx, cz). `pickRandom` chooses among the road points
 * on the FIRST ring that has any; otherwise the first usable one wins (the warp's
 * and respawn's "nearest"). Returns 1 and fills the outputs on success. The caller
 * still has to stream the winner - see MpWarpStreamTo. */
static int MpFindRoadSpotNear(int cx, int cy, int cz, int seed,
	int* outX, int* outZ, int* outDir, int* outRing)
{
	int ring, i;
	unsigned int rnd = 0;

	if (seed >= 0)
	{
		/* The seed is the JOINING PLAYER'S id, not the local one: seeded with the
		 * local id the host picked the same spot for every peer it placed (measured:
		 * players 1 and 2 both landed on 87358,-456175, i.e. one lay-by, which is the
		 * thing this exists to avoid). Per-player and deterministic, so every machine
		 * agrees on where a given player comes in and a replay lands the same way. */
		rnd = gMp.seed ^ (unsigned int)((seed + 1) * 2654435761u);
	}

	for (ring = 1; ring <= MP_ROAD_RINGS; ring++)
	{
		int r = ring * MP_ROAD_RING_STEP;
		int cand[MP_ROAD_PROBES];
		int n = 0;

		for (i = 0; i < MP_ROAD_PROBES; i++)
		{
			/* a full turn is 4096, so the probes are evenly spaced by construction */
			int ang = (i * 4096) / MP_ROAD_PROBES;
			int px = cx + (int)(((long)rsin(ang) * r) >> 12);
			int pz = cz + (int)(((long)rcos(ang) * r) >> 12);

			if (!JerRoadAt(px, cy, pz))
				continue;

			if (!MpWarpSpotOk(px, pz))
				continue;

			cand[n++] = i;

			if (seed < 0)
				break;		/* nearest: first usable on this ring */
		}

		if (n == 0)
			continue;

		i = cand[0];

		if (seed >= 0 && n > 1)
		{
			rnd = rnd * 1103515245u + 12345u;
			i = cand[(rnd >> 16) % (unsigned int)n];
		}

		{
			int ang = (i * 4096) / MP_ROAD_PROBES;

			*outX = cx + (int)(((long)rsin(ang) * r) >> 12);
			*outZ = cz + (int)(((long)rcos(ang) * r) >> 12);

			if (outDir != NULL && !MpRoadHeadingAt(*outX, cy, *outZ, outDir))
			{
				/* Neither the segment's angle nor the road's own axis: leave it to
				 * the caller's heading (the level start's) and say so in the log,
				 * rather than inventing a direction. The old "face tangentially to
				 * the probe" guess was 90 degrees wrong whenever the road ran away
				 * from the point instead of past it. */
				*outDir = -1;
			}
		}

		if (outRing != NULL)
			*outRing = ring;

		return 1;
	}

	return 0;
}

/* The road's heading at (x, z), turned to whichever of its two directions is
 * closer to `want` (a car already pointing down the road keeps its way). */
/* A RESPAWN goes somewhere else, not back onto the road you just crashed on: a road
 * point in a ring around the car, roughly 60-150 m out (12000-30000 world units; a car
 * is about 1000 units long), clear of every other player's car, facing along the road.
 * Already-resident regions are tried first so a respawn rarely has to stream; then
 * regions that need spooling. 0 when nothing qualified -- the callers then fall back
 * to the nearest road, as before. */
#define MP_RESPAWN_MIN		12000
#define MP_RESPAWN_MAX		30000
#define MP_RESPAWN_CLEAR	3000	/* no other player's car this close to the spot */
#define MP_RESPAWN_TRIES	12	/* per pass */

static int MpFindRespawnSpot(int cx, int cz, int* outX, int* outZ, int* outDir)
{
	unsigned int rnd;
	int pass, t, i;
	MP_PLAYER* me = MpLocalPlayer();

	if (!jer_map_ready())
		return 0;

	rnd = gMp.seed ^ (unsigned int)MpNowMs() ^ (unsigned int)((me != NULL ? me->id + 3 : 3) * 2654435761u);

	for (pass = 0; pass < 2; pass++)
	{
		for (t = 0; t < MP_RESPAWN_TRIES; t++)
		{
			int ang, r, px, pz, x, z, dir = -1, region, clear = 1;
			long long dx, dz;

			rnd = rnd * 1103515245u + 12345u;
			ang = (int)((rnd >> 8) & 0xfff);
			r = MP_RESPAWN_MIN + (int)((rnd >> 16) % (unsigned int)(MP_RESPAWN_MAX - MP_RESPAWN_MIN));
			px = cx + (int)(((long)rsin(ang) * r) >> 12);
			pz = cz + (int)(((long)rcos(ang) * r) >> 12);

			region = jer_map_region_of(px, pz);
			if (!jer_map_region_has_data(region))
				continue;
			if (!jer_map_region_resident(region))
			{
				if (pass == 0)
					continue;
				if (!jer_map_spool_to(px, pz))
					continue;
			}

			if (!MpFindRoadSpotNear(px, 0, pz, -1, &x, &z, &dir, NULL))
				continue;

			dx = (long long)x - cx;
			dz = (long long)z - cz;
			if (dx * dx + dz * dz < (long long)(MP_RESPAWN_MIN * 3 / 4) * (MP_RESPAWN_MIN * 3 / 4))
				continue;	/* the road search drifted back toward where we were */

			for (i = 0; i < MP_MAX_PLAYERS && clear; i++)
			{
				MP_PLAYER* o = MpGetPlayer(i);

				if (o != NULL && o->active && o != me && o->carId >= 0 && o->carId < MAX_CARS)
				{
					long long ox = (long long)car_data[o->carId].hd.where.t[0] - x;
					long long oz = (long long)car_data[o->carId].hd.where.t[2] - z;

					if (ox > -MP_RESPAWN_CLEAR && ox < MP_RESPAWN_CLEAR &&
						oz > -MP_RESPAWN_CLEAR && oz < MP_RESPAWN_CLEAR)
						clear = 0;
				}
			}
			if (!clear)
				continue;

			if (!MpWarpStreamTo(x, z))
				continue;

			/* A resident region can still answer 0 for a cell that is not in the
			 * barrel (the gather's measured note). For a DEATH respawn we can try
			 * another spot; landing on a void would just fall again. */
			{
				VECTOR gp;

				gp.vx = x;
				gp.vy = 0;
				gp.vz = z;
				gp.pad = 0;

				if (MapHeight(&gp) <= -1000)
					continue;
			}

			*outX = x;
			*outZ = z;
			*outDir = dir;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] respawn: road spot %d,%d in the %d-unit ring around %d,%d (pass %d, try %d)\n",
					x, z, r, cx, cz, pass, t);
			return 1;
		}
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] respawn: no clear road %d-%d units out from %d,%d - nearest road instead\n",
			MP_RESPAWN_MIN, MP_RESPAWN_MAX, cx, cz);
	return 0;
}

static int MpRoadHeadingNear(int x, int z, int want)
{
	int dir;

	if (!MpRoadHeadingAt(x, 0, z, &dir))
		return want;

	if (((dir - want + 4096 + 1024) & 0xfff) >= 2048)
		dir = (dir + 2048) & 0xfff;

	return dir;
}

/* Put the joiner's car on a chosen spot, snap the camera with it, and log what
 * kind of spot it was. Returns 0 when there is no ground under it. */
static int MpPlaceSelfAt(CAR_DATA* cp, int x, int z, int dir, const char* how, int attempt)
{
	VECTOR gp;
	int ground;

	/* THE GROUND HAS TO BE REAL AT THE MOMENT OF PLACEMENT. An unstreamed cell
	 * answers with the sea/default plane -- a perfectly plausible height, which is
	 * how a car ends up standing in the air. JerRoadAt is the road map's own
	 * answer: if it no longer sees road here, refuse and let the caller try
	 * another spot or take the fallback. */
	if (!JerRoadAt(x, 0, z))
		return 0;

	gp.vx = x;
	gp.vy = 0;
	gp.vz = z;
	gp.pad = 0;
	ground = MapHeight(&gp);

	if (!MpPlaceOnGround(cp, x, z, (dir >= 0) ? dir : PlayerStartInfo[0]->rotation))
		return 0;

	cp->hd.speed = 0;
	player[0].cameraAngle = cp->hd.direction + 1536;

	/* snap the camera with the car instead of letting it swing across */
	player[0].pos[0] = x;
	player[0].pos[2] = z;
	InitCamera(&player[0]);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] live join: placed our car at %d,%d,%d dir %d, ground %d (%s, try %d, frame %d)\n",
			x, cp->hd.where.t[1], z, dir, ground, how, attempt, gMpPlaceSelfTry);

	return 1;
}

/* Stream the REGION under (x,z), without requiring the point itself to be a
 * landable cell. A region centre is only a stream target -- the road search
 * inside the region supplies the landing spot -- so it must not be put through
 * MpWarpSpotOk's CellEmpty/height gate. Returns 1 when the region has data and
 * is now resident. */
static int MpStreamRegionAt(int x, int z)
{
	static VECTOR sSpool;	/* spoolXZ must outlive the call */
	VECTOR* oldSpoolXZ;
	int ok;

	if (!jer_map_ready() || !jer_map_region_has_data(jer_map_region_of(x, z)))
		return 0;

	oldSpoolXZ = MainPlayer.spoolXZ;

	sSpool.vx = x;
	sSpool.vy = 0;
	sSpool.vz = z;
	sSpool.pad = 0;

	MainPlayer.spoolXZ = &sSpool;

	ok = jer_map_spool_to(x, z);

	MainPlayer.spoolXZ = oldSpoolXZ;

	return ok;
}

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
#define MP_JOIN_WIDE_TRIES	8	/* centers probed inside the chosen region */

static int MpFindRoadSpotWide(int carId, int* outX, int* outZ, int* outDir)
{
	unsigned int rnd;
	int regionWorld, margin, span, t;

	if (!jer_map_ready() || regions_across <= 0 || regions_down <= 0 ||
		units_across_halved <= 0 || units_down_halved <= 0)
		return 0;

	regionWorld = MAP_REGION_SIZE * MAP_CELL_SIZE;

	if (regionWorld <= 0)
		return 0;

	rnd = gMp.seed ^ (unsigned int)((gMpWideRegion + 1) * 2654435761u) ^ (unsigned int)MpNowMs();

	/* Choose and stream ONE region, once for this join. */
	if (gMpWideRegion < 0)
	{
		int startRegion = jer_map_region_of(PlayerStartInfo[0]->position.vx,
			PlayerStartInfo[0]->position.vz);
		int attempt;

		for (attempt = 0; attempt < 32; attempt++)
		{
			int r, rx, rz, cx, cz;

			rnd = rnd * 1103515245u + 12345u;

			r = (int)((rnd >> 8) % (unsigned int)(regions_across * regions_down));

			if (r == startRegion || !jer_map_region_has_data(r))
				continue;

			rx = r % regions_across;
			rz = r / regions_across;

			cx = rx * regionWorld - units_across_halved + regionWorld / 2;
			cz = rz * regionWorld - units_down_halved + regionWorld / 2;

			/* The ONLY spool this placement makes. A region with nothing to load
			 * is re-picked rather than landed in. The point is a stream target,
			 * not a landing spot, so it skips MpWarpSpotOk's cell test. */
			if (!MpStreamRegionAt(cx, cz))
				continue;

			gMpWideRegion = r;
			gMpWideTries = 0;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] live join: map-wide region %d chosen (center %d,%d)\n",
					r, cx, cz);
			break;
		}

		if (gMpWideRegion < 0)
			return 0;
	}

	{
		int rx = gMpWideRegion % regions_across;
		int rz = gMpWideRegion / regions_across;
		int x0 = rx * regionWorld - units_across_halved;
		int z0 = rz * regionWorld - units_down_halved;

		/* Keep each center's 7.2k road-probe ring inside the resident region. A
		 * region too small for that is probed from its middle; MpWarpSpotOk then
		 * refuses any ring point that fell into a neighbour. */
		margin = 12000;

		if (regionWorld < 2 * margin + 1000)
			margin = 0;

		span = regionWorld - 2 * margin;

		if (span < 1)
			span = 1;

		for (t = gMpWideTries; t < MP_JOIN_WIDE_TRIES; t++)
		{
			int cx, cz, x, z, dir = -1;

			rnd = rnd * 1103515245u + 12345u;
			cx = x0 + margin + (int)((rnd >> 8) % (unsigned int)span);
			rnd = rnd * 1103515245u + 12345u;
			cz = z0 + margin + (int)((rnd >> 8) % (unsigned int)span);

			gMpWideTries++;

			if (!MpFindRoadSpotNear(cx, 0, cz, gMpWideRegion + t * 97, &x, &z, &dir, NULL))
				continue;

			if (!MpWarpSpotOk(x, z))
				continue;

			if (!MpJoinSpotClearOfPlayers(x, z, -1, carId))
				continue;

			if (!MpWarpStreamTo(x, z))	/* confirm the final spot's area */
				continue;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] live join: map-wide road %d,%d dir %d in region %d (clear by %d, center %d,%d)\n",
					x, z, dir, gMpWideRegion, MP_JOIN_CLEAR, cx, cz);

			*outX = x;
			*outZ = z;
			*outDir = dir;
			return 1;
		}
	}

	return 0;
}

/* The last resort before the level start: stream the start region once (the
 * probes may never have had its road data resident), then ask the near-start
 * road search again. */
static int MpPlaceSelfFallback(CAR_DATA* cp, int carId, int seed)
{
	int sx = PlayerStartInfo[0]->position.vx;
	int sz = PlayerStartInfo[0]->position.vz;
	int fx = 0, fz = 0, fdir = -1;

	if (MpWarpStreamTo(sx, sz) &&
		MpFindRoadSpotNear(sx, 0, sz, seed, &fx, &fz, &fdir, NULL) &&
		MpJoinSpotClearOfPlayers(fx, fz, -1, carId) &&
		MpPlaceSelfAt(cp, fx, fz, fdir, "near-start fallback", 0))
		return 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] live join: near-start fallback found no clear road - keeping the level start\n");

	return 0;
}

void MpPlaceSelfTick(void)
{
	MP_PLAYER* me;
	CAR_DATA* cp;
	unsigned int rnd;
	int k, i, carId;

	if (gMpPlaceSelfFrames <= 0)
		return;

	if (!gMp.running || gMp.role != MP_ROLE_CLIENT)
	{
		if (!gMp.running && gMp.role == MP_ROLE_NONE)
			gMpPlaceSelfFrames = 0;
		return;
	}

	me = MpLocalPlayer();

	if (me == NULL || PlayerStartInfo[0] == NULL)
		return;

	/* THE FIRST FRAME THE CAR EXISTS. Our roster row adopts the engine's car in
	 * MpFollowLocalCar, which runs later in this same tick (MpSendOwnCarState), so
	 * waiting for me->carId would delay placement by a frame -- and the frame it
	 * delays is the one whose state the peers receive. The engine's own slot is
	 * the car the level just built for us, so use it now. */
	carId = (me->carId >= 0 && me->carId < MAX_CARS) ? me->carId : (int)player[0].playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	/* Only a car that is STILL ON THE LEVEL START is a fresh joiner's to move. One that
	 * has been driven away belongs to a player already in the match: never relocate it
	 * (or snap its camera), whatever armed this. */
	{
		int sdx = car_data[carId].hd.where.t[0] - PlayerStartInfo[0]->position.vx;
		int sdz = car_data[carId].hd.where.t[2] - PlayerStartInfo[0]->position.vz;

		if (sdx < -3000 || sdx > 3000 || sdz < -3000 || sdz > 3000)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] live join: our car is already %d,%d from the level start - not placing it\n", sdx, sdz);
			gMpPlaceSelfFrames = 0;
			return;
		}
	}

	/* HOLD THE CAR STILL WHILE WE SORT THE SPAWN. The engine's initial pose can be
	 * over ground this machine has not streamed, and a car that falls or rolls
	 * while the road search runs is one the driver sees move before it teleports.
	 * Pin x/y/z and the heading, and zero the speed, on every frame spent looking;
	 * the placement below writes the final pose. */
	{
		CAR_DATA* hold = &car_data[carId];
		MATRIX m;

		if (gMpPlaceSelfTry == 0)
			gMpPlaceSelfHoldY = hold->hd.where.t[1];

		hold->hd.speed = 0;
		hold->hd.where.t[0] = PlayerStartInfo[0]->position.vx;
		hold->hd.where.t[1] = gMpPlaceSelfHoldY;
		hold->hd.where.t[2] = PlayerStartInfo[0]->position.vz;
		hold->hd.direction = PlayerStartInfo[0]->rotation;

		_RotMatrixY(&m, (short)hold->hd.direction);
		memcpy(hold->hd.where.m, m.m, sizeof(hold->hd.where.m));
	}

	/* gMpPlaceSelfFrames: >1 = settle countdown, then 1..-TRIES = try each frame */
	if (gMpPlaceSelfFrames > 1)
	{
		gMpPlaceSelfFrames--;
		return;
	}

	gMpPlaceSelfTry++;

	if (gMpPlaceSelfTry == 1 && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] place-self reason: live join welcome (armed once); last rx %s conn %d frame %lu\n",
			gMpLastRxTag, gMpLastRxConn, gMpLastRxFrame);

	cp = &car_data[carId];

	/* FULL-CITY TAKE A RIDE (gBootMpLevel == 0): a random road ANYWHERE on the
	 * map, not the near-start ring. One region is streamed for this join; if it
	 * has no road for us, the near-start fallback follows rather than thrashing
	 * the streamer through region after region. */
	if (!gBootMpLevel)
	{
		int x = 0, z = 0, dir = -1;

		if (MpFindRoadSpotWide(carId, &x, &z, &dir) &&
			MpPlaceSelfAt(cp, x, z, dir, "map-wide", gMpWideTries))
		{
			gMpPlaceSelfFrames = 0;
			return;
		}

		/* No region could even be streamed yet: the level is still settling, so
		 * retry on the next tick instead of falling back on the first frame. */
		if (gMpWideRegion < 0 && gMpPlaceSelfTry < 8)
			return;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] live join: no map-wide road (region %d, %d center(s) tried) - near-start fallback\n",
				gMpWideRegion, gMpWideTries);

		(void)MpPlaceSelfFallback(cp, carId, me->id + 8192);
		gMpPlaceSelfFrames = 0;
		return;
	}

	/* SMALL ARENA: the near-start ring, clear of the others, is the right answer. */
	rnd = gMp.seed ^ (unsigned int)((me->id + 7) * 2654435761u) ^ (unsigned int)MpNowMs();

	for (k = 0; k < 8; k++)
	{
		int ang, r, cx, cz, x, z, dir = -1, clear = 1;

		rnd = rnd * 1103515245u + 12345u;
		ang = (int)((rnd >> 8) & 0xfff);
		r = 1500 + (int)((rnd >> 20) % 3000u);
		cx = PlayerStartInfo[0]->position.vx + (int)(((long)rsin(ang) * r) >> 12);
		cz = PlayerStartInfo[0]->position.vz + (int)(((long)rcos(ang) * r) >> 12);

		if (!MpFindRoadSpotNear(cx, 0, cz, me->id + k * 64, &x, &z, &dir, NULL) || !MpWarpSpotOk(x, z))
			continue;

		for (i = 0; i < MP_MAX_PLAYERS && clear; i++)
		{
			MP_PLAYER* o = MpGetPlayer(i);

			if (o != NULL && o->active && o != me && o->carId >= 0 && o->carId < MAX_CARS)
			{
				int dx = car_data[o->carId].hd.where.t[0] - x;
				int dz = car_data[o->carId].hd.where.t[2] - z;

				if (dx > -MP_JOIN_CLEAR && dx < MP_JOIN_CLEAR &&
					dz > -MP_JOIN_CLEAR && dz < MP_JOIN_CLEAR)
					clear = 0;
			}
		}

		if (!clear)
			continue;

		if (!MpPlaceSelfAt(cp, x, z, dir, "near-start", k))
			continue;

		gMpPlaceSelfFrames = 0;
		return;
	}

	if (gMpPlaceSelfTry < MP_PLACE_SELF_TRIES)
		return;		/* road data may still be streaming: try again next frame */

	/* LAST RESORT, BEFORE THE LEVEL START: stream the start region once and ask
	 * the near-start search again. Leaving the car on the level start is the final
	 * answer, not the first. */
	(void)MpPlaceSelfFallback(cp, carId, me->id + 4096);

	gMpPlaceSelfFrames = 0;
}

/* Where to put the car, in the target's own axes: `along` is measured along
 * their FORWARD axis and every value is <= 0, which is the definition of "not in
 * their trajectory". Beside first (least in the way), then behind, then further
 * out. */
static const int kWarpAlong[]  = { -MP_WARP_NEAR, -MP_WARP_NEAR, -900, -900, -1600, -MP_WARP_FAR, 0, 0 };
static const int kWarpAcross[] = { 600, -600, 900, -900, 0, 0, 900, -900 };


/* How old an on-foot pose may be and still be a warp target: poses arrive every
 * few frames while someone is on foot, so a few seconds of silence means the copy
 * is stale (they got back in, or left). */
#define MP_WARP_PED_FRESH_MS	5000

/* Put OUR on-foot pedestrian down on the chosen spot.
 *
 * The engine owns the y on foot: AnimatePed rewrites position.vy from MapHeight
 * every frame (standing height -130 for Tanner), so it is set the same way here
 * rather than left at whatever the last frame's value was. player[0].pos is what
 * the camera and the streamer read, and AnimatePed would only refresh it on the
 * next sim frame -- too late for a camera that is still over the old place. */
static int MpPlaceOwnPedAt(LPPEDESTRIAN ped, int x, int z, int dir)
{
	VECTOR gp;
	int ground;

	if (ped == NULL)
		return 0;

	gp.vx = x;
	gp.vy = 0;
	gp.vz = z;
	gp.pad = 0;

	ground = MapHeight(&gp);

	ped->position.vx = x;
	ped->position.vz = z;
	ped->position.vy = -130 - ground;	/* pedest.c's Tanner standing height */
	ped->dir.vy = (short)(dir & 0xFFF);
	ped->speed = 0;

	player[0].pos[0] = ped->position.vx;
	player[0].pos[1] = -ped->position.vy;
	player[0].pos[2] = ped->position.vz;
	player[0].cameraAngle = (dir & 0xFFF) + 1536;

	InitCamera(&player[0]);

	return 1;
}

/* Warp to another player, from a car OR on foot. The spot is the same either way:
 * beside/behind the target, on resident ground. What lands there is whatever this
 * machine is currently controlling -- the car if we are driving, our own
 * pedestrian if we are not. Being warped TO works on foot too: the target's pose
 * comes from the owner's MP_TAG_PED packets (tp->pedX/Y/Z below), so a peer does
 * not need a car to be reachable. */
int MpWarpToPlayer(int playerId)
{
	MP_PLAYER* me;
	MP_PLAYER* tp;
	CAR_DATA* myc = NULL;
	CAR_DATA* tgt;
	LPPEDESTRIAN myped = NULL;
	int onFoot;
	int tx, ty, tz, dir, fx, fz, rx, rz, i;
	int bestX = 0, bestZ = 0, bestRoad = 0, found = 0;

	if (!gMp.running)
		return 0;

	me = MpLocalPlayer();
	tp = MpGetPlayer(playerId);

	if (me == NULL)
	{
		MpNotify("Warp: no local player");
		return 0;
	}

	if (tp == NULL || tp->id == gMp.localPlayerId)
	{
		MpNotify("Warp: that player is not in the match");
		return 0;
	}

	onFoot = (me->carId < 0 || me->carId >= MAX_CARS);

	if (onFoot)
	{
		myped = MpLocalPed();

		if (myped == NULL)
		{
			MpNotify("Warp: no on-foot character to move");
			return 0;
		}
	}
	else
	{
		myc = &car_data[me->carId];
	}

	if (tp->carId >= 0 && tp->carId < MAX_CARS)
	{
		tgt = &car_data[tp->carId];
		tx = tgt->hd.where.t[0];
		ty = tgt->hd.where.t[1];
		tz = tgt->hd.where.t[2];
		dir = tgt->hd.direction;
	}
	else if (tp->pedLastMs != 0 && MpNowMs() - tp->pedLastMs < MP_WARP_PED_FRESH_MS)
	{
		/* ON FOOT: a player out of their car has no car_data slot, only the pose
		 * their owner keeps sending (MP_TAG_PED -> MpHandlePedState). Land beside
		 * that, facing their way (ped heading is the same 0..4095 yaw). This used
		 * to refuse with "that player has no car to go to". */
		tgt = NULL;
		tx = tp->pedX;
		ty = tp->pedY;
		tz = tp->pedZ;
		dir = tp->pedHeading & 0xFFF;
	}
	else
	{
		MpNotify("Warp: no position for that player yet");
		return 0;
	}
	(void)tgt;

	/* Aim the streamer at the TARGET first, so the region its neighbours fall in
	 * has a chance of being present before the candidates are judged. Harmless
	 * when the spot the target is standing on is not landable. */
	MpWarpStreamTo(tx, tz);

	/* The target's axes: forward (fx,fz) and a perpendicular (rx,rz). The offsets
	 * are in those, so "behind" stays behind whichever way they are pointing
	 * when the menu is used. */
	fx = (int)rsin(dir);
	fz = (int)rcos(dir);
	rx = fz;
	rz = -fx;

	for (i = 0; i < (int)(sizeof(kWarpAlong) / sizeof(kWarpAlong[0])); i++)
	{
		int px = tx + ((fx * kWarpAlong[i] + rx * kWarpAcross[i]) >> 12);
		int pz = tz + ((fz * kWarpAlong[i] + rz * kWarpAcross[i]) >> 12);
		int onRoad;

		/* Test the GROUND this candidate stands on, not the target's: a region
		 * that is not resident is not a landing site, however good its coordinates
		 * look. No spooling here - one destination gets streamed below, because
		 * forcing region after region evicts the ones just loaded (four barrels). */
		if (!MpWarpSpotOk(px, pz))
			continue;

		onRoad = JerRoadAt(px, ty, pz) ? 1 : 0;

		/* A clear road point beats anything already found, and anything is better
		 * than nothing: this is also the "nearest road node to where we meant to
		 * go" fallback, since the ring starts beside the target and rings out. */
		if (!found || (onRoad && !bestRoad))
		{
			bestX = px;
			bestZ = pz;
			bestRoad = onRoad;
			found = 1;
		}

		if (bestRoad)
			break;
	}

	if (!found)
	{
		MpNotify("Warp: no ground there to land on");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] warp: nothing to land on near player %d (%s) at %d,%d "
				"(region %d data=%d resident=%d) - staying put\n",
				tp->id, tp->name, tx, tz, jer_map_region_of(tx, tz),
				jer_map_region_has_data(jer_map_region_of(tx, tz)),
				jer_map_region_resident(jer_map_region_of(tx, tz)));

		return 0;
	}

	/* Make the CHOSEN spot real, once, and confirm it: the single stream, and the
	 * answer afterwards is what says whether we may land. */
	if (!MpWarpStreamTo(bestX, bestZ))
	{
		MpNotify("Warp: could not stream the ground there");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] warp: chosen spot %d,%d (region %d) still not resident after "
				"spooling - REFUSING rather than landing in the void\n",
				bestX, bestZ, jer_map_region_of(bestX, bestZ));

		return 0;
	}

	if (onFoot)
	{
		/* ON FOOT: move our own pedestrian, not a car. The same landing spot,
		 * the same heading as the target. The pose goes out immediately rather
		 * than waiting for the next periodic send, so the teleport is one jump on
		 * every peer instead of a walk between the old and new positions. */
		if (!MpPlaceOwnPedAt(myped, bestX, bestZ, dir))
		{
			MpNotify("Warp: no ground at that spot");
			return 0;
		}

		MpSendOwnPedState();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] warp (on foot): to player %d (%s) at %d,%d,%d - %d units along "
				"their axis (negative = behind), %s\n",
				tp->id, tp->name, myped->position.vx, -myped->position.vy,
				myped->position.vz,
				(int)(((long)fx * (bestX - tx) + (long)fz * (bestZ - tz)) >> 12),
				bestRoad ? "on a road" : "off-road");
	}
	else
	{
		JER_ARGS_RESET_CAR rc;

		if (!MpPlaceOnGround(myc, bestX, bestZ, dir))
		{
			MpNotify("Warp: no ground at that spot");

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] warp: spot %d,%d has no ground under it - not moving\n",
					bestX, bestZ);

			return 0;
		}

		/* Arrive stopped, and pointing the way the target is - not pointing AT
		 * the target: the heading that matters is the one the driver can see out
		 * of, and a car dropped in at an arbitrary angle is one they have to sort
		 * out before they can drive. */
		myc->hd.speed = 0;
		myc->wheel_angle = 0;

		/* Re-seated because cameraAngle is an absolute world yaw, not an offset. */
		player[0].cameraAngle = myc->hd.direction + 1536;
		player[0].cameraCarId = myc->id;

		rc.carId = me->carId;
		jer_fire(JER_EVENT_RESET_CAR, &rc);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] warp: to player %d (%s) at %d,%d - %d units along their axis (negative = behind), %s\n",
				tp->id, tp->name, myc->hd.where.t[0], myc->hd.where.t[2],
				(int)(((long)fx * (bestX - tx) + (long)fz * (bestZ - tz)) >> 12),
				bestRoad ? "on a road" : "off-road");
	}

	return 1;
}

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

int MpRespawnOnRoad(void)
{
	MP_PLAYER* me;
	CAR_DATA* cp;
	int cx, cy, cz;
	int bestX = 0, bestZ = 0, found = 0, bestRing = 0, bestDir = -1;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
	{
		MpNotify("Respawn: you are on foot with no car");
		return 0;
	}

	cp = &car_data[me->carId];

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] respawn reason: Respawn (pause menu / Restart row / test) - slot %d damage %d felony %d; last rx %s conn %d frame %lu (now %lu)\n",
			me->carId, cp->totalDamage, (int)cp->felonyRating, gMpLastRxTag, gMpLastRxConn, gMpLastRxFrame, gMp.frame);

	cx = cp->hd.where.t[0];
	cy = cp->hd.where.t[1];
	cz = cp->hd.where.t[2];

	/* THE NEAREST ROAD, not the death-respawn ring.
	 *
	 * MpFindRespawnSpot deliberately picks a spot 12-30k units away: a DEATH
	 * should not put you back on the road you crashed on. The pause row is the
	 * other question ("I am wedged" / "I fell in the void"), and using the death
	 * search here teleported the car 21k units to somewhere the ground may not
	 * have streamed - the "Respawn, then keep falling" loop. */
	found = MpFindRoadSpotNear(cx, cy, cz, -1, &bestX, &bestZ, &bestDir, &bestRing);

	if (!found)
	{
		MpNotify("Respawn: no road near enough to put you on");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] respawn: no road within %d units of %d,%d (region %d data=%d) "
				"- leaving the car where it is\n",
				MP_ROAD_RINGS * MP_ROAD_RING_STEP, cx, cz,
				jer_map_region_of(cx, cz), jer_map_region_has_data(jer_map_region_of(cx, cz)));

		return 0;
	}

	/* Make the chosen road point real, and confirm it - the same gate, and the same
	 * reason, as the warp: MapHeight answers 0 for ground that is not streamed, and
	 * 0 is also a perfectly good height. */
	if (!MpWarpStreamTo(bestX, bestZ))
	{
		MpNotify("Respawn: could not stream the road there");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] respawn: road point %d,%d (region %d) still not resident after "
				"spooling - REFUSING rather than respawning into the void\n",
				bestX, bestZ, jer_map_region_of(bestX, bestZ));

		return 0;
	}

	{
		JER_ARGS_RESET_CAR rc;
		int placeDir = (bestDir >= 0) ? bestDir : MpRoadHeadingNear(bestX, bestZ, cp->hd.direction & 0xfff);

		/* InitCarPhysics builds an UPRIGHT orientation out of the heading, and
		 * MpPlaceOnGround resolves the y from the road there. y=0 was the old
		 * shortcut and it put the car below a road at y=2048: it fell, the engine
		 * called it a death, and the respawn dropped it again. */
		if (!MpPlaceOnGround(cp, bestX, bestZ, placeDir))
		{
			MpNotify("Respawn: no ground on that road");

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] respawn: road point %d,%d has no ground under it - staying put\n",
					bestX, bestZ);

			return 0;
		}

		cp->hd.speed = 0;
		cp->wheel_angle = 0;

		/* the other half: repaired. The same fields the Repair row clears. */
		cp->totalDamage = 0;
		memset(cp->ap.damage, 0, sizeof(cp->ap.damage));
		CreateDentableCar(cp);
		cp->ap.needsDenting = 0;

		/* Respawn implies repair AND a clean slate: drop the wanted level so the
		 * cops we just escaped are not instantly back on us from the old rating.
		 * (Repair alone leaves the felony, which is the "I repaired but they are
		 * still chasing me" surprise.) */
		MpSelfClearFelony();

		player[0].cameraAngle = cp->hd.direction + 1536;
		player[0].cameraCarId = cp->id;

		rc.carId = me->carId;
		jer_fire(JER_EVENT_RESET_CAR, &rc);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] respawn: on the nearest road at %d,%d - ring %d, from (%d,%d) - repaired and upright\n",
				cp->hd.where.t[0], cp->hd.where.t[2], bestRing, cx, cz);
	}

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
			/* No car to drive: drop the timeline too, or walking back into a
			 * body (or a different one) would replay the old car's motion. */
			MpPlayClear(pl->id);
			MpReleaseRemoteCar(pl);

			/* remember what they walked away from: if they leave, it goes too */
			if (pl->carId >= 0 && pl->carId < MAX_CARS)
			{
				pl->leftCarSlot1 = pl->carId + 1;
				pl->leftCarModel = car_data[pl->carId].ap.model;
			}

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

		/* FIRST REAL POSE WINS. A later body state can still be the
		 * frontend/loading car at 0,0; overwriting the good pose with it is what
		 * built the joiner at the origin. The sender gates keep such a state out,
		 * but the ordering here is what makes the spawn deterministic. */
		if (!pl->joinedPose)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] join pose: player %d wire %d,%d h=%d model=%d frame=%u conn=%d\n",
					pl->id, e.x, e.z, (int)e.heading, (int)e.model, (unsigned)h.frame, connIndex);

			pl->joinedPose = 1;
			pl->joinPoseX = e.x;
			pl->joinPoseZ = e.z;
			pl->joinPoseHeading = (int)(e.heading & 0xfff);
		}

		/* HEAL: the owner's own carstate says they are DRIVING, but this machine
		 * holds no car for them. Believe the owner -- it is the authority for its
		 * car, and this message is current (a player on foot sends NO_CAR, handled
		 * just above, so reaching here with carId < 0 cannot mean "on foot").
		 *
		 * This is a deadlock, not a race. carId is only ever assigned by a spawn,
		 * and the pose adopt below bails on carId < 0, so once a get-OUT had set
		 * carId = -1 / onFoot = 1 nothing here ever looked at the owner's next
		 * "I have a car" again. The re-seat pass (MpSpawnLateJoiners) skips anyone
		 * with onFoot set, so the pair never broke: no car drawn on this machine
		 * for the rest of the match, and the stand-in pedestrian -- whose pedstate
		 * stops the instant its owner is back in a car -- stayed where it was,
		 * walking the spot and playing footstep sounds.
		 *
		 * Clearing onFoot is the whole fix: the re-seat pass runs every 30 frames
		 * and will seat them on its next one. Logged once per get-out/get-in
		 * cycle, so the heal is visible when diagnosing. */
		if (pl->carId < 0 || pl->carId >= MAX_CARS)
		{
			int reused = 0;

			/* BACK IN THE BODY THEY LAST GOT OUT OF. The owner says so explicitly
			 * (MP_CARSTATE_SAME_BODY: walking back into their parked car, or the
			 * pause menu's Change car while on foot, which re-enters it and
			 * re-models it in place). We released that body on their NO_CAR row and
			 * it is still standing here, so put them back on it. Letting the heal
			 * below run instead would clear onFoot and the re-seat pass would call
			 * InitPlayer on ANOTHER slot: the same vehicle twice, one of them an
			 * orphan nobody drives.
			 *
			 * The body is only ours to hand back while it is still that car and
			 * nobody else has taken it: a cleared/recycled slot, a released mesh, a
			 * model that changed under us, a car another player (or WE) now drive -
			 * any of those and this falls back to the spawn path, exactly as before.
			 * The model-change block below then re-models the body if the owner's
			 * carstate names a different vehicle (the Change car case). */
			if ((e.flags & MP_CARSTATE_SAME_BODY) && pl->leftCarSlot1 > 0)
			{
				int slot = pl->leftCarSlot1 - 1;

				if (slot >= 0 && slot < MAX_CARS &&
					slot != (int)player[0].playerCarId &&
					MpGetPlayerByCar(slot) == NULL &&
					car_data[slot].controlType != CONTROL_TYPE_NONE &&
					car_data[slot].ap.carCos != NULL &&
					car_data[slot].ap.model == pl->leftCarModel &&
					JerCarSlotUsable(car_data[slot].ap.model))
				{
					pl->carId = slot;
					pl->onFoot = 0;
					pl->leftCarSlot1 = 0;
					pl->leftCarModel = -1;
					reused = 1;

					/* Back in the body they left: it has been standing still, so
					 * there is no motion to interpolate from the old timeline. */
					MpPlayClear(pl->id);

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] player %d is back in the car they left (slot %d) - "
							"reusing the body instead of spawning another\n",
							pl->id, slot);
				}
			}

			if (!reused)
			{
				if (pl->onFoot)
				{
					pl->onFoot = 0;

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] heal: player %d's carstate says they are driving but we have "
							"no car for them - re-seating them\n", pl->id);
				}

				continue;
			}
		}

		if (MpRemoteTargetsLocalCar(pl->carId, "carstate", pl->id))
		{
			pl->carId = -1;	/* that row's car is not ours to drive from the wire */
			MpPlayClear(pl->id);
			continue;
		}

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

					/* A new body: its timeline starts here, not at the old car's
					 * last sample. */
					MpPlayClear(pl->id);

					cp = &car_data[pl->carId];
				}
			}
		}

		/* OWNER-AUTHORITATIVE: this car belongs to another machine, so its owner is
		 * the truth. Adopt the WHOLE body every snapshot -- no easing, no tolerance.
		 * A contact is a separate message (MP_HIT: the owner of the MOVING car
		 * pushes its own), so there is no push to leave room for here. */
		{
			/* The NEWEST SAMPLE against the pose the playout is displaying. With
			 * the 3-frame buffer this is expected to be about 100 ms of travel
			 * (tens to a couple of hundred units while moving) -- it is the
			 * buffer's lead, NOT a desync metric any more. A spike well beyond
			 * that means a lost sample, an extrapolation or a teleport.
			 * (|d| is world units, dh the heading error in 1/4096 turns.)
			 * Logged on arrival, NOT gated on our own frame counter: the
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

			/* Adopt in full: the owner is the truth for its own car -- but as a
			 * SAMPLE on the owner's timeline, not as a pose to write now. The
			 * playout buffer smooths the gaps between samples; MpApplyRemoteCarPoses
			 * is the only writer of this car's body. */
			MpPlayPush(pl->id, h.frame, &e);

			/* keep the remote car placed and alive: the engine spools a
			 * non-local player car out of the world, so re-assert it
			 * (controlType included, or the spooler parks it). */
			cp->controlType = CONTROL_TYPE_PLAYER;
		}

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
			pl->palette = (int)e.palette;	/* the roster (late joiners) names the owner's colour */

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

			/* defensive: a palette this model does not have (an older peer, or one
			 * cycled on another car) is drawn in the model's own colours */
			pal = CarSlotClampPalette(cp->ap.model, pal);

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
		 * machine's take-a-ride spawn point can be right across the map.
		 *
		 * NEVER FOR A LIVE JOIN. A joiner arrives where the host already is, so
		 * gathering would put it back on top of them; and because this runs in the
		 * state poll, it used to move the car AFTER MpPlaceSelfTick had read it as
		 * "still on the level start", which made place-self abort instead of moving
		 * it away. A live join is placed by MpPlaceSelfTick alone. */
		if (!MpIsHost() && !gMp.localPlaced && !gMp.liveJoin)
		{
			MP_PLAYER* me = MpLocalPlayer();

			if (me != NULL && me->carId >= 0 && me->carId < MAX_CARS && me->carId != pl->carId)
			{
				CAR_DATA* my = &car_data[me->carId];
				MATRIX m;
				int gy;
				int oldX = my->hd.where.t[0];	/* so a REFUSED gather can put us back */
				int oldY = my->hd.where.t[1];
				int oldZ = my->hd.where.t[2];
				int oldDir = my->hd.direction;
				VECTOR* oldSpoolXZ = MainPlayer.spoolXZ;

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

				/* MAKE THE DESTINATION REAL BEFORE WE READ ITS GROUND.
				 *
				 * This gather is a HOP across the map, and the streamer does not follow a
				 * hop: the region we are dropped into is very often not the one that is
				 * resident, and MapHeight answers 0 for ground that is not there
				 * (sdGetCell -> NULL -> 0, dr2roads.c:544). A car placed on that 0 is a
				 * car below the world -- it falls, and the client has "no cells".
				 *
				 * jer_map_spool_to is the SDK's "stream there" call, and this is the same
				 * sequence antfarm and the arena use for their own hops: point the
				 * streamer at the destination first (it follows MainPlayer.spoolXZ), then
				 * ask, and the engine unpacks that region through its own level-start
				 * path -- the 2x2 barrel, its geometry and roadmap and its texture area
				 * together, none of which a bare UnpackRegion would do. */
				{
					static VECTOR sGatherSpool;	/* spoolXZ must outlive this call */
					int gx = my->hd.where.t[0];
					int gz = my->hd.where.t[2];
					int region = jer_map_ready() ? jer_map_region_of(gx, gz) : -1;
					int hasData = (region >= 0) ? jer_map_region_has_data(region) : 0;
					int before = MapHeight((VECTOR*)my->hd.where.t);
					int after = before;
					int resident = (region >= 0) ? jer_map_region_resident(region) : 0;
					int placed = 0;

					if (jer_map_ready() && region >= 0 && hasData)
					{
						if (resident)
						{
							/* Already unpacked and in the barrel: nothing to stream, so do
							 * NOT call the helper. jer_map_spool_to is not free when the
							 * region is resident -- it still runs CheckLoadAreaData +
							 * StartSpooling + UpdateSpool, a synchronous spool pass -- and
							 * calling it on every gather stalled both a pair run and a
							 * 3-seat run (measured). Stream only when the region genuinely
							 * is not in, which is the hop this gather is.
							 *
							 * And be plain about the limit of this test: residency is NOT
							 * the same question as "is there ground under us". A region
							 * can read resident=1 hasData=1 and still give MapHeight 0
							 * (measured on this path), which is why placement is not made
							 * conditional on the height -- on the level's own start that
							 * 0 is correct and the car is fine, so refusing on it would
							 * break the ordinary case. The streamer is what has to be
							 * right, and for a destination that was never streamed this
							 * branch is not taken at all. */
							placed = 1;
						}
						else
						{
							/* THE HOP. A region you are dropped into is never put into a
							 * barrel by the streamer, so the ground there never loads and
							 * MapHeight answers 0: this is the client's "no cells, fell
							 * into the void". jer_map_spool_to is the SDK's "stream there"
							 * call -- the same one antfarm and the arena use for their own
							 * teleports -- and it unpacks the region under
							 * MainPlayer.spoolXZ through the engine's own level-start
							 * path, so the caller must point spoolXZ at the destination
							 * first. That pointer needs to outlive the call, hence the
							 * static. */
							sGatherSpool.vx = gx;
							sGatherSpool.vy = 0;
							sGatherSpool.vz = gz;
							sGatherSpool.pad = 0;
							MainPlayer.spoolXZ = &sGatherSpool;

							/* 0 = there is nothing there to stream (off the map or no
							 * regions), in which case we must not place at all. */
							placed = jer_map_spool_to(gx, gz);
							after = MapHeight((VECTOR*)my->hd.where.t);

							/* And hand the streamer back. Ours pointed at a static, whose
							 * contents cannot follow a moving car, whereas the engine's own
							 * convention is the player car's position vector itself
							 * (cutscene.c:673 does exactly that) -- which is the car we are
							 * moving, so it already follows us. */
							MainPlayer.spoolXZ = oldSpoolXZ;
						}
					}

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] gather ground: region %d (resident=%d, hasData=%d) at %d,%d - MapHeight before %d, after %d, %s\n",
							region, resident, hasData, gx, gz, before, after,
							placed ? (resident ? "already streamed, placing" : "streamed, placing")
								: "nothing to stream, NOT placing");

					if (!placed)
					{
						/* NO GROUND, NO PLACE -- and put the car back. The x/z above was
						 * already written, so without this the refusal would leave the car
						 * standing at the destination with no ground under it: the very void
						 * this is here to prevent. It stays where the level put it -- real
						 * ground by construction -- and tries again on the next carstate,
						 * because localPlaced stays 0. */
						my->hd.where.t[0] = oldX;
						my->hd.where.t[1] = oldY;
						my->hd.where.t[2] = oldZ;
						my->hd.direction = oldDir;

						if (gMpCtx != NULL)
							gMpCtx->jer_log(gMpCtx,
								"[mp] gather: there is no ground to stream at %d,%d (region %d, hasData=%d) - staying where the level put us, will retry\n",
								gx, gz, region, hasData);
					}
					else
					{
						gy = after;	/* the height read immediately before placing */
						if (my->ap.carCos != NULL)
							gy -= my->ap.carCos->wheelDisp[0].vy;
						my->hd.where.t[1] = gy;

						/* Rebuild the handling matrix too: setting only t[]/direction left
						 * the collision box at the OLD spot until the engine next
						 * recomputed it -- and this is a teleport, so it matters. */
						_RotMatrixY(&m, (short)e.heading);
						memcpy(my->hd.where.m, m.m, sizeof(my->hd.where.m));

						/* No need to re-point the streamer here: the engine's spoolXZ is
						 * the player car's own position vector, and this IS that car, so
						 * it follows us to the new spot on its own. (Re-pointing it at a
						 * module static -- which is what this used to do -- leaves the
						 * streamer reading a vector that never moves again.) */

						gMp.localPlaced = 1;

						if (gMpCtx)
							gMpCtx->jer_log(gMpCtx,
								"[mp] gathered next to peer at %d,%d,%d (ground under us, not the peer's y)\n",
								my->hd.where.t[0], my->hd.where.t[1], my->hd.where.t[2]);
					}
				}
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
	cp->ap.palette = (u_char)CarSlotClampPalette(resident, palette);
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

/* LIGHT AND SOUND A MIRRORED POLICE CAR.
 *
 * A cop car belongs to ONE machine's traffic band; everywhere else it is a pose
 * rewritten from the wire, built as CONTROL_TYPE_PLAYER so no AI touches it. That
 * is why the siren never worked for a cop chasing somebody else: the engine turns
 * a cop's lightbar and siren on in cars.c and gamesnd.c only for
 * CONTROL_TYPE_PURSUER_AI, and a mirror never is one. The owner now says so in
 * the row (MP_TRAFFIC_SIREN), and this is what acts on it -- with the ENGINE's own
 * two calls, so a mirrored cop looks and sounds exactly like a local one instead
 * of like a module's approximation:
 *
 *   AddCopCarLight  -- the same flashing bar the engine adds to a pursuing cop;
 *   Start3DSoundVolPitch -- the siren sample CarHasSiren names for that model,
 *                      played on a voice jer_sound_ensure gives us (which refuses
 *                      rather than starving the engine's own SFX).
 *
 * The sound is re-issued every frame, which is how the engine starts a player's
 * siren (StartPlayerCarSounds): the sample itself loops in the SPU.
 *
 * Nothing here touches controlType. Handing a mirror to the traffic AI is the
 * access-violation class this file already documents (see MpReleaseRemoteCar). */
static void MpMirrorSirenTick(void)
{
	int i, playing = 0;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp;
		int siren;

		if (!gMpPeerTraffic[i] || gMpPeerTrafficSiren[i] == 0)
			continue;

		if ((unsigned)(gMp.frame - gMpPeerTrafficSiren[i]) > MP_TRAFFIC_STALE_FRAMES)
			continue;	/* the owner stopped saying it: off, like the pose */

		cp = &car_data[i];

		if (cp->controlType == CONTROL_TYPE_NONE)
			continue;

		AddCopCarLight(cp);

		siren = CarHasSiren(cp->ap.model);

		if (siren == 0)
			continue;

		/* ONE loop at a time: a second siren for the same cop would double the
		 * sample on top of itself. The first mirror found wins, and the engine's
		 * own limit for cop sirens is two anyway. */
		if (playing)
			continue;

		playing = 1;

		/* Say it once a second or so: "did the flag ever arrive, and was it acted
		 * on" is the one question a run of this could not otherwise answer. */
		if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % 30) == 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] siren: mirrored police slot %d is on duty here too (lightbar + siren)\n", i);

		Start3DSoundVolPitch(jer_sound_ensure(&gMpMirrorSirenChannel),
			(siren & 0xff00) >> 8, siren & 0xff,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -10000, 129);
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

		if (MpRemoteTargetsLocalCar(slot, "traffic", -1))
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

		/* Its owner says the siren and lightbar are up. Stamped with the frame so a
		 * cop that stops pursuing (or whose owner stops speaking) goes quiet on its
		 * own, exactly like the pose does. */
		gMpPeerTrafficSiren[slot] = (e[i].flags & MP_TRAFFIC_SIREN) ? gMp.frame : 0;
	}

	/* The host is the hub: relay a client's traffic to the other clients, exactly
	 * as it relays their carstate. */
	if (MpIsHost() && connIndex >= 0)
		MpHostRelay(connIndex, MP_TAG_TRAFFIC, 0, p, len);
}

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
