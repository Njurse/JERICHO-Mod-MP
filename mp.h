/*
 * mp.h -- JERICHO Multiplayer module: shared internal state and API.
 *
 * This header is deliberately free of game headers (like jer_menu.h); the
 * mp_*.c files include their own game headers as needed. It defines:
 *
 *   - MP_CONFIG : the per-instance settings persisted in
 *                 JERICHO/CONFIG/mp.ini (port, player name, host name,
 *                 beacon interval, and the host's lobby mod-check mode).
 *   - MP_PLAYER : one tracked network participant (self or a peer), with
 *                 the CAR_DATA slot it drives -- this is how we tell our
 *                 players apart from genuine AI/NPC traffic.
 *   - MP_STATE  : the module's live session state (role, lobby config,
 *                 the player registry).
 *
 * Transport internals (sockets, recv buffers, the beacon) live in mp_net.c;
 * UI in mp_ui.c; session sync in mp_session.c.
 */
#ifndef MP_H
#define MP_H

#include "mp_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */
typedef struct MP_CONFIG
{
	int  port;			/* UDP+TCP port (default 1318) */
	char playerName[MP_NAME_MAX];	/* this player's name */
	char hostName[MP_NAME_MAX];	/* advertised server name when hosting */
	int  beaconMs;			/* discovery beacon interval */
	int  keepaliveMs;		/* liveness ping cadence */
	int  idleDropMs;		/* silence that drops a peer; 0 = never drop */
	int  modCheck;			/* host lobby setting: MP_MODCHECK_* */
	int  strictVersion;		/* host lobby setting: also require the same build */
	int  car;			/* this machine's vehicle for the match, -1 = level default */
	int  carIsSlot;			/* 1 = `car` is a 1..10 frontend slot, resolved per city */
	int  carCity;			/* the city `car`'s MODEL NUMBER belongs to, -1 = the session's
					 * own (MP_CAR_CITY_SESSION on the wire). A slot is always the
					 * session's city, so this only applies to a raw model number. */
	int  firstNameSet;		/* 0 until the player confirms a name once */

	/* MY COLOUR. Off by default, and the default is the point: with it off a
	 * player's character looks exactly as the game made it, and nothing about
	 * their appearance is being invented by us. Turned on, it tints their own
	 * Tanner, and every other machine sees that colour too. */
	int  colorOn;			/* 0 = keep the original colours (default) */
	int  colorR, colorG, colorB;	/* 0..255 */
} MP_CONFIG;

/* ------------------------------------------------------------------ */
/* Player registry                                                     */
/* ------------------------------------------------------------------ */
typedef struct MP_PLAYER
{
	int  active;			/* slot in use */
	int  id;			/* 0 = host, 1..MP_MAX_PLAYERS-1 = clients */
	char name[MP_NAME_MAX];
	int  carId;
	int  carConfirmed;	/* this player has CHOSEN a car (see MpHandleCar): no car is
				 * built for anyone until then */			/* CAR_DATA slot it drives, -1 = none yet */
	int  car;			/* vehicle (car id) it asked for, -1 = unknown */
	int  carIsSlot;			/* 'car' is a per-city frontend SLOT to resolve, not a model */
	int  carCity;			/* the city that car number belongs to, -1 = the session's own */
	int  palette;			/* that player's car colour (0 = default) */
	int  isLocal;			/* 1 = this machine's own player */
	int  connected;			/* peer link still alive */
	int  isHost;			/* the host's own row (first in the list) */
	int  pingMs;			/* round trip, measured by the host */
	unsigned long lastStateFrame;	/* sim frame we last adopted this car's owner state */
	unsigned long lastHitFrame;	/* sim frame we last reported a contact with this player */

	/* ON FOOT. When the owner has no car we stand a pedestrian in for them, and
	 * hold it here rather than in the engine's ped table: it is ours, and the only
	 * thing that knows how to undo it is us. void* so this header does not need
	 * pedest.h. */
	void* ped;			/* JerNpc* we spawned for this player, or NULL */
	int   pedX, pedY, pedZ;		/* where the owner says it is */
	int   pedHeading;
	int   pedSpeed;
	unsigned long pedLastMs;	/* when we last heard a pose for it */

	/* The owner's carstate says this player is ON FOOT (MP_CARSTATE_NO_CAR).
	 * carId < 0 says the same thing, but only until the next roster arrives:
	 * the roster names the car a player OWNS, so it cannot be used to decide
	 * whether they are driving it. Without this flag a peer re-spawned a car
	 * for a player who was standing on the pavement -- every roster (every
	 * 120 frames), so the stand-in pedestrian appeared and vanished in a loop.
	 * Set by the carstate (which arrives every frame); consulted by the roster's
	 * spawn request and by MpSpawnLateJoiners.
	 *
	 * On OUR OWN row it is set by MpFollowLocalCar when we get out, so that getting
	 * into a car again is reported as a choice (MP_CARQ_CHOSEN) - and only then. */
	int   onFoot;

	/* That player's chosen colour, as THEIR machine reported it. Off means their
	 * character keeps the colours the game gave it. */
	int   colorOn;
	int   colorR, colorG, colorB;
} MP_PLAYER;

/* ------------------------------------------------------------------ */
/* Live module state                                                   */
/* ------------------------------------------------------------------ */
typedef struct MP_STATE
{
	MP_CONFIG config;

	int role;			/* MP_ROLE_* */
	int listenersUp;		/* host: listening socket open */
	int connected;			/* client: session socket open */
	int running;			/* a network level is live */
	int leaving;			/* 1 = we are deliberately leaving (stay quiet) */
	int localPlayerId;		/* this machine's player id */
	int  autoSession;		/* -host/-join/MP_AUTOSTART: bring a match up with no menus */
	int  pendingLaunch;		/* an auto session asked to launch: do it on a frame, not mid-poll */
	int  pendingSpawn;		/* a peer with no car joined: build one on a frame, not mid-poll */

	/* agreed lobby config */
	int gamemode;			/* MP_GAMEMODE_* */
	int city;			/* GameLevel 0..3 */
	int subGame;			/* multiplayer sub-level (gSubGameNumber) */
	int timeOfDay;			/* TIME_* (or -1 = mission default) */
	int weather;			/* WEATHER_* (or -1 = mission default) */
	unsigned int seed;

	int modsMatched;		/* client: our manifest matched the host */
	int localPlaced;
	unsigned long busyUntilMs;		/* client: our own car was gathered next to the host */

	/* the sim frame the session is synchronized on */
	unsigned int frame;

	/* input lockstep */
	int padForPlayer[MP_MAX_PLAYERS];	/* the pad applied this frame per player */
	int inputHave[MP_MAX_PLAYERS];		/* 1 = this player's input arrived */

	/* lower-left info overlay: who joined/left, plus the chat line */
	char notifyText[MP_NOTIFY_MAX][MP_NOTIFY_TEXT_MAX];
	unsigned long notifyUntil[MP_NOTIFY_MAX];	/* ms deadline; 0 = empty */
	int notifyNext;				/* ring write cursor */
	char chatBuf[MP_NOTIFY_TEXT_MAX];	/* pending chat line being typed */
	int chatOpen;				/* 1 = the chat prompt is up */

	MP_PLAYER players[MP_MAX_PLAYERS];
	int playerCount;		/* number of active registry rows */
} MP_STATE;

extern MP_STATE gMp;
extern JERICHO_CONTEXT* gMpCtx;

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */
void MpConfigLoad(void);
void MpConfigSave(void);
/* A non-empty default name for a first run (empty string == not set). */
const char* MpDefaultPlayerName(void);

/* ------------------------------------------------------------------ */
/* Lifecycle / queries                                                 */
/* ------------------------------------------------------------------ */
int  MpIsActive(void);			/* role != NONE */
int  MpIsHost(void);			/* role == HOST */

/* ------------------------------------------------------------------ */
/* Player registry                                                     */
/* ------------------------------------------------------------------ */
MP_PLAYER* MpLocalPlayer(void);
MP_PLAYER* MpGetPlayer(int id);
MP_PLAYER* MpGetPlayerByCar(int carId);	/* NULL when carId is not a player */

/* Is the LOCAL player's car a real CHOICE yet (a pick, or -mpcar), rather than what mp
 * assigned a player who has not picked? carhacks must not put an assignment into the
 * session's import set: it takes a resident slot nobody drives. */
int MpLocalCarChosen(void);
MP_PLAYER* MpAddPlayer(int id, const char* name, int isLocal);
void       MpRemovePlayer(int id);
void       MpReleaseOurCarSlot(int slot);	/* drop the sticky "ours" mark (a leaver's car) */
void       MpResetPlayers(void);

/* ------------------------------------------------------------------ */
/* Transport (mp_net.c)                                                */
/* ------------------------------------------------------------------ */
int  MpNetStart(void);			/* platform socket init; 1 = ok */
void MpNetShutdown(void);		/* close every socket (SHUTDOWN hook) */
void MpNetPoll(int waitMs);		/* service sockets (PRE_SIM/FRAME) */
unsigned long MpNowMs(void);		/* monotonic milliseconds */
int MpDebugOn(void);			/* is MP_DEBUG set? cached; safe to call per frame */
const char* MpTestChatKey(void);	/* MP_TEST_CHATKEY, resolved once */
const char* MpTestCarSelect(void);	/* MP_TEST_CARSELECT, resolved once */
const char* MpTestFrontendJoin(void);	/* MP_TEST_FRONTEND_JOIN, resolved once */
void MpSuppressCrashDialogs(void);
void* MpLocalPedPtr(void);		/* our own player's pedestrian, or NULL in a car */

/* How hard a custom colour is pushed onto a character's palette rows. Strong
 * enough to be unmistakable, short of repainting the whole model flat. */
#define MP_COLOR_STRENGTH 160

int  MpHostBegin(void);			/* open listener + start beaconing */
void MpHostEnd(void);
void MpClientDisconnect(void);

/* Asynchronous join (the UI, -join and MP_AUTOSTART all use it): start, then
 * poll -- so a slow or dead address cannot freeze a frame. */
int  MpClientConnectBegin(const char* host, int port);
void MpClientConnectPoll(unsigned long now);	/* now = the caller's poll clock */
int  MpJoinState(void);			/* MP_JOIN_* */
void MpJoinStateSet(int state);		/* mp_session.c: WELCOME / REJECT */

/* mp_net.c: a fresh, user-initiated join gets its full set of attempts again. The RETRY path
 * must not clear the count (it goes through MpClientConnectBegin), or the retries never end -
 * so the reset lives here, called from the join entry point, and not in the connect itself. */
void MpJoinRetryClear(void);
const char* MpJoinTarget(void);		/* host of the current attempt */
int  MpJoinTargetPort(void);

enum
{
	MP_JOIN_IDLE = 0,	/* nothing in flight */
	MP_JOIN_CONNECTING,	/* socket connecting, or hello awaiting WELCOME */
	MP_JOIN_READY,		/* the host accepted us */
	MP_JOIN_FAILED		/* refused / timed out / rejected */
};

/* Framed sends (envelope + payload) over the session transport. */
int  MpHostBroadcast(const char* tag, int flags, const void* payload, int len);
int  MpHostRelay(int exceptConn, const char* tag, int flags, const void* payload, int len);
int  MpSendToHost(const char* tag, int flags, const void* payload, int len);
int  MpSendConn(int connIndex, const char* tag, int flags, const void* payload, int len);
int  MpConnFindByPlayer(int playerId);
void MpConnAssignPlayer(int connIndex, int playerId);
void MpConnClose(int connIndex);		/* close a peer (after a reject) */
void MpConnShutdownGraceful(int connIndex);
void MpConnSetPing(int connIndex, unsigned long ms);
int  MpPingForPlayer(int playerId);	/* RTT in ms, or -1 -- from the PING/PONG tick */
void MpConnHandshakeDone(int connIndex);	/* this connection has seen a HELLO/WELCOME */
void MpConnEvent(const char* ev, int idx, const char* why);	/* the connection log */
void MpConnLineText(char* out, int cap, int part);	/* the on-screen one (part 0 / 1) */
void MpDiagDump(const char* reason);	/* everything we know -> mp_diag.txt */
void MpConnMatchStarted(void);			/* the level is up: every stage says so */
void MpConnDrop(int idx, const char* why);	/* close a connection, with the real reason */
int  MpConnPlayerId(int connIndex);	/* peer's assigned player id, -1 until hello */
int  MpPeerCount(void);

/* Per-peer link stats for the on-screen readout. Everything here is measured, not
 * guessed: ping from the PING/PONG tick, rx/tx from the transport's byte counters,
 * lossPct from how often a frame passes with nothing arriving. */
typedef struct MP_PEER_STATS
{
	unsigned long rxBytes;
	unsigned long txBytes;
	unsigned long linkMs;	/* how long the link has been up */
	int           pingMs;	/* round trip, from the PING/PONG tick */
	int           lossPct;	/* EWMA of the % of sim frames in which NOTHING arrived
				 * from this peer (0..100), sampled once per frame at the
				 * APPLICATION layer -- TCP never reports loss itself,
				 * so this is the closest honest proxy we have */
} MP_PEER_STATS;

int  MpPeerStats(int playerId, MP_PEER_STATS* out);	/* 0 = no such link */

/* Frames of silence from a remote car's owner before we let the replicated
 * input drive it again (a snapshot-gap fallback; see MpOnNetInput). ~8 frames is
 * about a quarter second at 30 fps. */
#define MP_INPUT_FALLBACK_FRAMES	8

/* Message dispatch: called by the transport for each complete message.
 * Implemented in mp_session.c (handshake, lobby, input, resync, channel). */
void MpHandleMessage(int connIndex, const char* tag,
		     const unsigned char* payload, int len);

/* ------------------------------------------------------------------ */
/* Handshake / session bring-up (mp_session.c)                         */
/* ------------------------------------------------------------------ */
int  MpBuildManifest(MP_MOD_INFO* out, int max);	/* enabled mods; mp_config.c */
void MpBuildSeries(char* out, size_t outSize);		/* release series of JERICHO_BUILD_VERSION; mp_config.c */
unsigned short MpBuildHash(void);			/* digest of that series; mp_config.c */

void MpSendHello(void);			/* client -> host identity + manifest */
int  MpBeginHost(void);			/* become host: listen + advertise + self row */
int  MpBeginJoinAsync(const char* host, int port);	/* the join (non-blocking) */
void MpLeaveSession(void);		/* tear the session down, keep the module */

/* Addon network bridge (mp_bridge.c / jer_net.h). */
void MpBridgeDeliver(const char* channel, int peer, const void* data, int len);

/* ------------------------------------------------------------------ */
/* UDP LAN discovery (mp_net.c)                                        */
/* ------------------------------------------------------------------ */
typedef struct MP_SERVER
{
	int  used;
	char ip[32];
	int  port;
	char hostName[MP_NAME_MAX];
	int  players, maxPlayers, gamemode, city, modsEnforced, inProgress;
	unsigned short modHash;
	unsigned long lastSeenMs;
} MP_SERVER;

/* host: advertise=1 (beacon out); join: advertise=0 (listen for beacons) */
void       MpDiscoveryStart(int advertise);
void       MpDiscoveryStop(void);
void       MpDiscoveryPoll(unsigned long now);
int        MpDiscoveryCount(void);
MP_SERVER* MpDiscoveryGet(int index);
int        MpDiscoveryRevision(void);	/* changes when the visible server set does */

/* Short digest of the enabled-module manifest (implemented in mp.c). */
unsigned short MpModHash(void);

/* ------------------------------------------------------------------ */
/* Session sync / start (mp_session.c)                                 */
/* ------------------------------------------------------------------ */
void MpSessionReset(void);
void MpTrafficBandClear(void);		/* release the slots we reserved (session teardown) */
int  MpTrafficBand(int* first, int* last);	/* this machine's traffic slot band, 0 = none */
int  MpStartMatch(void);		/* host: launch the agreed level for all players */
int  MpOnNetSpawn(void* userdata, void* args);	/* add remote player cars */
int  MpOnCarContact(void* userdata, void* args);	/* JER_EVENT_COLLISION: relay a car-to-car contact */
void MpLockstepFrame(void);		/* one input-lockstep tick (PRE_SIM) */
int  MpInputForPlayer(int id);		/* the pad to apply to this player's car */

/* ------------------------------------------------------------------ */
/* UI (mp_ui.c)                                                        */
/* ------------------------------------------------------------------ */
int MpIsValidAddress(const char* host);	/* dotted-quad check (no DNS in this module) */
void MpUiInit(void);			/* register the frontend menus (jer_frontend) */
void MpUiTick(void);			/* refresh the live lobby menu when needed */

/* Lower-left info overlay (who joined/left) + the chat prompt. */
void MpNotify(const char* text);	/* queue a line for the overlay */
void MpNotifyf(const char* fmt, ...);	/* printf-style MpNotify */

/* Replace OUR OWN vehicle with another one, mid-match (the pause menu's Change
 * car). (city, model) is the pair the wire carries; the model must be one this
 * machine can hold. Returns 1 if the car on the road changed. */
int MpChangeCar(int city, int model);

/* A city's display name (a 0..3 index, or -1 for the session's own city). */
const char* MpCarCityName(int city);

/* The live-car questions mp puts to other modules (carhacks) -- see
 * mp_carquery.h. Both are "no answer is not an error": 0 means nobody knew
 * better, and the caller falls back to the session's own city. */
int MpCarQueryCities(int* out, int max);	/* out: 0..3 city indices; returns the count */
int MpCarQueryLoad(int city, int model);	/* 1 = this machine holds it now */
void MpCarQueryChosen(int city, int model, int changed);	/* notice: what we drive now (mp_carquery.h) */

/* Which slots a city may be offered in, and the model in each. slots[] and models[] are
 * PARALLEL and `max` is the capacity of both. Returns the count; 0 = nobody knows. */
int MpCarQuerySlots(int city, int* slots, int* models, int max);

/* Size a roster array with this. The engine's own per-city list is 10 wide
 * (CarAvailability[4][10], carNumLookup likewise) and a module may offer the same ten; the
 * spare room is so a shorter list never has to be trimmed to fit a caller's buffer. */
#define MP_CAR_LIST_MAX	12

/* The car list a city may be offered - one answer for the pause picker AND the test lever,
 * so the two can never disagree about what is on offer. Both arrays are required (parallel);
 * `max` is their shared capacity.
 *
 * The car mods answer when they can. Without them this falls back to the engine's own
 * frontend table (CarAvailability + carNumLookup), i.e. exactly the list the picker showed
 * before - which is also all a session can honestly offer, because that table is written by
 * the FRONTEND car screen and a session started with -host/-join never opens it. So polling
 * it in game is what made the picker offer four cars a city. */
int MpCarListForCity(int city, int* slots, int* models, int max);

/* The multiplayer meaning of Restart: put this player back at the level's own
 * start, in their car, repaired, with no felony, and KEEP the session (the
 * engine's own restart rebuilds the level, which is not one player's to do in a
 * match). Returns 1 when the reset was applied. */
int MpSoftRestart(void);
void MpChatOpen(void);			/* open the chat prompt (bound to a key in mp.c) */
void MpChatSendText(const char* text);	/* send + locally echo a chat line */
void MpSendChat(const char* text);	/* put a chat line on the wire */
void MpSendInput(int pad);		/* replicate this frame's input (host relays the set) */
void MpSpawnLateJoiners(void);	/* give a car to a player who joined a live match */
void MpHostSendRoster(void);		/* host: publish who is in the match */
int MpOnCarDataSource(void* userdata, void* args);	/* resident car models: seat every player */

/* While a level is loading the other side has nothing to say for the whole load,
 * which is longer than the idle timeout. Every machine that orders a launch
 * marks itself busy for the duration, and the timeout check stands down. */
void MpMarkBusy(int ms);
int  MpBusy(void);
int  MpUiDrawOverlay(void* userdata, void* args);	/* JER_EVENT_DRAW_OVERLAY */
int  MpOnDrawMap(void* userdata, void* args);	/* JER_EVENT_DRAW_MAP */
void MpCarPose(int carId, int* x, int* y, int* z, int* heading);	/* car position (mp.c owns car_data) */
void MpCameraPose(int* x, int* y, int* z, int* yaw);	/* camera position + yaw */
int  MpGetGameLevel(void);		/* the frontend's current city (GameLevel) */
void MpSetSubGame(int n);		/* the multiplayer sub-level (gSubGameNumber) */
void MpUiOpenModeMenu(void);		/* open the Single Player / Multiplayer menu */
void MpUiArmModeMenu(void);		/* ask for it on the NEXT frame - call this from a hook */
void MpUiOpenCarSelect(void);		/* open the stock car select (joining player picks a car) */

/* The typed manual-address field. It lives in mp_ui.c but the keyboard belongs to
 * mp.c (PsyX has ONE text-input slot and mp.c chains it), so these are how the two
 * halves meet: mp.c asks whether the field wants the keys, and hands it what was
 * typed. */
int  MpUiManualEditing(void);
void MpUiManualType(const char* text);	/* one character, or NULL for backspace */
void MpUiManualCommit(void);
void MpUiManualCancel(void);
void MpClientLaunch(void);		/* launch a client into the host's level */
void MpReturnToFrontend(void);		/* end the match, back to the main frontend */
void MpHostByeAll(void);		/* tell every client the match is ending */
void MpClientBye(void);		/* tell the host WE are ending (a deliberate quit) */

/* JER_PAUSE_GAMEOVER's answer: 1 = "handled, this is not a game over" (the caller then
 * returns JER_RESULT_STOP and the engine leaves the game-over pause unarmed), 0 = let the
 * engine have its game over. Puts the local player back on the map and hands their car back
 * whole. */
int  MpRespawnAfterDeath(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_H */
