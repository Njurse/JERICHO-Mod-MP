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
/* gCopDifficultyLevel's range. The engine's own tables are indexed 0..2 --
 * cop_ai.c's speed1/speed2 and its batter thresholds -- so 2 is the ceiling. */
#define MP_COP_DIFF_MAX	2

typedef struct MP_CONFIG
{
	int  port;			/* UDP+TCP port (default 1318) */
	char playerName[MP_NAME_MAX];	/* this player's name */
	unsigned char playerId[MP_PLAYER_ID_LEN];	/* per-install id ([mp] player_id), sent in HELLO */
	char hostName[MP_NAME_MAX];	/* advertised server name when hosting */
	int  beaconMs;			/* discovery beacon interval */
	int  keepaliveMs;		/* liveness ping cadence */
	int  idleDropMs;		/* silence that drops a peer; 0 = never drop */
	int  modCheck;			/* host lobby setting: MP_MODCHECK_* */
	int  strictVersion;		/* host lobby setting: also require the same build */

	/* SESSION RULES -- the host's per-session gameplay settings, chosen in the
	 * Host Game menu and applied on EVERY machine (they ride the wire: see
	 * MP_RULES_* / MP_WELCOME.rules). This is where the host's defaults for the
	 * NEXT session live; the rules the LIVE session is enforcing are in
	 * MP_STATE.rule* -- the two are deliberately separate, because a host may
	 * change its mind in the menu without reaching back into a running match. */
	int  cops;			/* 1 = police cars spawn (CopsAllowed = 1), 0 = they do not */
	int  copDifficulty;		/* gCopDifficultyLevel, 0..MP_COP_DIFF_MAX */
	int  immune;			/* 1 = cops cannot come after you (gPlayerImmune). NOT the
				 * same axis as `cops`: this leaves them on the map and
				 * takes away their reason to bother you. */
	int  invuln;			/* 1 = no car can be damaged (gInvincibleCar) */
	int  palette;			/* palette: our car's colour variant 0..5 (defaultPlayerPalette) */
	int  chatHoldSec;		/* chat_hold_seconds: full-brightness time of a chat line */
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

	/* THE DISCOVERY SERVER (master), for the Online browser. Empty = the Online
	 * screen says so and lists nothing; that is an unset state, not a bad one.
	 * "host", "host:port" and "http://host:port" are all accepted. */
	char masterUrl[128];
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
	int  hasPlayerId;		/* the HELLO carried a JPID block (host side) */
	unsigned char playerId[MP_PLAYER_ID_LEN];	/* that block's id */
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

	/* This player's link to the HOST, as the host reports it (MP_TAG_STATS): what
	 * the scoreboard shows for a row this machine has no connection of its own to. */
	unsigned long netRxBps, netTxBps;
	int   netLoss;			/* 0..100, or MP_STATS_LOSS_UNKNOWN */
	unsigned long netLastMs;	/* when the last report arrived; 0 = never */

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

	/* The car this player last got OUT of (slot + 1, 0 = none) and its model, so a
	 * leaver's abandoned car can go with them -- unless someone else has since
	 * taken it (see MpCleanupLeaverWorld). */
	int   leftCarSlot1;
	int   leftCarModel;

	/* The pose the owner reported while this machine had NO car for them. A live
	 * joiner's first carstate arrives before its local slot is assigned; building
	 * the body at the level start and adopting the pose afterwards is the visible
	 * "spawns in the normal place, then teleports". The spawn pass uses this pose
	 * as the start record, so the car first appears where its owner is. */
	int   joinedPose;		/* 1 = joinPoseX/Z/heading are valid */
	int   joinPoseX, joinPoseZ, joinPoseHeading;

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

	/* THE SESSION RULES IN FORCE. The host sets these at launch; a client adopts
	 * them from MP_START (the launch path) or MP_WELCOME (join in progress).
	 * ruleSet is 0 until they are known, so the applier stays silent instead of
	 * forcing a default of its own onto whatever level is up. */
	int ruleCops;
	int ruleDiff;
	int ruleImmune;
	int ruleInvuln;
	int ruleSet;
	int localPlaced;
	int liveJoin;			/* client joined a match already in progress: no gather, place on a road */
	int gameStarted;		/* a level is up in this session: only then is our car real */
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
	char chatBuf[MP_CHAT_TEXT_MAX];	/* pending chat line being typed (the WIRE limit,
					 * MP_CHAT_TEXT_MAX -- not the notify one, which
					 * happens to be the same 96 today) */
	int chatOpen;				/* 1 = the chat prompt is up */
	int chatSwallowText;			/* drop the next text event: it is the key that
					 * OPENED the prompt (T arrives as text right after
					 * its keydown), not something typed into it */

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

/* Is this car object drawing the reload failsafe's grey stand-in (the host city's
 * car 1) because its real model is not loaded yet? Read by the draw-colour hook. */
int MpCarIsStandIn(int carSlot);

/* SP / pre-session readiness stand-in: put the local car on the host city's car 1 in
 * solid grey while its imported resident slot is not drawable, and put it back when it
 * is. In a running session the reload failsafe owns the same job; this covers the
 * single-player case the draw gate would otherwise leave invisible. */
void MpCarReadinessTick(void);

/* Is the LOCAL player's car a real CHOICE yet (a pick, or -mpcar), rather than what mp
 * assigned a player who has not picked? carhacks must not put an assignment into the
 * session's import set: it takes a resident slot nobody drives. */
int MpLocalCarChosen(void);
MP_PLAYER* MpAddPlayer(int id, const char* name, int isLocal);
void       MpRemovePlayer(int id);
void       MpCleanupLeaverWorld(MP_PLAYER* p);	/* a leaver's stand-in ped + abandoned car */
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
void MpConnLineText(char* out, int cap, int part);	/* the two-line rollup, part 0 / 1 */
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
void MpVersionLabel(char* out, size_t outSize);		/* "0.10.4[-dev]": the JPVR join-toast label */
unsigned int MpModsDigest(const MP_MOD_INFO* m, int n);	/* 24-bit digest of the ENABLED rows (sorted by id) */

void MpSendHello(void);			/* client -> host identity + manifest */
int  MpBeginHost(void);			/* become host: listen + advertise + self row */
int  MpBeginJoinAsync(const char* host, int port);	/* the join (non-blocking) */
void MpLeaveSession(void);		/* tear the session down, keep the module */
void MpKickPlayer(int playerId);	/* HOST: remove a player (sends MP_TAG_KICK) */
void MpCarChangeWaitForget(void);	/* drop a pending "waiting for a spare slot" change */
void MpCarChangeWaitTick(void);		/* retry it, once a frame, from the frame hook */
extern const char* gMpStage;		/* the last stage the frame reached (MP_WATCH) */
void MpSelfRepairCar(void);		/* pause menu: repair the local player's car */
int  MpWarpToPlayer(int playerId);	/* pause menu: put our car down near another player's */
int  MpRespawnOnRoad(void);		/* pause menu: back onto the nearest road, repaired */
void MpSelfClearFelony(void);		/* pause menu: clear the local player's wanted level */

/* Addon network bridge (mp_bridge.c / jer_net.h). */
void MpBridgeDeliver(const char* channel, int peer, const void* data, int len);
int  MpBridgeAccept(const char* channel, int len);	/* 1 = registered name and a
							 * payload a sender could have sent */

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
	int  rules;					/* MP_RULES_PACK'd, from the beacon: names the mode */
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

/* --- the discovery server (master): the Online browser's list ----------------
 *
 * The LAN beacon cannot serve this: a host on the internet is not on our
 * broadcast domain, so the list is FETCHED over HTTP from the master and kept
 * separately from gServers (which MpDiscoveryStop wipes).
 *
 * MpOnlineSetMaster resolves the name, and getaddrinfo BLOCKS, so call it from a
 * menu press or a config change -- never from the frame poll. */
#define MP_ONLINE_MAX	32		/* rows the master's reply is trimmed to */
#define MP_ONLINE_PING_FAILED	(-1)	/* listed, but nothing answers on that address:
					 * the connect completed with an error. A shut
					 * port is the commonest way a listing is
					 * undialable, and it must not look like "slow" */
#define MP_ONLINE_PING_UNSET	0	/* a row nobody has measured (the browser shows a
					 * dash: "not measured" must not look like "fast") */

enum
{
	MP_ONLINE_OFF = 0,		/* no master configured */
	MP_ONLINE_CONNECTING,
	MP_ONLINE_READING,
	MP_ONLINE_READY,		/* the list below is a real reply */
	MP_ONLINE_FAILED
};

typedef struct MP_ONLINE_SERVER
{
	char name[MP_NAME_MAX];
	char host[64];			/* as the master reported it (the peer's address) */
	int  port;
	int  players, maxPlayers;
	int  city;
	int  gamemode;			/* MP_GAMEMODE_*: names the mode (MpModeName) */
	int  rules;			/* MP_RULES_PACK'd session settings (cops etc.) */
	int  pingMs;			/* 0 = not measured yet */
} MP_ONLINE_SERVER;

void MpOnlineSetMaster(const char* url);	/* "" disables. Resolves -- off the frame path */
void MpOnlineRefresh(void);
/* The browser is on screen (1) or is not (0). Nothing contacts the master for a
 * listing unless this is set; the advertise path is independent. */
void MpOnlineSetWatching(int on);
void MpOnlineReset(void);
void MpOnlinePoll(void);			/* non-blocking; safe every frame */

/* --- advertising a session on that same master -------------------------
 * A hosted game lists ITSELF by default: registering is what turns a host into
 * something the Online browser can find, and an unlisted host is only reachable by
 * an address somebody was told out of band. It costs one POST when the session
 * starts, a small heartbeat every MPI_ADVERT_BEAT_MS and an unregister when it
 * ends; none of it blocks, and a master that is down or unreachable changes
 * nothing about hosting (a missing listing is not a broken session).
 *
 * Stopping it for one session is what an EMPTY master_url does: no master, no
 * advertisement, LAN beacon only. */
/* The master a stock build points at. A player should not have to find an address
 * before they can see a server list, and a host should not have to configure one
 * before it can be found. */
#define MP_DEFAULT_MASTER_URL		"http://jerichomp.animetiddi.es:8100"

#define MPI_ADVERT_BEAT_MS		20000	/* keep well under the master's default ttl (60 s) */
#define MPI_ADVERT_RETRY_MS		15000	/* after a master we could not reach */

void MpMasterAdvertiseStart(const char* name, int port, int city, int gamemode, int rules);
void MpMasterAdvertiseCounts(int players, int inProgress);	/* live, every frame */
void MpMasterAdvertiseStop(void);				/* unregister (best effort) */
void MpMasterAdvertisePoll(void);				/* the state machine + the beat */
int  MpOnlineState(void);
const char* MpOnlineError(void);
int  MpOnlineCount(void);
const MP_ONLINE_SERVER* MpOnlineGet(int index);
int  MpOnlineRevision(void);			/* changes when the list does */

/* The master's greeting, if it published one ("motd" + "motdColor" in the
 * master's config). It is carried in the /v1/servers reply, so it costs no extra
 * request, and the browser says it once per distinct text when it opens.
 * MpOnlineMotdRev changes when the TEXT does -- a master that edits its motd mid
 * session is a new line, not a repeat. */
const char* MpOnlineMotd(void);
int  MpOnlineMotdRev(void);
void MpOnlineMotdColor(unsigned char* r, unsigned char* g, unsigned char* b);

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
void MpUiDrawOnline(void);		/* paint the Online table over the menu rows --
					 * from JER_EVENT_FRONTEND_DRAW, never FRAME (the
					 * menu box is drawn after FRAME) */
void MpUiDrawJoin(void);		/* the same table for the LAN browser, whose rows
					 * come from the beacons instead of the master */

/* The local resident SLOT that holds (city, model) here, or -1 when none does.
 * `city` is -1 for the session's own city. A model number alone is not enough --
 * the same number is a different vehicle in every city -- so it is matched together
 * with the city its data came from. Exported because the SINGLE-PLAYER picker needs
 * the very question the apply path asks: the list it builds carries a city-list
 * index and a model, and only this turns those into a slot the level can build. */
int MpResidentSlotForCar(int city, int model);

/* Test lever (MP_TEST_PAUSECAR): drive the pause menu's Change car path. Called from
 * the lockstep tick inside a session, and from the frame hook in SINGLE PLAYER,
 * where there is no lockstep tick at all (MpLockstepFrame returns immediately when
 * not running). Inert unless the lever is set, and a no-op unless the player is
 * actually driving. */
void MpTestPauseCarTick(void);

/* Lower-left info overlay (who joined/left) + the chat prompt. */
void MpNotify(const char* text);	/* queue a line for the overlay */

/* The engine's status console (jer_console.h). Join/leave and chat go there
 * instead of the transient toast ring: the console is the scrolling stream, so
 * a line survives long enough to be read and is greppable in the log. */
void MpConsoleLine(const char* fmt, ...);	/* printf-style status line */
/* A chat line, "<name>: <text>", the name in the speaker's own colour (or the
 * neutral amber when they have not set one). */
/* ink: NULL for an ordinary chat line; otherwise r/g/b to draw BOTH the name and
 * the text in -- the server's own voice for a dedicated server's motd, which
 * carries its colour in the frame (see MP_CHAT.reserved). */
void MpConsoleChat(const char* name, int colorOn, int r, int g, int b, const char* text,
		   const unsigned char* ink);

/* Replace OUR OWN vehicle with another one, mid-match (the pause menu's Change
 * car). (city, model) is the pair the wire carries; the model must be one this
 * machine can hold. Returns 1 if the car on the road changed. */
int MpChangeCar(int city, int model);

/* Auto-rejoin after a server city change (MP_KICK_FLAG_REJOIN): mp_session.c.
 * Tick runs every frame (frontend too); Cancel is Esc or any other join/host. */
void MpRejoinTick(void);
void MpPlaceSelfTick(void);	/* live join: move our car off the level start */
int  MpRejoinActive(void);
void MpRejoinCancel(const char* why);

/* A city's display name (a 0..3 index, or -1 for the session's own city). */
const char* MpCarCityName(int city);

/* The mode a server plays, named from its gamemode id (MP_GAMEMODE_*); session
 * rules such as cops never rename it. Paired with the tools' mode_name() in mp_test.py. */
void MpFormatPlayerId(const unsigned char* id, char* out);	/* dashed UUID, out >= 37 chars */
const char* MpModeName(int gamemode);	/* MP_GAMEMODE_* -> display name */

/* The city name for a row of SOMEONE ELSE'S server: the engine's name, or "-"
 * when the engine has no row for that city. Not MpCarCityName, which says "the
 * session city" -- right for a local picker, wrong in a list of other people's
 * servers. */
const char* MpCityLabel(int city);

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
void MpSetMapShape(int mpLevel, int arena);	/* the session's map: 0 = the full city, 1 = the multiplayer map */

/* The host's session rules, MP_RULES_PACK'd, as they should be published right
 * now. mp_net.c's beacon is the third carrier of this byte (see mp_proto.h). */
uint32_t MpRulesForPeer(void);
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
