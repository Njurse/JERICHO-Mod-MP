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
	int  modCheck;			/* host lobby setting: MP_MODCHECK_* */
	int  strictVersion;		/* host lobby setting: also require the same build */
	int  car;			/* this machine's vehicle for the match, -1 = level default */
	int  carIsSlot;			/* 1 = `car` is a 1..10 frontend slot, resolved per city */
	int  firstNameSet;		/* 0 until the player confirms a name once */
} MP_CONFIG;

/* ------------------------------------------------------------------ */
/* Player registry                                                     */
/* ------------------------------------------------------------------ */
typedef struct MP_PLAYER
{
	int  active;			/* slot in use */
	int  id;			/* 0 = host, 1..MP_MAX_PLAYERS-1 = clients */
	char name[MP_NAME_MAX];
	int  carId;			/* CAR_DATA slot it drives, -1 = none yet */
	int  car;			/* vehicle (car id) it asked for, -1 = unknown */
	int  carIsSlot;			/* 'car' is a per-city frontend SLOT to resolve, not a model */
	int  palette;			/* that player's car colour (0 = default) */
	int  padId;			/* engine pad id bound to it, -1 = none */
	int  isLocal;			/* 1 = this machine's own player */
	int  connected;			/* peer link still alive */
	int  modsMatched;		/* handshake: manifest matched the host */
	int  isHost;			/* the host's own row (first in the list) */
	int  pingMs;			/* round trip, measured by the host */
	unsigned long lastSeenMs;	/* liveness */
	unsigned long lastStateFrame;	/* sim frame we last adopted this car's owner state */
	unsigned long lastHitFrame;	/* sim frame we last reported a contact with this player */
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
	int lastRejectReason;		/* client: MP_REJECT_* from a refused join */
	char lastRejectText[MP_REJECT_TEXT_MAX];

	/* the sim frame the session is synchronized on */
	unsigned int frame;

	/* input lockstep */
	int padForPlayer[MP_MAX_PLAYERS];	/* the pad applied this frame per player */
	int inputHave[MP_MAX_PLAYERS];		/* 1 = this player's input arrived */

	/* lower-left info overlay: who joined/left, plus the chat scaffolding */
	char notifyText[MP_NOTIFY_MAX][MP_NOTIFY_TEXT_MAX];
	unsigned long notifyUntil[MP_NOTIFY_MAX];	/* ms deadline; 0 = empty */
	int notifyNext;				/* ring write cursor */
	char chatBuf[MP_NOTIFY_TEXT_MAX];	/* pending chat line being typed */
	int chatOpen;				/* 1 = the chat prompt is up */
	int inputSetReady;			/* client: the host's full set arrived */

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
int        MpIsPlayerCar(int carId);	/* 1 = a tracked network player's car */
MP_PLAYER* MpAddPlayer(int id, const char* name, int isLocal);
void       MpRemovePlayer(int id);
void       MpResetPlayers(void);

/* ------------------------------------------------------------------ */
/* Transport (mp_net.c)                                                */
/* ------------------------------------------------------------------ */
int  MpNetStart(void);			/* platform socket init; 1 = ok */
void MpNetShutdown(void);		/* close every socket (SHUTDOWN hook) */
void MpNetPoll(int waitMs);		/* service sockets (PRE_SIM/FRAME) */
unsigned long MpNowMs(void);		/* monotonic milliseconds */

int  MpHostBegin(void);			/* open listener + start beaconing */
void MpHostEnd(void);
void MpClientDisconnect(void);

/* Asynchronous join (the UI, -join and MP_AUTOSTART all use it): start, then
 * poll -- so a slow or dead address cannot freeze a frame. */
int  MpClientConnectBegin(const char* host, int port);
void MpClientConnectPoll(void);
int  MpJoinState(void);			/* MP_JOIN_* */
void MpJoinStateSet(int state);		/* mp_session.c: WELCOME / REJECT */
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
int  MpSendToPlayer(int playerId, const char* tag, int flags, const void* payload, int len);
int  MpSendConn(int connIndex, const char* tag, int flags, const void* payload, int len);
int  MpConnFindByPlayer(int playerId);
void MpConnAssignPlayer(int connIndex, int playerId);
void MpConnClose(int connIndex);		/* close a peer (after a reject) */
void MpConnShutdownGraceful(int connIndex);
void MpConnSetPing(int connIndex, unsigned long ms);
int  MpPingForPlayer(int playerId);	/* half-close: flush the refusal, then drain */
void MpConnHandshakeDone(int connIndex);	/* this connection has seen a HELLO/WELCOME */
void MpConnEvent(const char* ev, int idx, const char* why);	/* the connection log */
void MpConnLineText(char* out, int cap, int part);	/* the on-screen one (part 0 / 1) */
void MpDiagDump(const char* reason);	/* everything we know -> mp_diag.txt */
void MpConnMatchStarted(void);			/* the level is up: every stage says so */
void MpConnDrop(int idx, const char* why);	/* close a connection, with the real reason */
int  MpConnPlayerId(int connIndex);	/* peer's assigned player id, -1 until hello */
int  MpPeerCount(void);

/* Per-peer link stats for the on-screen readout. Everything here is measured, not
 * guessed: ping from the PING/PONG tick, rx/tx from the transport's byte counters.
 * lossPct is -1 ("n/a") because the session is TCP, which does not lose datagrams
 * -- a real figure would have to come from a UDP-carried channel we don't have. */
typedef struct MP_PEER_STATS
{
	unsigned long rxBytes;
	unsigned long txBytes;
	unsigned long linkMs;	/* how long the link has been up */
	int           pingMs;	/* round trip, from the PING/PONG tick */
	int           lossPct;	/* % of polls in which NO data arrived from this peer
				 * (0..100), measured at the APPLICATION layer -- TCP
				 * itself never reports loss; -1 = unknown */
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
int  MpBuildManifest(MP_MOD_INFO* out, int max);	/* enabled mods; mp.c */
unsigned short MpBuildHash(void);			/* game build digest; mp.c */

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
void       MpDiscoveryPoll(void);
int        MpDiscoveryCount(void);
MP_SERVER* MpDiscoveryGet(int index);
int        MpDiscoveryRevision(void);	/* changes when the visible server set does */

/* Short digest of the enabled-module manifest (implemented in mp.c). */
unsigned short MpModHash(void);

/* ------------------------------------------------------------------ */
/* Session sync / start (mp_session.c)                                 */
/* ------------------------------------------------------------------ */
void MpSessionReset(void);
int  MpStartMatch(void);		/* host: launch the agreed level for all players */
int  MpOnNetSpawn(void* userdata, void* args);	/* add remote player cars */
void MpLockstepFrame(void);		/* one input-lockstep tick (PRE_SIM) */
int  MpInputForPlayer(int id);		/* the pad to apply to this player's car */

/* ------------------------------------------------------------------ */
/* UI (mp_ui.c)                                                        */
/* ------------------------------------------------------------------ */
int MpIsValidAddress(const char* host);	/* dotted-quad check (no DNS in this module) */
void MpUiInit(void);			/* register the frontend menus (jer_frontend) */
void MpUiTick(void);			/* refresh the live lobby menu when needed */

/* Lower-left info overlay (who joined/left) + chat scaffolding. */
void MpNotify(const char* text);	/* queue a line for the overlay */
void MpNotifyf(const char* fmt, ...);	/* printf-style MpNotify */
void MpChatOpen(void);			/* open the chat prompt (scaffolding) */
void MpChatSendText(const char* text);	/* send + locally echo a chat line */
void MpSendChat(const char* text);	/* put a chat line on the wire */
void MpSendInput(int pad);		/* replicate this frame's input (host relays the set) */
void MpPlaceSpawns(int x, int y, int z, int heading);	/* line every player car up here */
void MpSpawnLateJoiners(void);	/* give a car to a player who joined a live match */
void MpHostSendRoster(void);		/* host: publish who is in the match */

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
void MpUiOpenCarSelect(void);		/* open the stock car select (joining player picks a car) */
void MpClientLaunch(void);		/* launch a client into the host's level */
void MpReturnToFrontend(void);		/* end the match, back to the main frontend */
void MpHostByeAll(void);		/* tell every client the match is ending */
void MpClientBye(void);		/* tell the host WE are ending (a deliberate quit) */

#ifdef __cplusplus
}
#endif

#endif /* MP_H */
