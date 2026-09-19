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
#include "state.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Session lifecycle                                                   */
/* ------------------------------------------------------------------ */
void MpSessionReset(void)
{
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
	MpSessionReset();
	MpResetPlayers();

	gMp.role = MP_ROLE_HOST;
	gMp.localPlayerId = 0;
	{
		MP_PLAYER* me = MpAddPlayer(0, gMp.config.playerName, 1);
		if (me != NULL) me->carId = 0;
	}

	if (!MpHostBegin())
	{
		gMp.role = MP_ROLE_NONE;
		return 0;
	}

	/* Advertise from the moment the lobby exists. A host who is picking a city
	 * and waiting for people to arrive is exactly what somebody browsing needs
	 * to see, and the beacon carries inProgress = gMp.running, so a browser can
	 * still tell a lobby from a match already under way. */
	MpDiscoveryStart(1);

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
	GameLevel = gMp.city;
	GameType = GAME_TAKEADRIVE;
	NumPlayers = 1;
	gSubGameNumber = 0;

	if (gMp.timeOfDay >= 0)
		wantedTimeOfDay = gMp.timeOfDay;
	if (gMp.weather >= 0)
		wantedWeather = gMp.weather;

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] launching: city %d time %d weather %d\n",
			GameLevel, wantedTimeOfDay, wantedWeather);

	SetState(STATE_GAMESTART);
}

/* Host: broadcast the agreed config, then launch locally for everyone. */
int MpStartMatch(void)
{
	MP_START st;

	if (!MpIsHost() || gMp.running)
		return 0;

	/* the lobby values may still be unset (-1): clamp to valid ones BEFORE
	 * both the broadcast and the local launch so host and clients agree, and
	 * the mission loader never sees an out-of-range index */
	if (gMp.city < 0)
		gMp.city = 0;
	if (gMp.timeOfDay < 0)
		gMp.timeOfDay = 0;
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
	w.gamemode = (uint8_t)gMp.gamemode;
	w.city = (uint8_t)gMp.city;
	w.timeOfDay = (uint8_t)(gMp.timeOfDay < 0 ? 0 : gMp.timeOfDay);
	w.weather = (uint8_t)(gMp.weather < 0 ? 0 : gMp.weather);
	w.seed = gMp.seed;

	MpSendConn(connIndex, MP_TAG_WELCOME, MP_FLAG_RELIABLE, &w, sizeof(w));
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
			pl->modsMatched = matched;
	}

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

	MpAddPlayer(0, "Host", 0);

	{
		MP_PLAYER* me = MpAddPlayer(w.playerId, gMp.config.playerName, 1);

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

		MpUiOpenCarSelect();
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

	for (i = 0; i < MP_MAX_PLAYERS && slot < sp->maxPlayers; i++)
	{
		MP_PLAYER* p = &gMp.players[i];

		if (!p->active || p->isLocal)
			continue;

		PlayerStartInfo[slot] = &ReplayStreams[slot].SourceType;
		memcpy((u_char*)PlayerStartInfo[slot], (u_char*)PlayerStartInfo[0], sizeof(STREAM_SOURCE));

		PlayerStartInfo[slot]->type = 1;
		PlayerStartInfo[slot]->controlType = CONTROL_TYPE_PLAYER;
		PlayerStartInfo[slot]->flags = 0;
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
#define MP_BARRIER_MS 200
#define MP_SYNC_INTERVAL	30	/* host resync snapshot cadence (frames) */
#define MP_SYNC_SNAP_DIST	300	/* divergence that triggers a snap */

int MpInputForPlayer(int id)
{
	if (id < 0 || id >= MP_MAX_PLAYERS)
		return 0;

	return gMp.padForPlayer[id];
}


/* forward: the host resync snapshot + client car replication (defined later) */
static void MpHostSendCarState(void);
static void MpSendCarState(void);

/* One network tick (per simulation frame). Client-authoritative model: every
 * machine replicates the cars it owns; nobody blocks on the network. */
void MpLockstepFrame(void)
{
	if (!gMp.running)
		return;

	++gMp.frame;

	if (MpIsHost())
	{
		MpNetPoll(0);
		MpHostSendCarState();		/* replicate every car we know */
	}
	else
	{
		MpSendCarState();		/* replicate just our own car */
		MpNetPoll(0);
	}
}

/* ------------------------------------------------------------------ */
/* Host state resync (the lockstep fallback)                           */
/* ------------------------------------------------------------------ */
static void MpHostSendCarState(void)
{
	unsigned char buf[sizeof(MP_CARSTATE) + MP_MAX_PLAYERS * sizeof(MP_CARSTATE_ENTRY)];
	MP_CARSTATE h;
	int i, k = 0;

	memset(&h, 0, sizeof(h));
	h.frame = gMp.frame;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];
		MP_CARSTATE_ENTRY e;
		CAR_DATA* cp;

		if (!p->active || p->carId < 0)
			continue;

		cp = &car_data[p->carId];

		memset(&e, 0, sizeof(e));
		e.playerId = (uint8_t)p->id;
		e.x = cp->hd.where.t[0];
		e.y = cp->hd.where.t[1];
		e.z = cp->hd.where.t[2];
		e.heading = cp->hd.direction;

		memcpy(buf + sizeof(MP_CARSTATE) + k * sizeof(e), &e, sizeof(e));
		k++;
	}

	h.count = (uint8_t)k;
	memcpy(buf, &h, sizeof(h));

	MpHostBroadcast(MP_TAG_CARSTATE, 0, buf, (int)(sizeof(h) + k * sizeof(MP_CARSTATE_ENTRY)));
}

static void MpHandleCarState(const unsigned char* p, int len)
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
		if (pl == NULL || pl->isLocal || pl->carId < 0)
			continue;

		cp = &car_data[pl->carId];

		/* keep the remote car placed and alive: the engine spools a
		 * non-local player car out of the world, so re-assert it each frame
		 * (controlType included, or the spooler parks it). */
		cp->controlType = CONTROL_TYPE_PLAYER;
		cp->hd.where.t[0] = e.x;
		cp->hd.where.t[1] = e.y;
		cp->hd.where.t[2] = e.z;
		cp->hd.direction = e.heading;

		/* Client-side gather: the first time we hear a peer's car, drop our
		 * own car right next to it so both players start together -- each
		 * machine's take-a-ride spawn point can be right across the map. */
		if (!MpIsHost() && !gMp.localPlaced)
		{
			MP_PLAYER* me = MpLocalPlayer();

			if (me != NULL && me->carId >= 0 && me->carId != pl->carId)
			{
				CAR_DATA* my = &car_data[me->carId];

				my->hd.where.t[0] = e.x + 600;
				my->hd.where.t[1] = e.y;
				my->hd.where.t[2] = e.z;
				my->hd.direction = e.heading;
				gMp.localPlaced = 1;

				if (gMpCtx)
					gMpCtx->jer_log(gMpCtx, "[mp] gathered next to peer at %d,%d,%d\n", e.x, e.y, e.z);
			}
		}
	}
}

/* The client sends its own car's transform every frame (host-authoritative
 * replication keeps the other machines' view of it correct). */
static void MpSendCarState(void)
{
	unsigned char buf[sizeof(MP_CARSTATE) + sizeof(MP_CARSTATE_ENTRY)];
	MP_CARSTATE h;
	MP_CARSTATE_ENTRY e;
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;

	if (me == NULL || me->carId < 0)
		return;

	cp = &car_data[me->carId];

	memset(&h, 0, sizeof(h));
	h.frame = gMp.frame;
	h.count = 1;

	memset(&e, 0, sizeof(e));
	e.playerId = (uint8_t)me->id;
	e.x = cp->hd.where.t[0];
	e.y = cp->hd.where.t[1];
	e.z = cp->hd.where.t[2];
	e.heading = cp->hd.direction;

	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), &e, sizeof(e));

	MpSendToHost(MP_TAG_CARSTATE, 0, buf, (int)(sizeof(h) + sizeof(e)));
}

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
		MpHandleCarState(payload, len);
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
		return;		/* receiving it IS the liveness proof */

	/* SESSION is not used yet; unknown tags are ignored. */
}
