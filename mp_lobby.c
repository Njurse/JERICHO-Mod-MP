/*
 * mp_lobby.c -- split out of mp_session.c
 *
 * The lobby: hosting and joining, the HELLO/WELCOME handshake, the roster,
 * chat, kicks, the listen-host vote and map rotation, rejoin, and the launch
 * into the host's level.
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"
#include "mp_cmd.h"

/* ------------------------------------------------------------------ */
/* LISTEN-HOST VOTE AND MAP ROTATION (0.24.0)                          */
/* ------------------------------------------------------------------ */
/* A game host runs the dedicated server's !vote (dedi/builtin/vote.py: the same
 * grammar, one yes per player, passes at more than half, 30 s, 15 s cooldown)
 * and its map rotation (dedi/builtin/rotation.py). Settings: JERICHO/CONFIG/mp.ini
 * [mp] rounds_per_map (default 0 = off) -- read here with its default in code and
 * never written, so mp.ini is not changed by this feature.
 *
 * A switch is the dedicated server's: every peer is kicked with
 * MP_KICK_FLAG_REJOIN (they come back on their own, MpRejoinArm), then the host
 * itself leaves the level (EndGame) and relaunches the session on the new map
 * from the frontend (MpRelaunchTick, driven by mp.c) without closing the
 * listener, so the rejoining peers find it.
 *
 * THE ROTATION'S DATA MODEL is a list of stops {city, shape (1 = multiplayer
 * map, 0 = full city), arena, area}. `area` is reserved (-1) for stops INSIDE one
 * map (arenas within the big city map) -- the arena work fills it in. The order is
 * every city's multiplayer maps, areas within a city first, then the next city:
 * the dedicated server's default_rotation(). */
#define MP_LVOTE_MS		30000
#define MP_LVOTE_COOLDOWN_MS	15000
#define MP_ROTATE_DELAY_MS	4000	/* after the round's end: let the result show */
#define MP_ROT_CITIES		4
#define MP_ROT_ARENAS_MAX	4	/* Vegas has 4 (MpArenaCount); the rest 2 */

typedef struct
{
	int city;	/* GameLevel 0..3 */
	int shape;	/* 1 = multiplayer map, 0 = the full city */
	int arena;	/* the multiplayer map, 0..3 */
	int area;	/* reserved: a stop inside one map; -1 = the whole level */
} MP_ROT_STOP;

#define MP_JOINLEAVE_LEAVE_DELAY_MS	1000
#define MP_REJOIN_EVERY_MS	4000
#define MP_REJOIN_FIRST_MS	2500
#define MP_REJOIN_QUIET_MS		(MP_REJOIN_FIRST_MS + MP_REJOIN_WINDOW_MS + 30000)
#define MP_REJOIN_WINDOW_MS	20000

static struct
{
	int active;
	MP_ROT_STOP to;
	unsigned long untilMs, cooldownUntilMs;
	unsigned char yes[MP_MAX_PLAYERS];
} gMpLVote;

static struct
{
	int state;		/* 0 none, 1 leave the level, 2 waiting for the frontend */
	MP_ROT_STOP to;
	unsigned long atMs;	/* a delayed switch (rotation) */
	int lastRound, rounds;	/* round counting for the rotation */
	int levelCity, levelShape, levelArena;
	int mode;		/* -1 = keep gMp.gamemode; >= 0 = switch mode on relaunch */
	int modeSeconds;	/* -1 = keep mode time; > 0 = override round seconds on relaunch */
} gMpRelaunch = { 0, { 0, 1, 0, -1 }, 0, -1, 0, -1, -1, -1, -1, -1 };

static const char* const gMpRotCity[MP_ROT_CITIES] = { "CHICAGO", "HAVANA", "VEGAS", "RIO" };

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

static struct
{
	char name[MP_MAX_PLAYERS][MP_NAME_MAX];
	int  count;
	unsigned long untilMs;
} gMpRejoinQuiet;

static unsigned long gMpLeaveSoundDueMs;	/* 0 = no leave sound pending */

/* "<name> left" waits like the leave sound, so a city change's kick-by-kick
 * cancels it; the players come back silently. (mp 0.23.4) */
static struct
{
	char name[MP_MAX_PLAYERS][MP_NAME_MAX];
	unsigned long dueMs[MP_MAX_PLAYERS];
	int count;
} gMpLeftLines;

int MpBeginJoinAsync(const char* host, int port);
void MpLeaveSession(void);
static int MpPlayerCarModel(int car, int isSlot);
static int MpIsWireCode(int v);
uint8_t MpCityToWire(int city);
int MpCityFromWire(uint8_t wire);
static int MpPlayerCarCity(int city, int isSlot);
void MpHostSendRoster(void);
int MpPlayerCarReady(const MP_PLAYER* p);
static uint32_t MpRulesFromConfig(void);
uint32_t MpRulesForPeer(void);
static void MpRulesAdopt(uint32_t packed, const char* how);
int MpStartMatch(void);
static int MpModIsTooling(const char* id);
static int MpModsMatchHost(const MP_MOD_INFO* cm, int cn, int strict);
static void MpSendReject(int connIndex, int reason, const char* text);
static int MpAllocPlayerId(void);
static void MpSendWelcome(int connIndex, int playerId, int matched);
static int MpRotStopCount(void);
static MP_ROT_STOP MpRotStop(int i);
static MP_ROT_STOP MpRotationNext(int city, int shape, int arena);
static void MpRotWhere(const MP_ROT_STOP* s, char* out, int max);
static void MpHostSay(const char* text);
static int MpLVoteCityIndex(const char* w);
static int MpLVoteParse(const char* args, MP_ROT_STOP* to);
static int MpLVoteNeeded(void);
static void MpLVoteTally(char* out, int max);
static void MpListenVote(int pid, const char* args);
static void MpKickPlayerFlags(int playerId, int flags);
static void MpKickPlayerReason(int playerId, const char* reason);

static void MpListenSwitch(const MP_ROT_STOP* to, const char* why);
static int MpChatIsCommand(const char* t);
static void MpRejoinQuietArm(void);
static int MpRejoinQuietTake(const char* name);
static int MpJoinLeaveQuiet(void);
static int MpLeftLinesQuiet(void);
static void MpLeftLinesTick(void);
static void MpRejoinEnd(void);
void MpRejoinCancel(const char* why);
static void MpRejoinArm(void);


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

	/* UPnP: try to open the router for this session's port, so a friend outside
	 * the LAN can reach it. Runs on its own thread and reports on the console;
	 * a failure changes nothing about hosting (LAN and direct address work). */
	MpUpnpStart(gMp.config.port);

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
	MpRolePicksForget("left the session");

	gMp.role = MP_ROLE_NONE;
	gMp.connected = 0;
	gMp.running = 0;

	/* The LAN/multiplayer flow leaves the stock frontend's globals in a
	 * multiplayer state: a joiner's GameType is still GAME_MISSION and the stock
	 * "Multiplayer" button left NumPlayers at 2 for its split-screen path. A
	 * session that has ended must not leak either into the next single-player
	 * Take a Ride, so put the two back to their single-player defaults. */
	GameType = GAME_TAKEADRIVE;
	NumPlayers = 1;

	/* The cars stay in the world, but they stop being ours the moment the session
	 * does: a stale mark would make the next match take a STOCK traffic car for
	 * one of ours and break the traffic instead of protecting it. */
	memset(gMpOurCars, 0, sizeof(gMpOurCars));
	memset(gJerCarNoPingOut, 0, sizeof(gJerCarNoPingOut));

	/* ...and the peer's traffic mirrors, which are OUR builds of THEIR cars. */
	MpTrafficMirrorsClear();

	/* ...and the traffic bands stop being ours too: the engine's reservedSlots must
	 * go back to the engine once we are not replicating traffic (see the band
	 * section below). Leaving them set would keep stock traffic out of six slots
	 * for no reason after the session. */
	MpTrafficBandClear();
}

void MpSendPickedCar(int model, int city)
{
	MP_CAR c;

	if (MpIsHost() || !gMp.connected)
		return;

	memset(&c, 0, sizeof(c));
	c.model = (model >= 0 && model < 0xFF) ? (uint8_t)model : 0xFF;
	c.city = MpCityToWire(city);

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
/* ---- the city byte on the wire (MP_PROTO_VERSION 21) -------------------
 *
 * Stock rows 0..10 travel as their index. A runtime bank (row 11+) sits on whatever
 * row was free on the machine that registered it, so carhacks spells it by a code
 * from its NAME (jer_carquery.h, JER_CARQ_CITYWIRE): 0x80..0xFE. Reading a code
 * this machine does not hold gives back the code itself - >= CITY_COUNT, so every
 * resolver here misses it and the peer gets a stand-in, never a different car - and
 * writing it again forwards it unchanged, so a host re-sending a roster (or a
 * dedicated server, which has no carhacks) relays the original spelling. */
static int MpIsWireCode(int v)
{
	return v >= JER_CITY_WIRE_CODE_FIRST && v <= JER_CITY_WIRE_CODE_LAST;
}

uint8_t MpCityToWire(int city)
{
	JER_CARQ_CITYWIRE_ARGS a;

	if (city < 0 || city == MP_CAR_CITY_SESSION)
		return MP_CAR_CITY_SESSION;
	if (MpIsWireCode(city))
		return (uint8_t)city;

	memset(&a, 0, sizeof(a));
	a.toWire = 1;
	a.city = city;
	a.wire = (city < CITY_COUNT) ? city : MP_CAR_CITY_SESSION;	/* no handler: legacy */
	jer_fire(JER_CARQ_CITYWIRE, &a);
	return (uint8_t)a.wire;
}

/* -1 = the session's own city; 0..CITY_COUNT-1 = a row here; anything else is a car
 * this machine cannot name (opaque). */
int MpCityFromWire(uint8_t wire)
{
	JER_CARQ_CITYWIRE_ARGS a;

	if (wire == MP_CAR_CITY_SESSION)
		return -1;

	memset(&a, 0, sizeof(a));
	a.toWire = 0;
	a.wire = wire;
	a.city = wire;		/* no handler (dedicated server): keep the byte as-is */
	jer_fire(JER_CARQ_CITYWIRE, &a);
	return a.city;
}

static int MpPlayerCarCity(int city, int isSlot)
{
	if (isSlot || city < 0)
		return MP_CAR_CITY_SESSION;
	if (city >= CITY_COUNT && !MpIsWireCode(city))
		return MP_CAR_CITY_SESSION;

	return MpCityToWire(city);
}

/* Every player's link to us, for everyone's scoreboard (MP_TAG_STATS). Host only;
 * the same numbers the host's own scoreboard draws from MpPeerStats. */
void MpHostSendStats(void)
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
void MpHandleStats(const unsigned char* p, int len)
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
		 * config for ourselves (the registry row is fed from the wire). Send the
		 * OWNER'S CHOICE, not this host's effective view: a peer that has the
		 * Havana data can draw Jericho even when this machine cannot. */
		{
			int choice = p->isLocal ? gMp.config.pedModel : p->pedModel;
			e->pedModel = (uint8_t)MpPedModelFromWire(choice);
		}

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

		/* THE CAR THIS PLAYER WALKED AWAY FROM. A late joiner that missed the
		 * get-out has no local body for it; the roster is its snapshot, so name
		 * the abandoned car here (the pose still rides the carstate's
		 * MP_CARSTATE_LEFT_CAR row every frame). 0xff = none. Only while the body
		 * is still parked by us -- a commandeered car is a player car now, not
		 * ours to describe. */
		e->leftModel = 0xff;
		e->leftModelCity = MP_CAR_CITY_SESSION;
		if (p->carId < 0 && p->leftCarSlot1 > 0)
		{
			int left = p->leftCarSlot1 - 1;

			if (left >= 0 && left < MAX_CARS &&
				car_data[left].controlType == CONTROL_TYPE_CIV_AI &&
				car_data[left].ai.padid == &gMpQuietPad &&
				car_data[left].ap.model >= 0 &&
				car_data[left].ap.model < MAX_CAR_RESIDENT_MODELS &&
				residentCarModels[car_data[left].ap.model] >= 0)
			{
				int src = GetCarModelSourceCity(car_data[left].ap.model);

				e->leftModel = (uint8_t)residentCarModels[car_data[left].ap.model];
				e->leftModelCity = MpCityToWire(src);
			}
		}

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
				e->modelCity = MpCityToWire(src);
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
int MpSetStartCar(int slot, MP_PLAYER* p)
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
int MpPlayerCarReady(const MP_PLAYER* p)
{
	return p->carConfirmed || p->id == 0;
}

void MpHandleRoster(const unsigned char* p, int len)
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
			pl->carCity = MpCityFromWire(e->modelCity);
		}

		/* ON FOOT, from the roster: the host names no car (0xff) for a player who
		 * is standing next to theirs. Mark it HERE, from the deterministic late-join
		 * snapshot, instead of waiting for the carstate that says the same thing --
		 * otherwise the spawn request below fires for a player who must never be
		 * re-seated, and only the joinedPose guard (a live-joiner-only fact) stops
		 * it. The carstate keeps its authority: a later real body clears onFoot. */
		if (!pl->isLocal && e->model == 0xff && gMp.running)
			pl->onFoot = 1;

		/* A late joiner's roster arrives before it is running (pre-welcome), when
		 * 0xff alone cannot tell on foot from "lobby, no car yet". Remember it; the
		 * welcome says which (MpHandleWelcome), whether or not a left car is named. */
		pl->rosterNoCar = (e->model == 0xff);

		/* The roster also names the car they left, when they left one. The body
		 * itself is materialized from the carstate (whose pose a roster cannot
		 * carry); this log just makes the snapshot verifiable. Throttled because
		 * the roster repeats every 120 frames. */
		if (!pl->isLocal && e->model == 0xff && e->leftModel != 0xff)
		{
			static unsigned long lastLeftLogMs;

			if (gMpCtx != NULL && (MpNowMs() - lastLeftLogMs) > 2000)
			{
				lastLeftLogMs = MpNowMs();
				gMpCtx->jer_log(gMpCtx,
					"[mp] roster: player %d left model %d (city %d) parked\n",
					pl->id, (int)e->leftModel, (int)e->leftModelCity);
			}
		}

		pl->palette = (int)e->palette;
		/* The roster is the late-join snapshot: it names the character the owner
		 * chose, so a joiner can draw the right ped before JPPD arrives. Keep the
		 * owner's choice here; the apply path resolves whether this level can
		 * actually draw it (and imports Jericho first). */
		{
			int raw = (int)e->pedModel;
			int model = MpPedModelFromWire(raw);

			if (model != pl->pedModel)
				MpLogInboundPedModel("roster", pl->id,
					(int)(pl - gMp.players), raw,
					MpPedModelEffective(model));

			pl->pedModel = model;
		}

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

/* The host's SESSION RULES, packed for the wire. ONE packed byte (MP_RULES_*),
 * carried three ways: MP_WELCOME.rules for a client that joins a running match,
 * the free bits of MP_SESSION.spare for the launch, and MP_BEACON.rules so a
 * LAN browser can name the mode of a server it has not joined. These two
 * functions and those three sites are the only readers and writers,
 * deliberately -- the map shape already showed how easy it is for two carriers
 * to drift. */
static uint32_t MpRulesFromConfig(void)
{
	int gm = (gMp.gamemode >= 0 && gMp.gamemode < MP_GAMEMODE_COUNT) ? gMp.gamemode : 0;

	return MP_RULES_WITH_CHEATS(MP_RULES_PACK(gMp.config.cops, gMp.config.invuln,
		gMp.config.copDifficulty, gMp.config.immune), gMp.config.allowCheats[gm]);
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
		return MP_RULES_WITH_CHEATS(MP_RULES_PACK(gMp.ruleCops, gMp.ruleInvuln, gMp.ruleDiff,
			gMp.ruleImmune), gMp.ruleCheats);

	return MpRulesFromConfig();
}

/* mp 0.25: may the pause menu's cheats be used right now? Always outside a
 * session (single player is the player's own business); in one, the host's Allow
 * cheats rule once it is known, the mode's default until then. */
int MpCheatsAllowed(void)
{
	if (!gMp.running)
		return 1;

	if (gMp.ruleSet)
		return gMp.ruleCheats;

	return MpModeAllowsCheats(gMp.gamemode);
}

uint32_t MpAdvertRules(void)
{
	uint32_t r = MpRulesForPeer();

	if (gMp.config.hostPassword[0] != '\0')
		r |= MP_ADV_PASSWORD;

	return r;		/* a listen host is never MP_ADV_DEDICATED */
}

/* Adopt a packed rule byte and remember it as in force. `how` names the carrier,
 * because "the rules are not what I set" is otherwise unattributable. */
static void MpRulesAdopt(uint32_t packed, const char* how)
{
	int cops = MP_RULES_COPS(packed);
	int invuln = MP_RULES_INVULN(packed);
	int diff = MP_RULES_DIFF(packed);
	int immune = MP_RULES_IMMUNE(packed);
	/* an older host never sends the cheats rule: the mode's own default stands */
	int cheats = MP_RULES_CHEATS_SET(packed) ? MP_RULES_CHEATS(packed)
		: MpModeAllowsCheats(gMp.gamemode);

	if (diff > MP_COP_DIFF_MAX)
		diff = MP_COP_DIFF_MAX;

	gMp.ruleCops = cops;
	gMp.ruleInvuln = invuln;
	gMp.ruleImmune = immune;
	gMp.ruleDiff = diff;
	gMp.ruleCheats = cheats;
	gMp.ruleSet = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] session rules from %s: cops=%s cop difficulty=%d immune=%s invulnerable=%s cheats=%s\n",
			how, cops ? "on" : "off", diff, immune ? "on" : "off", invuln ? "on" : "off",
			cheats ? "allowed" : "off");
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
		gMp.city, gMp.gamemode, (int)MpAdvertRules());
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
			((uint16_t)(rules & 0x7F) << 1) |
			((gBootMpArena & MP_SESSION_ARENA_MASK) << MP_SESSION_ARENA_SHIFT));
	}

	/* The roster goes first, so a client knows how many player cars to spawn
	 * BEFORE its level loads. Without it the client added no car for the host
	 * and left the level's own AI car sitting in that slot. */
	MpHostSendRoster();

	/* ...then round 1, so every machine has the round state before it launches. */
	MpHostRoundBegin();

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
			  + 5 + MP_HELLO_VER_MAX + 5 + MP_PASSWORD_MAX + 5];
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

	/* the password block (mp_proto.h, MP_HELLO_PW_TAG): only when one was typed */
	if (gMp.config.joinPassword[0] != '\0')
	{
		int pl = (int)strlen(gMp.config.joinPassword);

		if (pl > MP_PASSWORD_MAX)
			pl = MP_PASSWORD_MAX;
		memcpy(buf + len, MP_HELLO_PW_TAG, 4);
		buf[len + 4] = (unsigned char)pl;
		memcpy(buf + len + 5, gMp.config.joinPassword, (size_t)pl);
		len += 5 + pl;
	}

	/* the arenas block (mp_proto.h, MP_HELLO_ARENA_TAG): always, last */
	memcpy(buf + len, MP_HELLO_ARENA_TAG, 4);
	buf[len + 4] = (unsigned char)MpArenaLocalBits();
	len += 5;

	MpSendToHost(MP_TAG_HELLO, MP_FLAG_RELIABLE, buf, len);

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] hello sent (%d mods)\n", n);
}

/* bake (and its old id, d1cars) is offline tooling. It is not a join
 * requirement: a host that has it on must still take a client that has it off. */
static int MpModIsTooling(const char* id)
{
	return id != NULL && (strncmp(id, "bake", MP_MOD_ID_MAX) == 0 ||
		strncmp(id, "d1cars", MP_MOD_ID_MAX) == 0);
}

/* Does the client's manifest satisfy the host's policy? */
static int MpModsMatchHost(const MP_MOD_INFO* cm, int cn, int strict)
{
	MP_MOD_INFO hm[MP_MAX_MODS];
	int hn = MpBuildManifest(hm, MP_MAX_MODS);
	int i, j, found;

	for (i = 0; i < cn; i++)
	{
		if (MpModIsTooling(cm[i].id))
			continue;

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
		if (MpModIsTooling(hm[j].id))
			continue;

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
	w.arena = (uint8_t)(gBootMpArena & MP_SESSION_ARENA_MASK);	/* which map, 0..MP_ARENA_MAX: the arena belongs in the session, not a local -mp */
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

	/* The round state AFTER the welcome (a client takes it only once it is one), so
	 * a joiner -- live or in the lobby -- has it before it plays. The others hear of
	 * the new row on the next frame's change check, or in the lobby at START. */
	MpHostSendRound(connIndex);

	/* ...and, in a timer mode, the marked player and the clocks (MpHostSendMark is a
	 * no-op for Take a Ride). */
	MpHostSendMark(connIndex);

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
void MpHandleCar(int connIndex, const unsigned char* p, int len)
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
	pl->carCity = MpCityFromWire(c.city);
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

/* The JPPW password a hello carries after its manifest, JPID and JPVR blocks (each
 * optional, in that order). 1 and `out` filled when there is one, else 0. */
static int MpHelloPassword(const unsigned char* p, int len, int modCount, char* out, size_t outLen)
{
	int at = (int)(sizeof(MP_HELLO) + (size_t)modCount * sizeof(MP_MOD_INFO));
	int n;

	out[0] = '\0';

	if (len >= at + 4 + MP_PLAYER_ID_LEN && memcmp(p + at, MP_HELLO_ID_TAG, 4) == 0)
		at += 4 + MP_PLAYER_ID_LEN;

	if (len >= at + 5 && memcmp(p + at, MP_HELLO_VER_TAG, 4) == 0)
		at += 5 + p[at + 4];

	if (len < at + 5 || memcmp(p + at, MP_HELLO_PW_TAG, 4) != 0)
		return 0;

	n = p[at + 4];
	if (n > MP_PASSWORD_MAX)
		n = MP_PASSWORD_MAX;
	if (n > (int)outLen - 1)
		n = (int)outLen - 1;
	if (len < at + 5 + n)
		return 0;

	memcpy(out, p + at + 5, (size_t)n);
	out[n] = '\0';
	return 1;
}

void MpHandleHello(int connIndex, const unsigned char* p, int len)
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

	/* A password-protected host (JMP host settings, [mp] host_password) admits only
	 * a hello that carries the same one. Checked before anything else is spent on
	 * the joiner; the reason tells their game to ask for it. */
	if (gMp.config.hostPassword[0] != '\0')
	{
		char got[MP_PASSWORD_MAX + 1];
		int has = MpHelloPassword(p, len, n, got, sizeof(got));

		if (!has || strncmp(got, gMp.config.hostPassword, MP_PASSWORD_MAX) != 0)
		{
			if (gMpCtx)
				gMpCtx->jer_log(gMpCtx, "[mp] reject: %s from '%s'\n",
					has ? "wrong password" : "no password", h.playerName);

			MpSendReject(connIndex, MP_REJECT_PASSWORD,
				has ? "wrong password" : "this server needs a password");
			MpConnShutdownGraceful(connIndex);
			return;
		}
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
			pl->carCity = MpCityFromWire(h.reserved[2]);
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
					at += 5 + p[at + 4];
				}

				/* ...the optional password block, then the arenas block */
				if (len >= at + 5 && memcmp(p + at, MP_HELLO_PW_TAG, 4) == 0)
					at += 5 + p[at + 4];

				pl->arenaBits = 0;
				if (len >= at + 5 && memcmp(p + at, MP_HELLO_ARENA_TAG, 4) == 0)
					pl->arenaBits = p[at + 4];
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
		MpConsoleLine("%s", toast);	/* once: the centre notice repeated it (mp 0.23.4) */
	}

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] player %d '%s' joined (%d/%d)%s\n",
			id, h.playerName, gMp.playerCount, MP_MAX_PLAYERS,
			matched ? "" : " [mods differ]");
}

/* The default order: every city's multiplayer maps in turn (Vegas MP 2 and 3
 * only when this machine and every player has them: MpArenaEveryoneHas).
 * Built on each call so a missing bake, or a peer without the files, drops them. */
static int MpRotStops(MP_ROT_STOP* out, int max)
{
	int c, a, n = 0;

	for (c = 0; c < MP_ROT_CITIES; c++)
	{
		for (a = 0; a < MpArenaCount(c) && a < MP_ROT_ARENAS_MAX; a++)
		{
			if (!MpArenaEveryoneHas(c, a) || n >= max)
				continue;
			out[n].city = c;
			out[n].shape = 1;
			out[n].arena = a;
			out[n].area = -1;
			n++;
		}
	}
	return n;
}

static int MpRotStopCount(void)
{
	MP_ROT_STOP s[MP_ROT_CITIES * MP_ROT_ARENAS_MAX];

	return MpRotStops(s, MP_ROT_CITIES * MP_ROT_ARENAS_MAX);
}

static MP_ROT_STOP MpRotStop(int i)
{
	MP_ROT_STOP s[MP_ROT_CITIES * MP_ROT_ARENAS_MAX];
	int n = MpRotStops(s, MP_ROT_CITIES * MP_ROT_ARENAS_MAX);

	return s[(n > 0) ? (i % n) : 0];	/* arenas 0/1 always count, so n >= 8 */
}

static MP_ROT_STOP MpRotationNext(int city, int shape, int arena)
{
	int i;

	for (i = 0; i < MpRotStopCount(); i++)
	{
		MP_ROT_STOP s = MpRotStop(i);

		if (s.city == city && s.shape == shape && (!shape || s.arena == arena))
			return MpRotStop(i + 1);
	}
	return MpRotStop(0);
}

static void MpRotWhere(const MP_ROT_STOP* s, char* out, int max)
{
	int c = (s->city >= 0 && s->city < MP_ROT_CITIES) ? s->city : 0;

	if (s->shape)
	{
		const char* name = MpArenaTitle(c, s->arena);

		if (name != NULL)
			snprintf(out, max, "%s %s", gMpRotCity[c], name);
		else
			snprintf(out, max, "%s mp %d", gMpRotCity[c], s->arena);
	}
	else
		snprintf(out, max, "%s CITY", gMpRotCity[c]);
}

/* A line from the host as the SERVER voice (no player row: MpHandleChat inks it). */
static void MpHostSay(const char* text)
{
	MP_CHAT c;

	memset(&c, 0, sizeof(c));
	c.playerId = 0xFF;		/* no player: the server voice */
	c.reserved[0] = 0xFF;		/* the dedicated server's vote ink, #FFD24A */
	c.reserved[1] = 0xD2;
	c.reserved[2] = 0x4A;
	snprintf(c.text, sizeof(c.text), "%s", text);

	MpConsoleChat(NULL, 0, 0, 0, 0, c.text, c.reserved);
	MpHostBroadcast(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
}

static int MpLVoteCityIndex(const char* w)
{
	int i;

	if (w[0] >= '0' && w[0] <= '3' && w[1] == '\0')
		return w[0] - '0';

	for (i = 0; i < MP_ROT_CITIES; i++)
	{
		const char* a = gMpRotCity[i];
		const char* b = w;

		while (*a != '\0' && *b != '\0' && *a == ((*b >= 'a' && *b <= 'z') ? *b - 'a' + 'A' : *b))
		{
			a++;
			b++;
		}
		if (*a == '\0' && *b == '\0')
			return i;
	}
	return -1;
}

/* vote.py parse_target: "city <city> [mp [0-3]]", "mp [0-3]", legacy "<city> [mp ..]".
 * The arena is checked against the city by the caller (MpArenaEveryoneHas).
 * Fills `to` from the current level for what the words leave out. 0 = bad. */
static int MpLVoteParse(const char* args, MP_ROT_STOP* to)
{
	char buf[64], *w[5];
	int n = 0, i = 0, city;
	char* p;

	snprintf(buf, sizeof(buf), "%s", args);
	for (p = strtok(buf, " "); p != NULL && n < 5; p = strtok(NULL, " "))
	{
		char* q;

		for (q = p; *q; q++)
			if (*q >= 'A' && *q <= 'Z')
				*q = (char)(*q - 'A' + 'a');
		w[n++] = p;
	}

	to->city = GameLevel;
	to->shape = gBootMpLevel ? 1 : 0;
	to->arena = gBootMpArena;
	to->area = -1;

	if (n == 0)
		return 0;

	if (strcmp(w[0], "mp") != 0)
	{
		if (strcmp(w[0], "city") == 0)
			i++;
		if (i >= n || (city = MpLVoteCityIndex(w[i])) < 0)
			return 0;
		to->city = city;
		to->shape = 0;
		to->arena = 0;
		i++;
		if (i >= n)
			return 1;
		if (strcmp(w[i], "mp") != 0)
			return 0;
	}

	/* w[i] == "mp" */
	to->shape = 1;
	to->arena = gBootMpArena;	/* "mp" alone keeps the current multiplayer map */
	i++;
	if (i < n)
	{
		if (w[i][0] < '0' || w[i][0] > '3' || w[i][1] != '\0' || i + 1 < n)
			return 0;
		to->arena = w[i][0] - '0';
	}
	return 1;
}

static int MpLVoteNeeded(void)
{
	return gMp.playerCount / 2 + 1;
}

static void MpLVoteTally(char* out, int max)
{
	char where[24];
	int i, yes = 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		yes += gMpLVote.yes[i];
	MpRotWhere(&gMpLVote.to, where, sizeof(where));
	snprintf(out, max, "%s: %d/%d yes", where, yes, MpLVoteNeeded());
}

/* "!vote ..." from `pid` (the host itself or a peer), on the HOST. */
static void MpListenVote(int pid, const char* args)
{
	char line[96], tally[48], where[24];
	MP_PLAYER* pl = MpGetPlayer(pid);
	unsigned long now = MpNowMs();
	MP_ROT_STOP to;
	int i, yes = 0;

	while (*args == ' ')
		args++;

	if (args[0] == '\0')
	{
		if (gMpLVote.active)
		{
			MpLVoteTally(tally, sizeof(tally));
			snprintf(line, sizeof(line), "vote in progress -- %s", tally);
		}
		else
			snprintf(line, sizeof(line), "no vote; !vote <city> [mp [0-3]] or !vote mp [0-3]");
		MpHostSay(line);
		return;
	}

	if (strcmp(args, "cancel") == 0)
	{
		if (pid != gMp.localPlayerId)
			return;		/* handled by MpListenCommand auth */
		if (!gMpLVote.active)
		{
			MpHostSay("no vote in progress");
			return;
		}
		gMpLVote.active = 0;
		gMpLVote.cooldownUntilMs = now + MP_LVOTE_COOLDOWN_MS;
		MpHostSay("Vote cancelled by the host");
		return;
	}

	if (!MpLVoteParse(args, &to))
	{
		MpHostSay("usage: !vote <city> [mp [0-3]] or !vote mp [0-3] (MP 2/3: Vegas)");
		return;
	}

	/* MP 2/3 exist in Vegas only, and only when everyone has the files
	 * (this machine's install, and every peer's JPAR hello bits). */
	if (to.shape && (to.arena >= MpArenaCount(to.city) || !MpArenaEveryoneHas(to.city, to.arena)))
	{
		MpHostSay(to.arena >= MpArenaCount(to.city)
			? "that city has only MP 0 and MP 1"
			: "not everyone has that map installed (BAKE_D1_CARS builds Vegas MP 2/3)");
		return;
	}

	if (!gMpLVote.active)
	{
		if ((long)(gMpLVote.cooldownUntilMs - now) > 0)
		{
			snprintf(line, sizeof(line), "next vote in %lus", (gMpLVote.cooldownUntilMs - now + 999) / 1000);
			MpHostSay(line);
			return;
		}
		if (to.city == GameLevel && to.shape == (gBootMpLevel ? 1 : 0) &&
			(!to.shape || to.arena == gBootMpArena))
		{
			MpHostSay("the game is already on that map");
			return;
		}
		memset(&gMpLVote, 0, sizeof(gMpLVote));
		gMpLVote.active = 1;
		gMpLVote.to = to;
		gMpLVote.untilMs = now + MP_LVOTE_MS;
		MpRotWhere(&to, where, sizeof(where));
		snprintf(line, sizeof(line), "%s started a vote for %s -- type the same !vote to agree (30s)",
			pl != NULL ? pl->name : "?", where);
		MpHostSay(line);
	}
	else if (to.city != gMpLVote.to.city || to.shape != gMpLVote.to.shape ||
		(to.shape && to.arena != gMpLVote.to.arena))
	{
		MpHostSay("another vote is running; wait for it to end");
		return;
	}

	if (pid < 0 || pid >= MP_MAX_PLAYERS || gMpLVote.yes[pid])
		return;

	gMpLVote.yes[pid] = 1;
	MpLVoteTally(tally, sizeof(tally));
	snprintf(line, sizeof(line), "%s voted -- %s", pl != NULL ? pl->name : "?", tally);
	MpHostSay(line);

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		yes += gMpLVote.yes[i];

	if (yes >= MpLVoteNeeded())
	{
		gMpLVote.active = 0;
		gMpLVote.cooldownUntilMs = now + MP_LVOTE_COOLDOWN_MS;
		MpListenSwitch(&gMpLVote.to, "vote passed");
	}
}

/* Per frame on the HOST in a match: the vote's clock, and the rotation's count. */
void MpListenRotationTick(void)
{
	unsigned long now = MpNowMs();
	int perMap;

	if (!MpIsHost() || !gMp.running || gMpRelaunch.state != 0)
		return;

	if (gMpLVote.active && (long)(now - gMpLVote.untilMs) >= 0)
	{
		char line[96], tally[48];

		MpLVoteTally(tally, sizeof(tally));
		snprintf(line, sizeof(line), "Vote failed -- %s", tally);
		MpHostSay(line);
		gMpLVote.active = 0;
		gMpLVote.cooldownUntilMs = now + MP_LVOTE_COOLDOWN_MS;
	}

	/* a new level (a vote, our own switch, a fresh session): count from zero */
	if (gMpRelaunch.levelCity != GameLevel || gMpRelaunch.levelShape != (gBootMpLevel ? 1 : 0) ||
		gMpRelaunch.levelArena != gBootMpArena)
	{
		gMpRelaunch.levelCity = GameLevel;
		gMpRelaunch.levelShape = gBootMpLevel ? 1 : 0;
		gMpRelaunch.levelArena = gBootMpArena;
		gMpRelaunch.rounds = 0;
		gMpRelaunch.lastRound = MpRoundNumber();
		gMpRelaunch.atMs = 0;
	}

	if (gMpRelaunch.atMs != 0)
	{
		if ((long)(now - gMpRelaunch.atMs) >= 0)
		{
			MP_ROT_STOP to = gMpRelaunch.to;

			gMpRelaunch.atMs = 0;
			MpListenSwitch(&to, "map rotation");
		}
		return;
	}

	/* A game mode stays on the map the host picked: the round count only moves the
	 * map on for a mode whose table row allows map cycling (mp_mode.c mapCycle, 0
	 * for every mode), so mp.ini rounds_per_map no longer rotates a mode's rounds.
	 * A passed !vote still switches (MpListenVote -> MpListenSwitch). */
	if (!MpModeAllowsMapCycle(gMp.gamemode))
		return;

	if (!MpModeUsesTimer(gMp.gamemode) || MpMarkPhase() != MP_MARK_PHASE_ENDED ||
		MpRoundNumber() == gMpRelaunch.lastRound)
		return;

	gMpRelaunch.lastRound = MpRoundNumber();
	gMpRelaunch.rounds++;

	perMap = jer_config_get_int("mp", "rounds_per_map", 0);	/* default in code; never written */

	if (perMap > 0 && gMpRelaunch.rounds >= perMap)
	{
		char line[96], where[24];

		gMpRelaunch.to = MpRotationNext(GameLevel, gBootMpLevel ? 1 : 0, gBootMpArena);
		gMpRelaunch.atMs = now + MP_ROTATE_DELAY_MS;
		if (gMpRelaunch.atMs == 0)
			gMpRelaunch.atMs = 1;
		MpRotWhere(&gMpRelaunch.to, where, sizeof(where));
		snprintf(line, sizeof(line), "%d round%s on this map - next: %s", perMap, perMap == 1 ? "" : "s", where);
		MpHostSay(line);
	}
}

static void MpKickPlayerFlags(int playerId, int flags)
{
	MP_KICK k;
	int conn = MpConnFindByPlayer(playerId);

	if (conn < 0)
		return;

	memset(&k, 0, sizeof(k));
	k.playerId = (uint8_t)playerId;
	k.reserved[0] = (uint8_t)flags;
	MpSendConn(conn, MP_TAG_KICK, MP_FLAG_RELIABLE, &k, sizeof(k));
}

static void MpListenSwitch(const MP_ROT_STOP* to, const char* why)
{
	char line[96], where[24];
	int i;

	MpRotWhere(to, where, sizeof(where));
	snprintf(line, sizeof(line), "%s: switching to %s -- everyone reconnects", why, where);
	MpHostSay(line);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] listen host: %s -> city %d shape %d arena %d; kicking peers with REJOIN, relaunching\n",
			why, to->city, to->shape, to->arena);

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		if (gMp.players[i].active && gMp.players[i].id != gMp.localPlayerId)
			MpKickPlayerFlags(gMp.players[i].id, MP_KICK_FLAG_REJOIN);

	gMpRelaunch.to = *to;
	gMpRelaunch.state = 1;
}

int MpRelaunchActive(void)
{
	return gMpRelaunch.state != 0;
}

/* From mp.c's frame hook, every frame. MP_RELAUNCH_END_LEVEL: the caller leaves
 * the level (EndGame). MP_RELAUNCH_BUSY: a relaunch owns the session this frame
 * (skip the "host left the match" teardown). */
int MpRelaunchTick(int inFrontend)
{
	if (gMpRelaunch.state == 1)
	{
		gMpRelaunch.state = 2;
		return inFrontend ? MP_RELAUNCH_BUSY : MP_RELAUNCH_END_LEVEL;
	}

	if (gMpRelaunch.state != 2)
		return MP_RELAUNCH_NONE;

	if (!inFrontend)
		return MP_RELAUNCH_BUSY;

	/* back in the frontend: the same START the host's menu would give, on the
	 * new map, with the listener and the session still up */
	gMpRelaunch.state = 0;
	GameLevel = gMpRelaunch.to.city;
	MpSetMapShape(gMpRelaunch.to.shape, gMpRelaunch.to.arena);
	gMpLaunched = 0;
	gMp.running = 0;

	if (gMpRelaunch.mode >= 0)
	{
		int nextMode = gMpRelaunch.mode;

		gMpRelaunch.mode = -1;
		MpSetSessionGamemode(nextMode);
	}
	if (gMpRelaunch.modeSeconds > 0)
	{
		int nextSec = gMpRelaunch.modeSeconds;

		gMpRelaunch.modeSeconds = -1;
		MpSetSessionModeSeconds(nextSec);
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] listen host: relaunching on city %d shape %d arena %d\n",
			GameLevel, gMpRelaunch.to.shape, gMpRelaunch.to.arena);

	if (!MpStartMatch() && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] listen host: the relaunch did not start\n");

	return MP_RELAUNCH_BUSY;
}

/* Chat: the host is the echo point, so a line is announced
 * exactly once on every peer. Clients send to the host, which republishes to
 * everyone (including the sender); the host announces its own line directly. */
/* "!login secret", "/kick 2": a chat COMMAND, the same test the dedicated server's
 * commands plugin uses ('!' or '/' then a non-space, non-'!'/'/' character). Only a
 * dedicated server runs commands; a game host has none, so it must never relay such a
 * line -- it may carry a password meant for a server. */
static void MpHostReply(int pid, int connIndex, const char* text)
{
	static const unsigned char ink[3] = { 0xFF, 0xE0, 0x8A };
	MP_CHAT c;

	if (text == NULL || text[0] == '\0')
		return;

	if (pid == gMp.localPlayerId)
	{
		MpConsoleChat(NULL, 0, 0, 0, 0, text, ink);
		return;
	}

	if (connIndex < 0)
		connIndex = MpConnFindByPlayer(pid);
	if (connIndex < 0)
		return;

	memset(&c, 0, sizeof(c));
	c.playerId = 0xFF;
	c.reserved[0] = ink[0];
	c.reserved[1] = ink[1];
	c.reserved[2] = ink[2];
	snprintf(c.text, sizeof(c.text), "%s", text);
	MpSendConn(connIndex, MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
}

static int MpCmdAsciiLower(int c)
{
	return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

static int MpCmdStrIEq(const char* a, const char* b)
{
	while (*a && *b)
	{
		if (MpCmdAsciiLower((unsigned char)*a) != MpCmdAsciiLower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static int MpCmdStrIPrefix(const char* s, const char* prefix)
{
	while (*prefix)
	{
		if (*s == '\0' || MpCmdAsciiLower((unsigned char)*s) != MpCmdAsciiLower((unsigned char)*prefix))
			return 0;
		s++;
		prefix++;
	}
	return 1;
}

static MP_PLAYER* MpFindPlayerByToken(const char* tok)
{
	char* end = NULL;
	long slot;
	int i;
	MP_PLAYER* match = NULL;
	int matches = 0;

	if (tok == NULL || tok[0] == '\0')
		return NULL;

	if (tok[0] >= '0' && tok[0] <= '9')
	{
		slot = strtol(tok, &end, 10);
		if (end != tok && *end == '\0' && slot >= 0 && slot < MP_MAX_PLAYERS)
		{
			MP_PLAYER* p = MpGetPlayer((int)slot);
			if (p != NULL)
				return p;
		}
	}

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];
		if (p->active && MpCmdStrIEq(p->name, tok))
			return p;
	}

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];
		if (p->active && MpCmdStrIPrefix(p->name, tok))
		{
			match = p;
			matches++;
		}
	}

	return (matches == 1) ? match : NULL;
}

static void MpListenKick(int pid, int connIndex, const char* args)
{
	char buf[128];
	char* words[8];
	int nw = 0, n, i;
	MP_PLAYER* target = NULL;
	char reason[64];
	char kickMsg[MP_KICK_TEXT_LEN];
	char line[128];

	reason[0] = '\0';
	snprintf(buf, sizeof(buf), "%s", args != NULL ? args : "");
	{
		char* p = buf;
		while (*p != '\0' && nw < 8)
		{
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '\0')
				break;
			words[nw++] = p;
			while (*p != '\0' && *p != ' ' && *p != '\t')
				p++;
			if (*p != '\0')
				*p++ = '\0';
		}
	}

	for (n = nw; n > 0; n--)
	{
		char cand[128];

		cand[0] = '\0';
		for (i = 0; i < n; i++)
		{
			if (i > 0 && strlen(cand) + 1 < sizeof(cand))
				strcat(cand, " ");
			if (strlen(cand) + strlen(words[i]) < sizeof(cand))
				strcat(cand, words[i]);
		}
		target = MpFindPlayerByToken(cand);
		if (target != NULL)
		{
			for (i = n; i < nw; i++)
			{
				if (i > n && strlen(reason) + 1 < sizeof(reason))
					strcat(reason, " ");
				if (strlen(reason) + strlen(words[i]) < sizeof(reason))
					strcat(reason, words[i]);
			}
			break;
		}
	}

	if (target == NULL)
	{
		MpHostReply(pid, connIndex, "kick who? (!kick <slot|name> [reason])");
		return;
	}
	if (target->id == gMp.localPlayerId)
	{
		MpHostReply(pid, connIndex, "cannot kick the host");
		return;
	}

	if (reason[0] != '\0')
	{
		snprintf(kickMsg, sizeof(kickMsg), "Kicked by the host: %s", reason);
		snprintf(line, sizeof(line), "%s was kicked by the host: %s", target->name, reason);
	}
	else
	{
		snprintf(kickMsg, sizeof(kickMsg), "You were kicked by the host");
		snprintf(line, sizeof(line), "%s was kicked by the host", target->name);
	}

	MpHostSay(line);
	MpKickPlayerReason(target->id, kickMsg);
}

static void MpListenHelp(int pid, int connIndex, const char* args, int isHost)
{
	char word[24];
	size_t n = 0;
	const char* p = args;

	while (*p != '\0' && *p != ' ' && *p != '\t' && n + 1 < sizeof(word))
		word[n++] = (char)MpCmdAsciiLower((unsigned char)*p++);
	word[n] = '\0';

	if (word[0] == '\0')
	{
		if (isHost)
		{
			MpHostReply(pid, connIndex,
				"Host commands: !map !mode !endvote !endround !restart !kick !vote !help");
		}
		else
		{
			MpHostReply(pid, connIndex,
				"Commands: !vote <city|mp>, !mode, !help [cmd]");
		}
		return;
	}

	if (strcmp(word, "map") == 0 || strcmp(word, "changemap") == 0)
		MpHostReply(pid, connIndex, "!map <city> [mp [0-3]] | !map mp [0-3] -- force map switch (host)");
	else if (strcmp(word, "mode") == 0 || strcmp(word, "gamemode") == 0)
		MpHostReply(pid, connIndex, "!mode [0-3|ride|fox|bomb|cnr] [minutes] -- show or switch mode (host)");
	else if (strcmp(word, "endvote") == 0 || strcmp(word, "cancelvote") == 0)
		MpHostReply(pid, connIndex, "!endvote -- cancel the active map vote (host)");
	else if (strcmp(word, "endround") == 0 || strcmp(word, "skipround") == 0 || strcmp(word, "skip") == 0)
		MpHostReply(pid, connIndex, "!endround -- end the current timed round now (host)");
	else if (strcmp(word, "restart") == 0 || strcmp(word, "restartround") == 0)
		MpHostReply(pid, connIndex, "!restart -- restart the current round now (host)");
	else if (strcmp(word, "kick") == 0)
		MpHostReply(pid, connIndex, "!kick <slot|name> [reason] -- remove a player (host)");
	else if (strcmp(word, "vote") == 0)
		MpHostReply(pid, connIndex, "!vote <city> [mp [0-3]] | !vote mp [0-3] | !vote cancel");
	else
		MpHostReply(pid, connIndex, "unknown command (try !help)");
}

static void MpListenCommand(int pid, int connIndex, const char* text)
{
	MP_CMD_PARSED cmd;
	char line[128];
	int isHost = (pid >= 0 && pid == gMp.localPlayerId);

	if (!MpCmdParse(text, &cmd))
		return;

	if (!MpCmdAuthorize(pid, gMp.localPlayerId, cmd.adminOnly))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] denied !%s from player %d (host only)\n",
				cmd.name, pid);
		snprintf(line, sizeof(line), "!%s is for the host only", cmd.name);
		MpHostReply(pid, connIndex, line);
		return;
	}

	switch (cmd.kind)
	{
	case MP_CMD_HELP:
		MpListenHelp(pid, connIndex, cmd.args, isHost);
		break;

	case MP_CMD_VOTE:
		MpListenVote(pid, cmd.args);
		break;

	case MP_CMD_ENDVOTE:
		MpListenVote(pid, "cancel");
		break;

	case MP_CMD_MAP:
	{
		MP_ROT_STOP to;

		if (cmd.args[0] == '\0')
		{
			MP_ROT_STOP cur = { GameLevel, gBootMpLevel ? 1 : 0, gBootMpArena, -1 };
			char where[24];

			MpRotWhere(&cur, where, sizeof(where));
			snprintf(line, sizeof(line), "map: %s -- !map <city> [mp [0-3]] | !map mp [0-3]", where);
			MpHostReply(pid, connIndex, line);
			break;
		}
		if (!MpLVoteParse(cmd.args, &to))
		{
			MpHostReply(pid, connIndex, "usage: !map <city> [mp [0-3]] | !map mp [0-3]");
			break;
		}
		if (to.shape && (to.arena >= MpArenaCount(to.city) || !MpArenaEveryoneHas(to.city, to.arena)))
		{
			MpHostReply(pid, connIndex, to.arena >= MpArenaCount(to.city)
				? "that city has only MP 0 and MP 1"
				: "not everyone has that map installed (BAKE_D1_CARS builds Vegas MP 2/3)");
			break;
		}
		if (to.city == GameLevel && to.shape == (gBootMpLevel ? 1 : 0) &&
			(!to.shape || to.arena == gBootMpArena))
		{
			MpHostReply(pid, connIndex, "the game is already on that map");
			break;
		}
		gMpLVote.active = 0;
		gMpRelaunch.mode = -1;
		gMpRelaunch.modeSeconds = -1;
		MpListenSwitch(&to, "host command");
		break;
	}

	case MP_CMD_MODE:
	{
		int mode = -1, sec = -1;

		if (cmd.args[0] == '\0')
		{
			if (MpModeUsesTimer(gMp.gamemode))
				snprintf(line, sizeof(line), "mode: %d (%s), %ds rounds",
					gMp.gamemode, MpModeName(gMp.gamemode), MpSessionModeSeconds());
			else
				snprintf(line, sizeof(line), "mode: %d (%s)",
					gMp.gamemode, MpModeName(gMp.gamemode));
			MpHostReply(pid, connIndex, line);
			break;
		}
		if (!MpCmdParseMode(cmd.args, &mode, &sec))
		{
			MpHostReply(pid, connIndex, "usage: !mode <0-3|ride|fox|bomb|cnr> [minutes]");
			break;
		}
		{
			MP_ROT_STOP cur = { GameLevel, gBootMpLevel ? 1 : 0, gBootMpArena, -1 };

			gMpLVote.active = 0;
			gMpRelaunch.mode = mode;
			gMpRelaunch.modeSeconds = sec;
			snprintf(line, sizeof(line), "mode %s", MpModeName(mode));
			MpListenSwitch(&cur, line);
		}
		break;
	}

	case MP_CMD_ENDROUND:
		if (!gMp.running)
		{
			MpHostReply(pid, connIndex, "match is not running yet");
			break;
		}
		if (!MpModeUsesTimer(gMp.gamemode))
		{
			MpHostReply(pid, connIndex, "current mode has no round timer");
			break;
		}
		if (MpHostMarkForceEnd())
			MpHostSay("Round ended by the host");
		else
			MpHostReply(pid, connIndex, "no active round to end");
		break;

	case MP_CMD_RESTART:
		if (!gMp.running)
		{
			MpHostReply(pid, connIndex, "match is not running yet");
			break;
		}
		if (MpHostMarkForceRestart())
			MpHostSay("Round restarted by the host");
		else
			MpHostReply(pid, connIndex, "cannot restart round right now");
		break;

	case MP_CMD_KICK:
		MpListenKick(pid, connIndex, cmd.args);
		break;

	case MP_CMD_NONE:
	default:
		if (gMpCtx != NULL && !isHost)
			gMpCtx->jer_log(gMpCtx, "[mp] dropped chat command '!%s' from player %d (not relayed)\n",
				cmd.name, pid);
		snprintf(line, sizeof(line), "unknown command '!%s' (try !help)", cmd.name);
		MpHostReply(pid, connIndex, line);
		break;
	}
}

void MpSendChat(const char* text)
{
	MP_CHAT c;

	if (!MpIsActive() || text == NULL || text[0] == '\0')
		return;

	memset(&c, 0, sizeof(c));
	c.playerId = (uint8_t)(gMp.localPlayerId < 0 ? 0 : gMp.localPlayerId);
	snprintf(c.text, sizeof(c.text), "%s", text);

	if (MpIsHost() && MpCmdIsCommand(c.text))
	{
		MpListenCommand(gMp.localPlayerId, -1, c.text);
		return;
	}

	if (MpIsHost())
	{
		int r, g, b;
		int on = MpModePlayerColour(gMp.gamemode, gMp.localPlayerId, gMp.config.colorOn,
			gMp.config.colorR, gMp.config.colorG, gMp.config.colorB, &r, &g, &b);

		/* the name's ink goes through the game mode's colour seam (unchanged in
		 * Take a Ride: no teams, so the player's own colour) */
		MpConsoleChat(gMp.config.playerName, on, r, g, b, c.text, NULL);
		MpHostBroadcast(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
	}
	else
	{
		MpSendToHost(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
	}
}

void MpHandleChat(int connIndex, const unsigned char* p, int len)
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

	/* Commands on a game host are handled here and never relayed to peers. */
	if (MpIsHost() && MpCmdIsCommand(c.text))
	{
		if (from >= 0)
			MpListenCommand(from, connIndex, c.text);
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

			int r, g, b;
			int on = MpModePlayerColour(gMp.gamemode, pl->id, pl->colorOn,
				pl->colorR, pl->colorG, pl->colorB, &r, &g, &b);

			if (admin)
				snprintf(tagged, sizeof(tagged), "[Admin] %s", pl->name);

			/* the speaker's ink through the game mode's colour seam (see above) */
			MpConsoleChat(admin ? tagged : pl->name, on, r, g, b, c.text, ink);
		}
		else
			MpConsoleChat(NULL, 0, 0, 0, 0, c.text, ink);
	}

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_CHAT, MP_FLAG_RELIABLE, &c, sizeof(c));
}

/* The host ended the match: tell the player and go back to the frontend. */
void MpHandleLeave(int connIndex, const unsigned char* p, int len)
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
static void MpKickPlayerReason(int playerId, const char* reason)
{
	struct
	{
		MP_KICK k;
		char text[MP_KICK_TEXT_LEN];
	} pkt;
	int conn, pktLen;

	if (!MpIsHost())
		return;

	if (playerId <= 0)
		return;

	conn = MpConnFindByPlayer(playerId);

	if (conn < 0)
		return;

	memset(&pkt, 0, sizeof(pkt));
	pkt.k.playerId = (uint8_t)playerId;
	pktLen = (int)sizeof(MP_KICK);

	if (reason != NULL && reason[0] != '\0')
	{
		snprintf(pkt.text, sizeof(pkt.text), "%s", reason);
		pktLen = (int)sizeof(pkt);
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] kicking player %d (conn %d)%s%s\n",
			playerId, conn, (reason != NULL && reason[0] != '\0') ? ": " : "",
			(reason != NULL && reason[0] != '\0') ? reason : "");

	MpSendConn(conn, MP_TAG_KICK, MP_FLAG_RELIABLE, &pkt, pktLen);

	/* The removal on THIS side is the kicked client's own LEAVE (MpLeaveSession
	 * replies with one), exactly as for any other leaver, which is what keeps the
	 * roster/car cleanup in one place. A wedged client that never replies is reaped
	 * by the ordinary 30 s liveness check. (Half-closing here instead was tried and
	 * reverted: the FIN raced the KICK, so the client saw a bare connection loss
	 * and never printed "kicked by the host".) */
}

void MpKickPlayer(int playerId)
{
	MpKickPlayerReason(playerId, NULL);
}

int MpRejoinActive(void)
{
	return gMpRejoin.active;
}

static void MpRejoinQuietArm(void)
{
	int i;

	memset(&gMpRejoinQuiet, 0, sizeof(gMpRejoinQuiet));

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		const MP_PLAYER* pl = &gMp.players[i];

		if (!pl->active || pl->isLocal || pl->name[0] == '\0')
			continue;

		snprintf(gMpRejoinQuiet.name[gMpRejoinQuiet.count], MP_NAME_MAX, "%s", pl->name);
		gMpRejoinQuiet.count++;
	}

	gMpRejoinQuiet.untilMs = MpNowMs() + MP_REJOIN_QUIET_MS;
	gMpLeaveSoundDueMs = 0;	/* the leaves this kick is part of */
	gMpLeftLines.count = 0;	/* ...and their "left" lines */
}

/* 1 = `name` was in the match before the city change and this is its return:
 * the entry is used up, so a real leave and join later still sound. */
static int MpRejoinQuietTake(const char* name)
{
	int i;

	if (gMpRejoinQuiet.count <= 0)
		return 0;

	if ((long)(MpNowMs() - gMpRejoinQuiet.untilMs) >= 0)
	{
		gMpRejoinQuiet.count = 0;
		return 0;
	}

	for (i = 0; i < gMpRejoinQuiet.count; i++)
	{
		if (strcmp(gMpRejoinQuiet.name[i], name) != 0)
			continue;

		gMpRejoinQuiet.count--;
		if (i != gMpRejoinQuiet.count)
			memcpy(gMpRejoinQuiet.name[i], gMpRejoinQuiet.name[gMpRejoinQuiet.count], MP_NAME_MAX);
		return 1;
	}

	return 0;
}

static int MpJoinLeaveQuiet(void)
{
	return !gMp.config.joinLeaveSounds || !gMp.running || gMp.leaving ||
		gMpRejoin.active || gMpRejoin.settling;
}

void MpJoinLeaveNote(int joined, const char* name)
{
	if (name == NULL)
		name = "";

	if (joined)
	{
		/* used up even when quiet for another reason: it was this return */
		int back = MpRejoinQuietTake(name);

		if (MpJoinLeaveQuiet())
			return;

		if (back)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] '%s' is back after the city change - no join sound\n", name);
			return;
		}

		if (jer_sound_menu_play(JER_MENU_SFX_ACCEPT) < 0 && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] join sound for '%s' not played (menu samples not resident or no voice)\n", name);
		return;
	}

	if (MpJoinLeaveQuiet())
		return;

	gMpLeaveSoundDueMs = MpNowMs() + MP_JOINLEAVE_LEAVE_DELAY_MS;
	if (gMpLeaveSoundDueMs == 0)
		gMpLeaveSoundDueMs = 1;
}

static int MpLeftLinesQuiet(void)
{
	return gMp.leaving || gMpRejoin.active || gMpRejoin.settling;
}

/* One "<name> left" per leaver. A dedicated server's client prints none: the
 * server's announce plugin says it in chat (and stays quiet for a city change's
 * rejoin kick) -- the second copy was the duplicate. A game host and its clients
 * print it after MP_JOINLEAVE_LEAVE_DELAY_MS, unless a rejoin kick lands first. */
void MpJoinLeaveLeftLine(const char* name)
{
	int k, hostRow = 0;

	if (name == NULL || name[0] == '\0')
		return;

	if (gMp.role == MP_ROLE_CLIENT)
	{
		for (k = 0; k < MP_MAX_PLAYERS; k++)
			if (MpGetPlayer(k) != NULL && MpGetPlayer(k)->isHost)
				hostRow = 1;

		if (!hostRow)
			return;	/* a dedicated server announces it */
	}

	if (!gMp.running)
	{
		MpConsoleLine("%s left", name);
		return;
	}

	if (MpLeftLinesQuiet() || gMpLeftLines.count >= MP_MAX_PLAYERS)
		return;

	snprintf(gMpLeftLines.name[gMpLeftLines.count], MP_NAME_MAX, "%s", name);
	gMpLeftLines.dueMs[gMpLeftLines.count] = MpNowMs() + MP_JOINLEAVE_LEAVE_DELAY_MS;
	gMpLeftLines.count++;
}

static void MpLeftLinesTick(void)
{
	int i = 0;

	if (gMpLeftLines.count == 0)
		return;

	if (MpLeftLinesQuiet())
	{
		gMpLeftLines.count = 0;
		return;
	}

	while (i < gMpLeftLines.count)
	{
		if ((long)(MpNowMs() - gMpLeftLines.dueMs[i]) < 0)
		{
			i++;
			continue;
		}

		MpConsoleLine("%s left", gMpLeftLines.name[i]);
		gMpLeftLines.count--;
		if (i != gMpLeftLines.count)
		{
			memcpy(gMpLeftLines.name[i], gMpLeftLines.name[gMpLeftLines.count], MP_NAME_MAX);
			gMpLeftLines.dueMs[i] = gMpLeftLines.dueMs[gMpLeftLines.count];
		}
	}
}

void MpJoinLeaveTick(void)
{
	MpLeftLinesTick();

	if (gMpLeaveSoundDueMs == 0)
		return;

	if (MpJoinLeaveQuiet())
	{
		gMpLeaveSoundDueMs = 0;
		return;
	}

	if ((long)(MpNowMs() - gMpLeaveSoundDueMs) < 0)
		return;

	gMpLeaveSoundDueMs = 0;

	if (jer_sound_menu_play(JER_MENU_SFX_BACK) < 0 && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] leave sound not played (menu samples not resident or no voice)\n");
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
	MpRolePicksForget("the city-change rejoin was cancelled");
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

	MpRejoinQuietArm();	/* the players coming back after this change stay quiet */

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

		/* A role-car mode: the picks were kept across the switch (ROLE CARS), so the
		 * car is the one of this player's role, not whatever was on the road. */
		if (MpModeRoleSlotCount(gMp.gamemode) > 0)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] rejoin: role cars kept - into this round's role car\n");

			MpRoleCarApply();
			MpRejoinEnd();
			return;
		}

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
		MpRolePicksForget("the city-change rejoin gave up");
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
void MpHandleKick(int connIndex, const unsigned char* p, int len)
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

void MpHandleWelcome(const unsigned char* p, int len)
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
	MpModeNoteAdopted(gMp.gamemode, "the welcome");
	gMp.city = w.city;
	gMp.timeOfDay = w.timeOfDay;
	gMp.weather = w.weather;
	gMp.seed = w.seed;

	/* THE JOIN IS WHERE THE CORE ORDER IS APPLIED. A client's own modlist may put
	 * optional modules first (or have a core one off); a session needs carhacks,
	 * pedhacks and mp first, in that order. bake is not part of that. Normalise
	 * the file now - the modules already
	 * loaded cannot be reordered in this process, so the next boot is what uses it,
	 * and that is said plainly rather than implied. */
	{
		int moved = jer_manager_normalize_core(jer_root_dir());

		if (gMpCtx != NULL)
		{
			if (moved > 0)
				gMpCtx->jer_log(gMpCtx, "[mp] modlist: core order applied (carhacks, pedhacks, mp first) - restart to use it\n");
			else if (moved < 0)
				gMpCtx->jer_log(gMpCtx, "[mp] modlist: could not write the core order (read-only install?)\n");
		}
	}

	/* Enforce the host's map. Which multiplayer map (arena 0/1), and whether it is
	 * a multiplayer map at all, are LOCAL boot flags -- -mp and -level set them --
	 * so two machines could quietly load different maps and then disagree about
	 * everything standing on them. The host is the authority: its session says
	 * which shape the level is and the client loads that, whatever its own
	 * arguments said. */
	gBootMpLevel = w.mpLevel ? 1 : 0;
	gBootMpArena = (int)(w.arena & MP_SESSION_ARENA_MASK);

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

	/* the session is already up: adopt the host's config and launch so we
	 * load the same level (the LAN browser lists sessions that have started).
	 * "Session up" is not "round LIVE": in a pre-game mode a join during
	 * WAITING / COUNTDOWN / ENDED still goes through car selection and is
	 * counted in the wait. Mid-round placement is armed by MpSessionAdoptLiveJoin
	 * once a JPMK snapshot shows LIVE. Take a Ride has no pre-game, so a join
	 * into a running session is mid-round as before. */
	if (w.running)
	{
		gMp.running = 1;
		gMp.subGame = w.subGame;

		if (MpModeHasPreGame(gMp.gamemode))
		{
			gMp.liveJoin = 0;
			gMpSawPreLive = 0;
			MpSessionAdoptLiveJoin();
			if (gMpCtx != NULL && !gMp.liveJoin)
				gMpCtx->jer_log(gMpCtx,
					"[mp] joined a session already up - pick a car; mid-round placement waits until GO\n");
		}
		else
		{
			/* WE JOINED A MATCH ALREADY IN PROGRESS. That is a different placement
			 * problem from both machines launching together: the level places our car
			 * on its own start, which is where the host came in, and the old
			 * client-side gather would then drop us beside the first peer it heard
			 * from. Mark the join so the gather is skipped and MpPlaceSelfTick owns
			 * the spawn. Cleared by MpSessionReset. */
			gMp.liveJoin = 1;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] live join: not gathering beside a peer - our car will be placed on a clear road\n");

			gMpPlaceSelfTry = 0;
			gMpPlaceSelfFrames = MP_PLACE_SELF_SETTLE;
		}

		/* ...so a peer the pre-welcome roster named no car for is ON FOOT, not in a
		 * lobby: the same rule MpHandleRoster applies once running. Without it the
		 * net spawn seats them in a phantom car at the level start (A1). */
		MpMarkRosterNoCarOnFoot((int)w.playerId);

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
			gMpCtx->jer_log(gMpCtx, "[mp] joined a session already up - pick a car\n");

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

void MpHandleReject(const unsigned char* p, int len)
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

	/* ...and SAY why: a refused join used to drop back to the menu without a word. */
	if (r.reason == MP_REJECT_PASSWORD)
		jer_error("%s - enter it under Join Game > Manual Connect > Password", r.text);
	else
		jer_error("Join refused: %s", r.text[0] ? r.text : "the server said no");
}

/* The addon net bridge: deliver an inbound MP_CHANNEL to modules (and, on the
 * host, relay a client's payload to the other clients). */
void MpHandleChannel(int connIndex, const unsigned char* p, int len)
{
	MP_CHANNEL h;
	int peer;

	if (len < (int)sizeof(MP_CHANNEL))
		return;

	memcpy(&h, p, sizeof(h));
	h.name[sizeof(h.name) - 1] = '\0';	/* wire string: never trust it terminated */

	if (len < (int)(sizeof(MP_CHANNEL) + (size_t)h.len))
		return;

	/* WHO SENT IT. On the host the connection is the authority: a client cannot claim
	 * another player's id. On a CLIENT every inbound channel frame arrived over the
	 * one link to the host/server, so the connection says nothing about the original
	 * sender - the relayed header's fromPlayer is the only copy, and dropping it made
	 * every peer's addon message look like player 0's. Carhacks then rejected its own
	 * peers' car picks (peer < 1) on every dedicated-server client. */
	peer = MpIsHost() ? MpConnPlayerId(connIndex) : (int)h.fromPlayer;

	if (peer < 0 || peer >= MP_MAX_PLAYERS)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] channel: dropped '%s' (%d bytes) with invalid sender id %d\n",
				h.name, (int)h.len, peer);

		return;
	}

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
	{
		/* relay exactly the message -- not whatever the sender appended after it --
		 * and STAMP the sender the connection proved. A relayed frame is re-received
		 * by clients through the host, where fromPlayer is the only record of who
		 * sent it; trusting the sender's own byte there would also let one client
		 * impersonate another in the addon bridge. */
		unsigned char relay[sizeof(MP_CHANNEL) + MP_CHANNEL_MAX_PAYLOAD];
		MP_CHANNEL out = h;
		int total = (int)(sizeof(MP_CHANNEL) + (size_t)h.len);

		out.fromPlayer = (uint8_t)peer;

		memcpy(relay, &out, sizeof(out));

		if (h.len > 0)
			memcpy(relay + sizeof(out), p + sizeof(MP_CHANNEL), (size_t)h.len);

		MpHostBroadcast(MP_TAG_CHANNEL, h.reliable ? MP_FLAG_RELIABLE : 0, relay, total);
	}

	MpBridgeDeliver(h.name, peer, p + sizeof(MP_CHANNEL), (int)h.len);
}

void MpHandleStart(const unsigned char* p, int len)
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

	/* A dedicated server (its roster names no host row) that welcomed us as a
	 * LOBBY ("live": false) starts a level whose players may already be on foot:
	 * mark them as the live-join welcome does. Not for a game host's lobby: nobody
	 * is on foot there, yet its roster says 0xff for every player (no level yet). */
	if (!gMp.running)
	{
		int k, hostRow = 0;

		for (k = 0; k < MP_MAX_PLAYERS; k++)
			if (MpGetPlayer(k) != NULL && MpGetPlayer(k)->isHost)
				hostRow = 1;

		if (!hostRow)
			MpMarkRosterNoCarOnFoot(gMp.localPlayerId);
	}

	gMp.gamemode = st.session.gamemode;
	MpModeNoteAdopted(gMp.gamemode, "the start");
	gMp.city = st.session.city;
	gMp.timeOfDay = st.session.timeOfDay;
	gMp.weather = st.session.weather;
	gMp.seed = st.session.seed;
	gMp.running = 1;

	/* Adopt the host's map SHAPE before the launch below, or this machine loads
	 * whatever its own -mp said - the exact drift the host-as-authority rule
	 * exists to prevent. MpLaunchLocal derives gSubGameNumber from these. */
	gBootMpLevel = st.session.spare & 1;
	gBootMpArena = (st.session.spare >> MP_SESSION_ARENA_SHIFT) & MP_SESSION_ARENA_MASK;

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

void MpHeartbeatTick(void)
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
