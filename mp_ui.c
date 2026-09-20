/*
 * mp_ui.c -- Multiplayer frontend menus, registered as REAL frontend screens
 * via jer_frontend.h (native buttons + navigation, no overlay).
 *
 * Menu tree:
 *   mp.root     Host Game | Join Game | Options | Local Split-Screen | Back
 *   mp.host     Take a Ride | Back
 *   mp.hostset  City / Time / Weather / Enforce Mods / Start Session / Back
 *   mp.join     <LAN servers...> / Manual IP / Back
 *   mp.lobby    <players...> / Start Match (host) or Leave / Back
 *   mp.options  Change Name / Enforce Mods / Port / Back
 *   mp.name     10 character slots / Done / Back
 *
 * The engine renders and navigates these; the callbacks here drive the
 * module (MpBeginHost / MpBeginJoin / discovery / config).
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_frontend.h"
#include "mp.h"

/* The engine's text primitives (declared in Game/C/pres.h, which pulls in the
 * full PSX type set -- declare just what the overlay needs instead). */
extern void SetTextColour(unsigned char Red, unsigned char Green, unsigned char Blue);
extern int  PrintString(char* string, int x, int y);

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>

enum
{
	/* NOTE: these MUST match the jer_frontend_register_menu() order in
	 * MpUiInit -- JerFrontendMenuScreen reports the registered index, which
	 * is what jer_frontend_current_menu() returns. */
	M_ROOT = 0,
	M_LAN,
	M_HOST,
	M_HOSTSET,
	M_JOIN,
	M_LOBBY,
	M_OPTIONS,
	M_NAME,
	M_MANUAL
};

static const char* const kCityNames[] = { "Chicago", "Havana", "Las Vegas", "Rio" };
static const char* const kTimeNames[] = { "Dawn", "Day", "Dusk", "Night" };
static const char* const kWeatherNames[] = { "Sunny", "Rain", "Wet" };
static const char* const kModCheckNames[] = { "Off", "By ID", "By ID+Ver" };

/* host settings + editor state */
static int gCity, gTimeOfDay, gWeather;
static char gNameEdit[11];
static int  gNameInit;
/* The manual address is edited one octet at a time in its own submenu: the
 * join row has a single adjust axis, which can never reach past the last
 * octet. gManualIp is only ever a rendering of gManualOct. */
static int  gManualOct[4] = { 127, 0, 0, 1 };
static int  gManualSeed;		/* seeded from a discovered host yet? */
static char gManualIp[32] = "127.0.0.1";
static int gManualPort;

/* LAN browser (Join Game) state -- declared up here because the Join action
 * below needs it, while the menu arrays live further down. */
static int  gJoinRev = -1;		/* discovery revision the browser was built from */
static int  gJoinBrowsing;		/* we opened the discovery socket for the browser */
static int  gJoinScanning;		/* the browser is still in its first scan window */
static unsigned long gJoinScanStart;	/* when the browse began (0 = not browsing) */

/* how long "(searching LAN...)" is shown before admitting nothing was found:
 * one beacon interval plus slack for the sender's timer */
#define MP_JOIN_SCAN_MS		(MP_BEACON_INTERVAL_MS + 500)

/* ------------------------------------------------------------------ */
/* Labels / adjusters                                                  */
/* ------------------------------------------------------------------ */
static void LblCity(void* ud, char* o, int n)    { (void)ud; snprintf(o, n, "City:  < %s >", kCityNames[gCity & 3]); }
static void LblTime(void* ud, char* o, int n)    { (void)ud; snprintf(o, n, "Time:  < %s >", kTimeNames[gTimeOfDay & 3]); }
static void LblWeather(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Weather: < %s >", kWeatherNames[gWeather % 3]); }
static void LblEnforce(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Enforce Mods: < %s >", kModCheckNames[gMp.config.modCheck % 3]); }
static void LblStrict(void* ud, char* o, int n)  { (void)ud; snprintf(o, n, "Strict Version: < %s >", gMp.config.strictVersion ? "On" : "Off"); }
static void LblPort(void* ud, char* o, int n)    { (void)ud; snprintf(o, n, "Port: %d", gMp.config.port); }
static void LblManualIp(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Manual IP: %s ...", gManualIp); }
static void LblManualOct(void* ud, char* o, int n) { snprintf(o, n, "Part %d: < %d >", (int)(intptr_t)ud + 1, gManualOct[(int)(intptr_t)ud]); }
static void LblManualGo(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Connect to %s", gManualIp); }

static int AdjCity(void* ud, int dir)    { (void)ud; gCity = (gCity + dir + 4) & 3; return 1; }
static int AdjTime(void* ud, int dir)    { (void)ud; gTimeOfDay = (gTimeOfDay + dir + 4) & 3; return 1; }
static int AdjWeather(void* ud, int dir) { (void)ud; gWeather = (gWeather + dir + 3) % 3; return 1; }
static int AdjEnforce(void* ud, int dir) { (void)ud; gMp.config.modCheck = (gMp.config.modCheck + dir + 3) % 3; MpConfigSave(); return 1; }

/* Host-side policy: also require an identical build hash. Off by default --
 * the hash tracks git describe, so requiring it shuts out anyone who is on a
 * different commit. */
static int AdjStrict(void* ud, int dir) { (void)ud; gMp.config.strictVersion = (gMp.config.strictVersion + dir + 2) & 1; MpConfigSave(); return 1; }

static int AdjPort(void* ud, int dir)
{
	(void)ud;
	gMp.config.port += dir;
	if (gMp.config.port < 1024) gMp.config.port = 65535;
	if (gMp.config.port > 65535) gMp.config.port = 1024;
	MpConfigSave();
	return 1;
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */
static int ActJoinEnter(void* ud)
{
	(void)ud;

	/* (re)start the browse and let the browser say "searching" for the first
	 * beacon interval, so an empty list is not reported as "no games" */
	gJoinScanStart = MpNowMs();
	gJoinScanning = 1;
	gJoinBrowsing = 1;

	MpDiscoveryStart(0);	/* begin browsing; engine opens the submenu */
	return 0;
}

static int ActHostStart(void* ud)
{
	(void)ud;

	if (MpBeginHost())
	{
		/* set the lobby config AFTER MpBeginHost (which resets state) */
		gMp.gamemode = MP_GAMEMODE_TAKEADRIDE;
		gMp.city = gCity;
		gMp.timeOfDay = gTimeOfDay;
		gMp.weather = gWeather;
		jer_frontend_open(M_LOBBY);
	}

	return 1;
}

static int ActSplitScreen(void* ud)
{
	(void)ud;
	jer_frontend_goto(6);	/* the stock multiplayer gamemode screen */
	return 1;
}

/* Enter the LAN host flow. Bring the socket up NOW so the module is in HOST
 * role for the whole stock level-select chain -- the stock main menu's
 * "Multiplayer" button (var 0x301 -> SetVariable code 3) sets NumPlayers = 2
 * for its split-screen path, and the module pins that back to 1 while a LAN
 * session is up. Returns 0 so the engine also opens the gamemode submenu. */
static int ActHostGame(void* ud)
{
	(void)ud;

	/* Start from a clean session every time: pressing Host Game again after
	 * backing out of the flow used to keep the previous listener and session
	 * state, so the second attempt behaved oddly (and could lose the peer). */
	if (gMp.role != MP_ROLE_NONE)
		MpLeaveSession();

	MpBeginHost();

	return 0;
}

/* After the host confirms a CITY in the stock city screen: the large main
 * map, or that city's dedicated multiplayer level. */
static int ActModeSP(void* ud)
{
	(void)ud;

	MpSetSubGame(0);		/* the whole city, no sub-level */
	jer_frontend_goto(3);		/* continue the stock flow: time of day */
	return 1;
}

static int ActModeMP(void* ud)
{
	(void)ud;

	/* 35..38 are the per-city multiplayer level lists (35 = Chicago,
	 * 36 = Havana, 37 = Las Vegas, 38 = Rio); their buttons set
	 * gSubGameNumber (SetVariable code 8). */
	jer_frontend_goto(35 + (MpGetGameLevel() & 3));
	return 1;
}

/* Open the Single Player / Multiplayer question (the engine calls this from
 * the city-confirm hook). */
void MpUiOpenModeMenu(void)
{
	jer_frontend_open(M_HOSTSET);
}

/* Open the stock CAR SELECT (screen 14) so a joining player picks their own
 * vehicle; its "Select" is the START the module claims. */
void MpUiOpenCarSelect(void)
{
	jer_frontend_goto(14);
}

/* Jump into the STOCK level-select chain (screen indices come from
 * DATA/SCRS.BIN): 1 = the city screen, then the SP/MP question, then 3 =
 * time of day and 14 = car select whose "Select" is the START. The host
 * picks the level exactly like the normal game. */
static int ActTakeARide(void* ud)
{
	(void)ud;

	if (gMp.role == MP_ROLE_NONE)
		MpBeginHost();		/* in case the gamemode menu was reached directly */

	jer_frontend_goto(1);		/* the stock city screen */
	return 1;
}

static int ActJoinServer(void* ud)
{
	int idx = (int)(intptr_t)ud;
	MP_SERVER* s = MpDiscoveryGet(idx);

	/* Say exactly what we are joining. The browser is the one path that can
	 * silently target a stale entry, or our own address on a machine running
	 * both sides -- and the only symptom is "connection lost", which says
	 * nothing. With this in the log the address is on the record. */
	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] joining discovered %s %s:%d (inProgress=%d, %d/%d)\n",
			s != NULL ? s->hostName : "?", s != NULL ? s->ip : "?", s != NULL ? s->port : -1,
			s != NULL ? s->inProgress : -1, s != NULL ? s->players : -1,
			s != NULL ? s->maxPlayers : -1);

	/* asynchronous: the lobby shows "Connecting to <addr>..." while the
	 * socket connects, instead of the menu freezing for up to 5 s */
	if (s != NULL && MpBeginJoinAsync(s->ip, s->port))
		jer_frontend_open(M_LOBBY);

	return 1;
}

static void MpManualSync(void)
{
	snprintf(gManualIp, sizeof(gManualIp), "%d.%d.%d.%d",
		gManualOct[0], gManualOct[1], gManualOct[2], gManualOct[3]);
}

/* Seed the address from the first LAN game we can see, so the usual case is
 * a couple of taps on the last octet rather than typing a whole subnet. */
static void MpManualSeed(void)
{
	MP_SERVER* s = (MpDiscoveryCount() > 0) ? MpDiscoveryGet(0) : NULL;

	if (!gManualSeed && s != NULL &&
		sscanf(s->ip, "%d.%d.%d.%d",
			&gManualOct[0], &gManualOct[1], &gManualOct[2], &gManualOct[3]) == 4)
	{
		gManualSeed = 1;
	}

	MpManualSync();
}

/* The submenu's Connect row. The button sticks to the one we press, so the
 * accent stays on the last octet it was landed on. */
static int ActManualConnect(void* ud)
{
	(void)ud;

	/* gManualPort, not our own configured port: the manual entry has to be able
	 * to reach a host listening somewhere else. */
	if (gManualPort <= 0)
		gManualPort = gMp.config.port;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] joining manual %s:%d\n", gManualIp, gManualPort);

	if (MpBeginJoinAsync(gManualIp, gManualPort))
		jer_frontend_open(M_LOBBY);

	return 1;
}

static int AdjManualOct(void* ud, int dir)
{
	int i = (int)(intptr_t)ud;
	int v;

	if (i < 0 || i > 3)
		return 1;

	/* holding the button ramps, so wrap around instead of sticking at the end */
	v = gManualOct[i] + dir;

	if (v < 0)
		v = 255;
	if (v > 255)
		v = 0;

	gManualOct[i] = v;
	gManualSeed = 1;	/* the player has taken over */
	MpManualSync();

	return 1;
}

static int ActLobbyLeave(void* ud)
{
	(void)ud;
	MpLeaveSession();
	jer_frontend_goto(0);	/* back to the main menu */
	return 1;
}

static int ActLobbyStart(void* ud)
{
	(void)ud;
	MpStartMatch();
	return 1;
}

/* name editor: one adjustable slot per character */
static int ActNameDone(void* ud)
{
	int n;
	(void)ud;

	n = (int)strlen(gNameEdit);
	while (n > 0 && gNameEdit[n - 1] == ' ')
		gNameEdit[--n] = '\0';

	if (gNameEdit[0] == '\0')
		snprintf(gNameEdit, sizeof(gNameEdit), "%s", "Player");

	snprintf(gMp.config.playerName, sizeof(gMp.config.playerName), "%s", gNameEdit);
	gMp.config.firstNameSet = 1;
	MpConfigSave();
	jer_frontend_open(M_OPTIONS);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Static menus                                                        */
/* ------------------------------------------------------------------ */
static const JER_FE_ITEM kRootItems[] =
{
	{ "LAN",          NULL, NULL, NULL,           NULL, M_LAN, 0 },
	{ "Split-Screen", NULL, NULL, ActSplitScreen, NULL, -1,    0 },
	{ "Back",         NULL, NULL, NULL,           NULL, -1,    1 },
};
static const JER_FE_MENU kRootMenu = { "mp.root", kRootItems, 3, NULL, NULL };

static const JER_FE_ITEM kLanItems[] =
{
	{ "Host Game", NULL, NULL, ActHostGame, NULL, M_HOST,    0 },
	{ "Join Game", NULL, NULL, ActJoinEnter, NULL, M_JOIN,    0 },
	{ "Options",   NULL, NULL, NULL,         NULL, M_OPTIONS, 0 },
	{ "Back",      NULL, NULL, NULL,         NULL, -1,        1 },
};
static const JER_FE_MENU kLanMenu = { "mp.lan", kLanItems, 4, NULL, NULL };

static const JER_FE_ITEM kHostItems[] =
{
	{ "Take a Ride", NULL, NULL, ActTakeARide, NULL, -1, 0 },
	{ "Back",        NULL, NULL, NULL,         NULL, -1, 1 },
};
static const JER_FE_MENU kHostMenu = { "mp.host", kHostItems, 2, NULL, NULL };

static const JER_FE_ITEM kHostSetItems[] =
{
	{ "Single Player", NULL, NULL, ActModeSP, NULL, -1, 0 },
	{ "Multiplayer",   NULL, NULL, ActModeMP, NULL, -1, 0 },
	{ "Back",          NULL, NULL, NULL,      NULL, -1, 1 },
};
static const JER_FE_MENU kHostSetMenu = { "mp.mode", kHostSetItems, 3, NULL, NULL };

static const JER_FE_ITEM kOptionsItems[] =
{
	{ "Change Name",    NULL, NULL, NULL,       NULL,       M_NAME, 0 },
	{ NULL, LblEnforce, NULL, NULL, AdjEnforce, -1,       0 },
	{ NULL, LblStrict,  NULL, NULL, AdjStrict,  -1,       0 },
	{ NULL, LblPort,    NULL, NULL, AdjPort,    -1,       0 },
	{ "Back",           NULL, NULL, NULL,       NULL,      -1,     1 },
};
static const JER_FE_MENU kOptionsMenu = { "mp.options", kOptionsItems, 5, NULL, NULL };

/* ------------------------------------------------------------------ */
/* Dynamic menus (join / lobby / name)                                 */
/* ------------------------------------------------------------------ */
static JER_FE_ITEM gJoinItems[JER_FE_MAX_ITEMS];
static JER_FE_MENU gJoinMenu = { "mp.join", gJoinItems, 0, NULL, NULL };
static char gJoinLabel[JER_FE_MAX_ITEMS][64];

static JER_FE_ITEM gLobbyItems[JER_FE_MAX_ITEMS];
static JER_FE_MENU gLobbyMenu = { "mp.lobby", gLobbyItems, 0, NULL, NULL };
static int gLobbyBuiltCount = -1;	/* player count the lobby was last built with */
static int gLobbyJoinState = -1;	/* join state the lobby was last built with */
static char gLobbyLabel[MP_MAX_PLAYERS][40];
static char gLobbyStatus[64];		/* "Connecting to <addr>..." / "(could not connect)" */

static JER_FE_ITEM gNameItems[JER_FE_MAX_ITEMS];
static JER_FE_MENU gNameMenu = { "mp.name", gNameItems, 0, NULL, NULL };

/* manual LAN address: one row per octet, then Connect */
static JER_FE_ITEM gManualItems[6];
static JER_FE_MENU gManualMenu = { "mp.manual", gManualItems, 0, NULL, NULL };
static char gNameSlotLabel[10][24];
static const char kAlpha[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";

static void JoinOnEnter(void* ud);
static void LobbyOnEnter(void* ud);
static void NameOnEnter(void* ud);
static void ManualOnEnter(void* ud);
static int  AdjNameChar(void* ud, int dir);

static void JoinOnEnter(void* ud)
{
	int ns, i, k = 0, scanning;
	(void)ud;

	ns = MpDiscoveryCount();

	/* one beacon interval is the shortest wait before believing the LAN is
	 * really empty -- a beacon already in flight still counts */
	scanning = (gJoinScanStart != 0) && (MpNowMs() - gJoinScanStart) < MP_JOIN_SCAN_MS;
	gJoinScanning = scanning;

	for (i = 0; i < ns && k < JER_FE_MAX_ITEMS - 2; i++)
	{
		MP_SERVER* s = MpDiscoveryGet(i);

		if (s == NULL)
			continue;

		/* LOBBY vs LIVE comes from the beacon's inProgress flag, which the
		 * host sets from gMp.running. */
		snprintf(gJoinLabel[k], sizeof(gJoinLabel[k]), "%s  %s:%d  %d/%d  %s",
			s->hostName, s->ip, s->port, s->players, s->maxPlayers,
			s->inProgress ? "LIVE" : "LOBBY");

		gJoinItems[k].label = gJoinLabel[k];
		gJoinItems[k].get_label = NULL;
		gJoinItems[k].userdata = (void*)(intptr_t)i;
		gJoinItems[k].on_activate = ActJoinServer;
		gJoinItems[k].on_adjust = NULL;
		gJoinItems[k].submenu = -1;
		gJoinItems[k].is_back = 0;
		k++;
	}

	if (ns == 0 && k < JER_FE_MAX_ITEMS - 2)
	{
		gJoinItems[k].label = scanning ? "(searching LAN...)" : "(no games found)";
		gJoinItems[k].get_label = NULL;
		gJoinItems[k].userdata = NULL;
		gJoinItems[k].on_activate = NULL;
		gJoinItems[k].on_adjust = NULL;
		gJoinItems[k].submenu = -1;
		gJoinItems[k].is_back = 0;
		k++;
	}

	/* manual IP -- a submenu, because a real address needs all four octets and
	 * this row has one adjust axis */
	gJoinItems[k].label = NULL;
	gJoinItems[k].get_label = LblManualIp;
	gJoinItems[k].userdata = NULL;
	gJoinItems[k].on_activate = NULL;
	gJoinItems[k].on_adjust = NULL;
	gJoinItems[k].submenu = M_MANUAL;
	gJoinItems[k].is_back = 0;
	k++;

	/* back */
	gJoinItems[k].label = "Back";
	gJoinItems[k].get_label = NULL;
	gJoinItems[k].userdata = NULL;
	gJoinItems[k].on_activate = NULL;
	gJoinItems[k].on_adjust = NULL;
	gJoinItems[k].submenu = -1;
	gJoinItems[k].is_back = 1;
	k++;

	gJoinMenu.item_count = k;

	/* remember what this list was built from, so MpUiTick can rebuild it
	 * only when the server set actually changed */
	gJoinRev = MpDiscoveryRevision();
}

static void LobbyOnEnter(void* ud)
{
	int i, k = 0, joinState;
	(void)ud;

	/* While a join is in flight (or has just failed) say so on the first row,
	 * so the player is not staring at an empty lobby wondering. Hosts never
	 * have an attempt in flight, and READY means the normal lobby. */
	joinState = MpJoinState();
	gLobbyJoinState = joinState;

	if (!MpIsHost() && joinState != MP_JOIN_IDLE && joinState != MP_JOIN_READY)
	{
		if (joinState == MP_JOIN_CONNECTING)
			snprintf(gLobbyStatus, sizeof(gLobbyStatus), "Connecting to %s:%d ...",
				MpJoinTarget(), MpJoinTargetPort());
		else
			snprintf(gLobbyStatus, sizeof(gLobbyStatus), "(could not connect)");

		gLobbyItems[k].label = gLobbyStatus;
		gLobbyItems[k].get_label = NULL;
		gLobbyItems[k].userdata = NULL;
		gLobbyItems[k].on_activate = NULL;
		gLobbyItems[k].on_adjust = NULL;
		gLobbyItems[k].submenu = -1;
		gLobbyItems[k].is_back = 0;
		k++;
	}

	for (i = 0; i < MP_MAX_PLAYERS && k < JER_FE_MAX_ITEMS - 3; i++)
	{
		if (!gMp.players[i].active)
			continue;

		snprintf(gLobbyLabel[k], sizeof(gLobbyLabel[k]), "%s %s",
			gMp.players[i].isLocal ? ">" : " ", gMp.players[i].name);

		gLobbyItems[k].label = gLobbyLabel[k];
		gLobbyItems[k].get_label = NULL;
		gLobbyItems[k].userdata = NULL;
		gLobbyItems[k].on_activate = NULL;
		gLobbyItems[k].on_adjust = NULL;
		gLobbyItems[k].submenu = -1;
		gLobbyItems[k].is_back = 0;
		k++;
	}

	if (MpIsHost())
	{
		gLobbyItems[k].label = "Start Match";
		gLobbyItems[k].get_label = NULL;
		gLobbyItems[k].userdata = NULL;
		gLobbyItems[k].on_activate = ActLobbyStart;
		gLobbyItems[k].on_adjust = NULL;
		gLobbyItems[k].submenu = -1;
		gLobbyItems[k].is_back = 0;
		k++;
	}

	gLobbyItems[k].label = "Leave";
	gLobbyItems[k].get_label = NULL;
	gLobbyItems[k].userdata = NULL;
	gLobbyItems[k].on_activate = ActLobbyLeave;
	gLobbyItems[k].on_adjust = NULL;
	gLobbyItems[k].submenu = -1;
	gLobbyItems[k].is_back = 0;
	k++;

	gLobbyMenu.item_count = k;
	gLobbyBuiltCount = gMp.playerCount;	/* rebuild only when this changes */
}

static void NameOnEnter(void* ud)
{
	int i, k = 0;
	(void)ud;

	if (!gNameInit)
	{
		int n = (int)strlen(gMp.config.playerName);
		int j;

		memset(gNameEdit, ' ', 10);
		gNameEdit[10] = '\0';

		for (j = 0; j < 10 && j < n; j++)
			gNameEdit[j] = gMp.config.playerName[j];

		gNameInit = 1;
	}

	for (i = 0; i < 10 && k < JER_FE_MAX_ITEMS - 2; i++)
	{
		snprintf(gNameSlotLabel[k], sizeof(gNameSlotLabel[k]), "%d: < %c >", i + 1, gNameEdit[i]);

		gNameItems[k].label = gNameSlotLabel[k];
		gNameItems[k].get_label = NULL;
		gNameItems[k].userdata = (void*)(intptr_t)i;
		gNameItems[k].on_activate = NULL;
		gNameItems[k].on_adjust = AdjNameChar;
		gNameItems[k].submenu = -1;
		gNameItems[k].is_back = 0;
		k++;
	}

	gNameItems[k].label = "Done";
	gNameItems[k].get_label = NULL;
	gNameItems[k].userdata = NULL;
	gNameItems[k].on_activate = ActNameDone;
	gNameItems[k].on_adjust = NULL;
	gNameItems[k].submenu = -1;
	gNameItems[k].is_back = 0;
	k++;

	gNameItems[k].label = "Back";
	gNameItems[k].get_label = NULL;
	gNameItems[k].userdata = NULL;
	gNameItems[k].on_activate = NULL;
	gNameItems[k].on_adjust = NULL;
	gNameItems[k].submenu = -1;
	gNameItems[k].is_back = 1;
	k++;

	gNameMenu.item_count = k;
}

static int AdjNameChar(void* ud, int dir)
{
	int idx = (int)(intptr_t)ud;
	const char* p;
	int n = (int)strlen(kAlpha);
	int i;

	if (idx < 0 || idx > 9)
		return 1;

	i = 0;
	p = strchr(kAlpha, gNameEdit[idx]);
	if (p != NULL)
		i = (int)(p - kAlpha);

	i = (i + dir + n) % n;
	gNameEdit[idx] = kAlpha[i];
	return 1;
}

/* wire the dynamic on_enter callbacks (the menus are non-const statics) */
/* Four octet rows, Connect, Back. Built on entry so the label picks up the
 * seeded address, which is why the address defaults to the subnet the first
 * discovered game is on. */
static void ManualOnEnter(void* ud)
{
	int i, k = 0;

	(void)ud;

	MpManualSeed();

	for (i = 0; i < 4; i++)
	{
		gManualItems[k].label = NULL;
		gManualItems[k].get_label = LblManualOct;
		gManualItems[k].userdata = (void*)(intptr_t)i;
		gManualItems[k].on_activate = NULL;
		gManualItems[k].on_adjust = AdjManualOct;
		gManualItems[k].submenu = -1;
		gManualItems[k].is_back = 0;
		k++;
	}

	gManualItems[k].label = NULL;
	gManualItems[k].get_label = LblManualGo;
	gManualItems[k].userdata = NULL;
	gManualItems[k].on_activate = ActManualConnect;
	gManualItems[k].on_adjust = NULL;
	gManualItems[k].submenu = -1;
	gManualItems[k].is_back = 0;
	k++;

	gManualItems[k].label = "Back";
	gManualItems[k].get_label = NULL;
	gManualItems[k].userdata = NULL;
	gManualItems[k].on_activate = NULL;
	gManualItems[k].on_adjust = NULL;
	gManualItems[k].submenu = -1;
	gManualItems[k].is_back = 1;
	k++;

	gManualMenu.item_count = k;
}

static void WireDynamic(void)
{
	gJoinMenu.on_enter = JoinOnEnter;
	gLobbyMenu.on_enter = LobbyOnEnter;
	gNameMenu.on_enter = NameOnEnter;
	gManualMenu.on_enter = ManualOnEnter;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */
/* Per-frame hook: rebuild the live lobby menu only when the player list
 * actually changes (never from on_enter -- that rebuilt it every frame and
 * reset the cursor), and the LAN browser when the server set changes. The
 * engine keeps the cursor across a live refresh, so scrolling does not jump. */
void MpUiTick(void)
{
	if (gLobbyBuiltCount >= 0 && gMp.playerCount != gLobbyBuiltCount)
		jer_frontend_refresh();

	/* the lobby's "Connecting to ..." row clears itself the moment the join
	 * resolves -- accepted (READY) or refused/failed */
	if (jer_frontend_current_menu() == M_LOBBY && MpJoinState() != gLobbyJoinState)
		jer_frontend_refresh();

	/* The Join screen is rebuilt only by its on_enter, which the engine runs
	 * on setup/refresh alone -- without this the server list would be frozen
	 * at whatever had been found when the player opened it. */
	if (jer_frontend_current_menu() == M_JOIN)
	{
		int scanning = (gJoinScanStart != 0) &&
			(MpNowMs() - gJoinScanStart) < MP_JOIN_SCAN_MS;

		/* rows changed, or "searching" just became "no games found" */
		if (MpDiscoveryRevision() != gJoinRev || scanning != gJoinScanning)
			jer_frontend_refresh();
	}
	else if (gJoinBrowsing)
	{
		/* left the browser: release the discovery socket we opened. Never a
		 * live host's advertisement -- MpStartMatch arms that separately. */
		if (gMp.role == MP_ROLE_NONE && !gMp.running)
			MpDiscoveryStop();

		gJoinBrowsing = 0;
		gJoinScanStart = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Lower-left info overlay: who joined/left + chat scaffolding         */
/* ------------------------------------------------------------------ */
void MpNotify(const char* text)
{
	int slot;

	if (text == NULL || text[0] == '\0')
		return;

	slot = gMp.notifyNext % MP_NOTIFY_MAX;
	snprintf(gMp.notifyText[slot], MP_NOTIFY_TEXT_MAX, "%s", text);
	gMp.notifyUntil[slot] = MpNowMs() + MP_NOTIFY_MS;
	gMp.notifyNext = (slot + 1) % MP_NOTIFY_MAX;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] info: %s\n", text);
}

void MpNotifyf(const char* fmt, ...)
{
	char b[MP_NOTIFY_TEXT_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);

	MpNotify(b);
}

/* Radar/compass markers: each car placed on a ring by its bearing. Disabled
 * here in favour of the world-space labels below, but KEPT for reuse (flip
 * MP_RADAR to 1). */
#define MP_RADAR 0

#if MP_RADAR
static void MpDrawRadar(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	int mx, my, mz, mh, k;

	if (me == NULL || me->carId < 0)
		return;

	MpCarPose(me->carId, &mx, &my, &mz, &mh);

	{
		float head = (float)(mh & 0xfff) * (6.2831853f / 4096.0f);

		for (k = 0; k < MP_MAX_PLAYERS; k++)
		{
			MP_PLAYER* p = &gMp.players[k];
			float dx, dz, ang;
			int cx, cy, cz, ch, sx, sy;
			char tag[8];

			if (!p->active || p->isLocal || p->carId < 0)
				continue;

			MpCarPose(p->carId, &cx, &cy, &cz, &ch);
			dx = (float)(cx - mx);
			dz = (float)(cz - mz);
			ang = (float)atan2(dx, dz) - head;

			sx = 160 + (int)(sinf(ang) * 112.0f);
			sy = 120 - (int)(cosf(ang) * 92.0f);

			if (sx < 4) sx = 4;
			if (sx > 308) sx = 308;
			if (sy < 8) sy = 8;
			if (sy > 228) sy = 228;

			snprintf(tag, sizeof(tag), "%d", p->id);
			SetTextColour(120, 205, 255);
			PrintString(tag, sx, sy);
		}
	}
}
#endif	/* MP_RADAR */

/* The other players' numbers, drawn ABOVE their cars. Yaw-only projection
 * (the camera pitch is small enough not to matter for a label). */
static void MpDrawCarLabels(void)
{
	int camx, camy, camz, camyaw;
	int k;

	MpCameraPose(&camx, &camy, &camz, &camyaw);

	{
		float yaw = (float)(camyaw & 0xfff) * (6.2831853f / 4096.0f);
		float s = sinf(yaw), c = cosf(yaw);

		for (k = 0; k < MP_MAX_PLAYERS; k++)
		{
			MP_PLAYER* p = &gMp.players[k];
			int wx, wy, wz, wh, sx, sy;
			float dx, dy, dz, rx, rz, f;
			char tag[8];

			if (!p->active || p->isLocal || p->carId < 0)
				continue;

			MpCarPose(p->carId, &wx, &wy, &wz, &wh);

			dx = (float)(wx - camx);
			dy = (float)(wy + 320 - camy);	/* float the label over the roof */
			dz = (float)(wz - camz);

			rx = dx * c - dz * s;
			rz = dx * s + dz * c;

			if (rz < 64.0f || rz > 8000.0f)
				continue;		/* behind the camera / too far */

			f = 520.0f / rz;
			sx = 160 + (int)(rx * f);
			sy = 120 - (int)(dy * f);

			if (sx < 6 || sx > 308 || sy < 6 || sy > 232)
				continue;

			snprintf(tag, sizeof(tag), "%d", p->id);
			SetTextColour(255, 240, 120);
			PrintString(tag, sx, sy);
		}
	}
}

/* JER_EVENT_DRAW_OVERLAY: the overlay pass, every frame the world is drawn.
 * Screen space here is the 320x240 PSX buffer, so the lower-left corner is
 * around y = 178..232. */
int MpUiDrawOverlay(void* userdata, void* args)
{
	unsigned long now = MpNowMs();
	int row = 0, i;

	(void)userdata;
	(void)args;

	if (!MpIsActive())
		return JER_RESULT_CONTINUE;

	/* oldest -> newest, stacked up from the lower-left corner */
	for (i = 0; i < MP_NOTIFY_MAX; i++)
	{
		int idx = (gMp.notifyNext + i) % MP_NOTIFY_MAX;

		if (gMp.notifyUntil[idx] == 0 || now >= gMp.notifyUntil[idx])
			continue;

		SetTextColour(255, 235, 140);
		PrintString(gMp.notifyText[idx], 8, 178 + row * 10);
		row++;
	}

#if MP_RADAR
	MpDrawRadar();
#endif
	MpDrawCarLabels();

	/* chat prompt (scaffolding: the buffer is filled by a future key hook) */
	if (gMp.chatOpen)
	{
		char line[MP_NOTIFY_TEXT_MAX + 2];

		snprintf(line, sizeof(line), "%s_", gMp.chatBuf);
		SetTextColour(150, 255, 150);
		PrintString(line, 8, 232);
	}

	return JER_RESULT_CONTINUE;
}

void MpChatOpen(void)
{
	gMp.chatOpen = 1;
	gMp.chatBuf[0] = '\0';
}

void MpChatSendText(const char* text)
{
	if (text == NULL || text[0] == '\0')
	{
		gMp.chatOpen = 0;
		gMp.chatBuf[0] = '\0';
		return;
	}

	MpSendChat(text);	/* mp_session.c puts it on the wire and logs it */

	gMp.chatBuf[0] = '\0';
	gMp.chatOpen = 0;
}

void MpUiInit(void)
{
	WireDynamic();

	jer_frontend_register_menu(&kRootMenu);
	jer_frontend_register_menu(&kLanMenu);
	jer_frontend_register_menu(&kHostMenu);
	jer_frontend_register_menu(&kHostSetMenu);
	jer_frontend_register_menu(&gJoinMenu);
	jer_frontend_register_menu(&gLobbyMenu);
	jer_frontend_register_menu(&kOptionsMenu);
	jer_frontend_register_menu(&gNameMenu);
	jer_frontend_register_menu(&gManualMenu);

	jer_frontend_set_main_entry("mp.root");

	MpManualSync();

	/* sensible host-settings defaults */
	gCity = gMp.city;
	gTimeOfDay = (gMp.timeOfDay < 0) ? 1 : gMp.timeOfDay;
	gWeather = (gMp.weather < 0) ? 0 : gMp.weather;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] registered %d frontend menus (main entry mp.root)\n",
			jer_frontend_menu_count());
}
