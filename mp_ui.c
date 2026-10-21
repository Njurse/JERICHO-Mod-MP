/*
 * mp_ui.c -- Multiplayer frontend menus, registered as REAL frontend screens
 * via jer_frontend.h (native buttons + navigation, no overlay).
 *
 * Menu tree:
 *   mp.root     Online | LAN | JMP Options | Split-Screen | Back
 *   mp.lan      Host Game | Join Game | Back
 *   mp.host     Take a Ride | Back
 *   mp.mode     Single Player | Multiplayer | Back          (the take-a-ride question)
 *   mp.join     <LAN servers...> / Manual IP / Back
 *   mp.lobby    <players...> / Start Match (host) or Leave / Back
 *   mp.jmpoptions  Change Name | Enforce Mods | Port / Back
 *   mp.name     10 character slots / Done / Back
 *
 * "JMP Options" used to be an "Options" row INSIDE the LAN page. It moved up one
 * level because it is session configuration, not a LAN action: hosting, joining
 * and the options a player changes before either are siblings on the root page.
 *
 * THE MATCH'S CITY, TIME OF DAY AND WEATHER ARE NOT SET HERE. They are the host's
 * choices on the ENGINE'S OWN screens -- the city screen, then the combined Time of
 * Day / Condition screen that "Single Player" hands over to -- and MpStartMatch
 * seeds the session from them so the host and every client load the same thing.
 * mp used to carry a lobby menu (City / Time / Weather / Start Session) for these;
 * mp.mode replaced it and those rows were removed, which left the session fields
 * with no writer. They are now the WIRE's override channel only.
 *
 * The engine renders and navigates these; the callbacks here drive the
 * module (MpBeginHost / MpBeginJoin / discovery / config).
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_frontend.h"
#include "jer_console.h"		/* the status stream (join/leave, chat) */
#include "jer_screen.h"		/* jer_screen_remap_x: 3D->framebuffer x under widescreen */
#include "jer_text.h"		/* measure + draw at an x: the Online table's columns */
#include "mp.h"

/* The engine's text primitives (declared in Game/C/pres.h, which pulls in the
 * full PSX type set -- declare just what the overlay needs instead). */
extern void SetTextColour(unsigned char Red, unsigned char Green, unsigned char Blue);
extern int  PrintString(char* string, int x, int y);
extern int  PrintStringHiresScaledSpaced(char* string, int x, int y, float scale, float extra);
extern int  StringWidth(char* pString);	/* pres.h: full-size text width, PSX units */
extern int  gInFrontend;		/* glaunch.h: the engine is showing the frontend */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>

enum
{
	/* The LOGICAL order of MpUiInit's registrations. It is NOT the registered
	 * index: another module may register a menu first (carhacks does), which
	 * shifts every index below by one. Every use goes through gMenuIdx[], resolved
	 * by id after registration - see MpResolveMenus. */
	M_ROOT = 0,
	M_LAN,
	M_ONLINE,	/* the discovery-server browser (was: not a screen at all) */
	M_HOST,
	M_MODE,			/* the "mp.mode" menu (was M_HOSTSET: the registered id has
				 * said `mode` for a while, so the enum now matches it) */
	M_JOIN,
	M_LOBBY,
	M_JMP_OPTIONS,		/* was M_OPTIONS: the same page, moved out of LAN and
				 * renamed "JMP Options" (registered id mp.jmpoptions) */
	M_NAME,
	M_MANUAL,
	M_COUNT
};

/* Each mp menu's id, in the enum's order, and the index it ACTUALLY registered at
 * (-1 = not registered). */
static const char* const kMenuIds[M_COUNT] = {
	"mp.root", "mp.lan", "mp.online", "mp.host", "mp.mode", "mp.join",
	"mp.lobby", "mp.jmpoptions", "mp.name", "mp.manual"
};
static int gMenuIdx[M_COUNT];
static int gMenusResolved;	/* set once MpResolveMenus has run: an index is only
				 * meaningful after the registration pass */

/* Armed by MpUiArmModeMenu (from mp.c's JER_EVENT_FRONTEND confirm hook) and acted
 * on in MpUiTick, i.e. on a LATER frame. Opening the menu from inside the hook that
 * triggered it does not take -- the engine finishes the screen switch after the hook
 * returns -- which is exactly why the Single Player / Multiplayer step never
 * appeared. carhacks defers its own menu the same way. Counts down so the open is
 * retried while the screens settle. */
static int gModeMenuArmed;

/* Open the menu `logical`, if it registered. Inert until MpResolveMenus has run,
 * so a caller that fires before the registration pass cannot open index 0 by
 * accident (the array is zero-filled). */
static void MpMenuOpen(int logical)
{
	if (gMenusResolved && logical >= 0 && logical < M_COUNT && gMenuIdx[logical] >= 0)
		jer_frontend_open(gMenuIdx[logical]);
}

/* Is the menu `logical` the one on screen? Not a plain index comparison: the
 * engine reports -1 for "not a module menu", and an unregistered menu is never
 * on screen. */
static int MpMenuIs(int logical)
{
	return gMenusResolved && logical >= 0 && logical < M_COUNT &&
		gMenuIdx[logical] >= 0 && jer_frontend_current_menu() == gMenuIdx[logical];
}

static const char* const kModCheckNames[] = { "Off", "By ID", "By ID+Ver" };

/* editor state */
static char gNameEdit[11];
static int  gNameInit;
/* The manual join target, TYPED rather than dialled an octet at a time: a DNS name
 * or an IPv4, optionally with ":port" -- which is what makes the field usable for a
 * host on the internet, where the address is a NAME and the port is forwarded.
 *
 * gManualIp is the source of truth and holds exactly what was typed. The menu row
 * renders it and an EDIT MODE feeds it keystrokes: no frontend menu item can take
 * text (jer_frontend.h has no keyboard hook -- the same reason the name menu is a
 * row of per-character adjusters), so the field borrows PsyX's text-input slot the
 * way chat does. */
#define MP_MANUAL_MAX	100		/* what the row can show without spilling */
static char gManualIp[128] = "127.0.0.1";
static char gManualUndo[128];		/* what Escape restores */
static int  gManualEdit;		/* the field owns the keyboard right now */
static int  gManualSeed;		/* seeded from a discovered host yet? */

/* LAN browser (Join Game) state -- declared up here because the Join action
 * below needs it, while the menu arrays live further down. */
static int  gJoinRev = -1;		/* discovery revision the browser was built from */
static int  gJoinBrowsing;		/* we opened the discovery socket for the browser */
static int  gJoinScanning;		/* the browser is still in its first scan window */

/* The master's motd revision already said in this run (-1 = nothing said yet). It
 * is said once per distinct TEXT, so a browser that re-fetches every few seconds
 * does not repeat itself. */
static int  gOnlineMotdSaid = -1;
static unsigned long gJoinScanStart;	/* when the browse began (0 = not browsing) */

/* how long "(searching LAN...)" is shown before admitting nothing was found:
 * one beacon interval plus slack for the sender's timer */
#define MP_JOIN_SCAN_MS		(MP_BEACON_INTERVAL_MS + 500)

/* ------------------------------------------------------------------ */
/* Labels / adjusters                                                  */
/* ------------------------------------------------------------------ */
static void LblEnforce(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Enforce Mods: < %s >", kModCheckNames[gMp.config.modCheck % 3]); }
static void LblStrict(void* ud, char* o, int n)  { (void)ud; snprintf(o, n, "Strict Version: < %s >", gMp.config.strictVersion ? "On" : "Off"); }
static void LblPort(void* ud, char* o, int n)    { (void)ud; snprintf(o, n, "Port: %d", gMp.config.port); }
static void LblManualGo(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Connect to %s", gManualIp); }

/* The field itself. It says it is taking keystrokes and shows a caret, so "why does
 * nothing happen when I press keys" answers itself. */
static void LblManualField(void* ud, char* o, int n)
{
	(void)ud;

	snprintf(o, n, "%s %s%s", gManualEdit ? "" : "Address:",
		gManualIp, gManualEdit ? "_" : "");
}

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

/* ---- SESSION RULES (the Host Game menu) --------------------------------
 *
 * What the host picks here is applied on EVERY machine for the WHOLE match:
 * the values are packed (MP_RULES_PACK) and published to the launch
 * (MP_SESSION.spare) and to every joiner (MP_WELCOME.rules), then re-asserted
 * every frame by MpSessionRulesTick. They are the host's defaults for the NEXT
 * match, so they live in mp.ini and survive a restart -- and they are NOT read
 * mid-match, so editing them cannot change a game already in progress.
 *
 * Cops is a strict on/off in both directions: it writes CopsAllowed. The
 * engine's own default for that flag is per level, so "On" is an assertion a
 * map with no cop capacity (an mp arena) is free to ignore.
 *
 * IMMUNITY is a DIFFERENT axis and must not be folded into Cops: Cops = do
 * police cars exist, Immunity = they may exist but cannot come after you
 * (gPlayerImmune). "Police cars: Off" clears the map; "Immunity: On" leaves
 * them there and disarms them. */
static void LblCops(void* ud, char* o, int n)    { (void)ud; snprintf(o, n, "Police cars: < %s >", gMp.config.cops ? "On" : "Off"); }
static void LblCopDiff(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Cop difficulty: < %d >", gMp.config.copDifficulty); }
static void LblInvuln(void* ud, char* o, int n)  { (void)ud; snprintf(o, n, "Invulnerability: < %s >", gMp.config.invuln ? "On" : "Off"); }
static void LblImmune(void* ud, char* o, int n)  { (void)ud; snprintf(o, n, "Immunity: < %s >", gMp.config.immune ? "On" : "Off"); }

static int AdjCops(void* ud, int dir)   { (void)ud; gMp.config.cops = (gMp.config.cops + dir + 2) & 1; MpConfigSave(); return 1; }
static int AdjInvuln(void* ud, int dir) { (void)ud; gMp.config.invuln = (gMp.config.invuln + dir + 2) & 1; MpConfigSave(); return 1; }
static int AdjImmune(void* ud, int dir) { (void)ud; gMp.config.immune = (gMp.config.immune + dir + 2) & 1; MpConfigSave(); return 1; }

/* Wraps rather than stopping at the ends, so the row is a cycle and cannot be
 * walked off the end into a value the engine's tables do not have. */
static int AdjCopDiff(void* ud, int dir)
{
	(void)ud;

	gMp.config.copDifficulty += dir;
	if (gMp.config.copDifficulty < 0) gMp.config.copDifficulty = MP_COP_DIFF_MAX;
	if (gMp.config.copDifficulty > MP_COP_DIFF_MAX) gMp.config.copDifficulty = 0;

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

	/* The large main map: shape 0, no sub-level. MpSetMapShape is what makes the
	 * answer real -- without it MpLaunchLocal derived gSubGameNumber from the
	 * boot -mp flag, so this and Multiplayer loaded the same level. */
	MpSetMapShape(0, 0);
	MpSetSubGame(0);		/* the whole city, no sub-level */
	jer_frontend_goto(3);		/* continue the stock flow: time of day */
	return 1;
}

static int ActModeMP(void* ud)
{
	(void)ud;

	/* The city's small multiplayer map. Which ARENA (0/1) is chosen on the stock
	 * per-city level list reached below, whose buttons write gSubGameNumber
	 * (SetVariable code 8); the host's START turns that into the session arena. */
	MpSetMapShape(1, 0);

	/* 35..38 are the per-city multiplayer level lists (35 = Chicago,
	 * 36 = Havana, 37 = Las Vegas, 38 = Rio). */
	jer_frontend_goto(35 + (MpGetGameLevel() & 3));
	return 1;
}

/* Open the Single Player / Multiplayer question (the engine calls this from
 * the city-confirm hook). */
void MpUiOpenModeMenu(void)
{
	/* Log the resolved index BEFORE opening. MpMenuOpen no-ops on an unregistered
	 * menu (idx -1), so jer_frontend_open(-1) used to be a silent nothing -- and
	 * "the Single Player / Multiplayer menu never appeared" then had no evidence
	 * at all, since the only trace was the line MpOnFrontendConfirm prints when
	 * it BELIEVES it opened it. */
	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] opening mp.mode (resolved idx %d)\n", gMenuIdx[M_MODE]);

	MpMenuOpen(M_MODE);
}

/* Ask for the Single Player / Multiplayer menu on the NEXT frame instead of now.
 * See gModeMenuArmed for why opening it from inside the confirm hook does not
 * work. */
void MpUiArmModeMenu(void)
{
	gModeMenuArmed = 8;
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

	/* The attempt says itself in the console as well as the lobby: a join that
	 * cannot reach the server used to be silent for the whole timeout, and a
	 * player who sees nothing happen clicks again. */
	if (s != NULL)
		jer_console_log("joining %s:%d", s->ip, s->port);

	/* asynchronous: the lobby shows "Connecting to <addr>..." while the
	 * socket connects, instead of the menu freezing for up to 5 s */
	if (s != NULL && MpBeginJoinAsync(s->ip, s->port))
		jer_frontend_open(gMenuIdx[M_LOBBY]);

	return 1;
}

/* "host" or "host:port" -> the two parts. Returns 1 on success, 0 if the text after
 * the colon is not a usable port -- so a typo is REFUSED rather than silently dialled
 * on the configured port. No colon at all leaves `*port` alone, which is what makes
 * "no port" mean "the normal port".
 *
 * A pasted LINK is taken too: "http://host:1318/" and "host/whatever" both reduce to
 * the address, because a scheme and a path are things a browser bar hands you and
 * neither is part of a game address. */
static int MpManualParse(const char* text, char* host, size_t hostLen, int* port)
{
	char buf[160];
	const char* start = text;
	const char* end;
	const char* colon;
	size_t n;
	int p, d;

	{
		const char* scheme = strstr(text, "://");

		if (scheme != NULL)
			start = scheme + 3;
	}

	end = start + strlen(start);

	{
		const char* slash = strchr(start, '/');

		if (slash != NULL)
			end = slash;
	}

	n = (size_t)(end - start);

	if (n == 0 || n >= sizeof(buf) || n >= hostLen)
		return 0;

	memcpy(buf, start, n);
	buf[n] = '\0';

	colon = strrchr(buf, ':');

	if (colon == NULL)
	{
		snprintf(host, hostLen, "%s", buf);
		return 1;
	}

	n = (size_t)(colon - buf);

	if (n == 0 || n >= hostLen)
		return 0;

	for (d = 1; colon[d] != '\0'; d++)
	{
		if (colon[d] < '0' || colon[d] > '9')
			return 0;
	}

	if (d == 1)
		return 0;

	p = atoi(colon + 1);

	if (p < 1 || p > 65535)
		return 0;

	memcpy(host, buf, n);
	host[n] = '\0';
	*port = p;

	return 1;
}

/* Seed the address from the first LAN game we can see, so the usual case is an
 * address already sitting there. Only until the player types: gManualSeed marks
 * that they have taken over. */
static void MpManualSeed(void)
{
	MP_SERVER* s = (MpDiscoveryCount() > 0) ? MpDiscoveryGet(0) : NULL;

	if (!gManualSeed && s != NULL && s->ip[0] != '\0')
	{
		snprintf(gManualIp, sizeof(gManualIp), "%s", s->ip);
		gManualSeed = 1;
	}
}

/* The submenu's Connect row. */
static int ActManualConnect(void* ud)
{
	char host[128];

	/* The base is the CONFIGURED port, never the last thing that was typed: no port
	 * in the text must mean "the normal port", or a one-off :1400 would quietly
	 * follow the player around. */
	int port = gMp.config.port;

	(void)ud;

	if (!MpManualParse(gManualIp, host, sizeof(host), &port))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] manual address '%s' does not parse (want host or host:port)\n", gManualIp);

		jer_error("Use an address or host:port, not %s", gManualIp);
		return 1;
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] joining manual %s:%d\n", host, port);

	jer_console_log("joining %s:%d", host, port);

	if (MpBeginJoinAsync(host, port))
		jer_frontend_open(gMenuIdx[M_LOBBY]);

	return 1;
}

/* ------------------------------------------------------------------ */
/* The typed address field                                             */
/* ------------------------------------------------------------------ */
int MpUiManualEditing(void)
{
	return gManualEdit;
}

void MpUiManualBegin(void)
{
	snprintf(gManualUndo, sizeof(gManualUndo), "%s", gManualIp);
	gManualEdit = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] manual address: typing (Enter keeps it, Escape undoes)\n");
}

void MpUiManualCommit(void)
{
	gManualEdit = 0;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] manual address: '%s'\n", gManualIp);
}

void MpUiManualCancel(void)
{
	snprintf(gManualIp, sizeof(gManualIp), "%s", gManualUndo);
	gManualEdit = 0;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] manual address: reverted to '%s'\n", gManualIp);
}

/* One typed character (or NULL for backspace) from PsyX, while the field is editing.
 * The accepted set is what an address can hold: letters and digits for a name, dots
 * and hyphens for a host name, a colon for the port. Anything else is dropped rather
 * than shown, so the field cannot fill with text no resolver would accept. */
void MpUiManualType(const char* text)
{
	size_t len = strlen(gManualIp);

	if (text == NULL)
	{
		if (len > 0)
			gManualIp[len - 1] = '\0';

		return;
	}

	if (text[0] == '\0' || len + 1 >= MP_MANUAL_MAX)
		return;

	if (!((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') ||
	      (text[0] >= '0' && text[0] <= '9') ||
	      text[0] == '.' || text[0] == '-' || text[0] == ':' || text[0] == '_'))
		return;

	snprintf(gManualIp + len, sizeof(gManualIp) - len, "%s", text);
}

/* Cross on the field: start typing, or stop. Committing on the SECOND press as well
 * as on Enter matters because the pad still navigates this screen -- a player with a
 * pad and no keyboard must be able to leave the field. */
static int ActManualEdit(void* ud)
{
	(void)ud;

	if (gManualEdit)
		MpUiManualCommit();
	else
		MpUiManualBegin();

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

/* The lobby's player rows are PAGED. The frontend menu holds JER_FE_MAX_ITEMS
 * rows and the pinned ones (status / Start Match / Leave) take three, so a
 * sixteen-player lobby cannot fit on one page: the old loop stopped at the ninth
 * player and the rest were simply invisible. This row is the page turner. */
static int gLobbyPage;			/* which page of the player list is shown */

static int ActLobbyNextPage(void* ud)
{
	(void)ud;

	gLobbyPage++;
	jer_frontend_refresh();	/* re-runs LobbyOnEnter, which rebuilds the rows */
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
	jer_frontend_open(gMenuIdx[M_JMP_OPTIONS]);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Static menus                                                        */
/* ------------------------------------------------------------------ */
static JER_FE_ITEM kRootItems[] =
{
	/* ONLINE first: it is the one row that reaches past the LAN, and the screen a
	 * player who has been told an address wants. */
	{ "Online",       NULL, NULL, NULL,           NULL, -1, 0 },
	{ "LAN",          NULL, NULL, NULL,           NULL, -1, 0 },
	/* JMP OPTIONS is a SIBLING of LAN, not a row inside it: it configures the
	 * session (name, mod matching, port) whichever way the player is about to
	 * connect. */
	{ "JMP Options",  NULL, NULL, NULL,           NULL, -1, 0 },
	{ "Split-Screen", NULL, NULL, ActSplitScreen, NULL, -1,    0 },
	{ "Back",         NULL, NULL, NULL,           NULL, -1,    1 },
};
static const JER_FE_MENU kRootMenu = { "mp.root", kRootItems, 5, NULL, NULL };

static JER_FE_ITEM kLanItems[] =
{
	{ "Host Game", NULL, NULL, ActHostGame, NULL, -1,    0 },
	{ "Join Game", NULL, NULL, ActJoinEnter, NULL, -1,    0 },
	{ "Back",      NULL, NULL, NULL,         NULL, -1,        1 },
};
static const JER_FE_MENU kLanMenu = { "mp.lan", kLanItems, 3, NULL, NULL };

static const JER_FE_ITEM kHostItems[] =
{
	/* SESSION RULES first: these are the host's gameplay settings for the match
	 * about to be set up, and they apply to everyone who joins it. */
	{ NULL, LblCops,    NULL, NULL, AdjCops,    -1, 0 },
	{ NULL, LblCopDiff, NULL, NULL, AdjCopDiff, -1, 0 },
	{ NULL, LblImmune,  NULL, NULL, AdjImmune,  -1, 0 },
	{ NULL, LblInvuln,  NULL, NULL, AdjInvuln,  -1, 0 },

	{ "Take a Ride", NULL, NULL, ActTakeARide, NULL, -1, 0 },
	{ "Back",        NULL, NULL, NULL,         NULL, -1, 1 },
};
static const JER_FE_MENU kHostMenu = { "mp.host", kHostItems, 6, NULL, NULL };

static const JER_FE_ITEM kHostSetItems[] =
{
	{ "Single Player", NULL, NULL, ActModeSP, NULL, -1, 0 },
	{ "Multiplayer",   NULL, NULL, ActModeMP, NULL, -1, 0 },
	{ "Back",          NULL, NULL, NULL,      NULL, -1, 1 },
};
static const JER_FE_MENU kHostSetMenu = { "mp.mode", kHostSetItems, 3, NULL, NULL };

/* Frontend character picker. Like the carhacks car roster, it is a live cycle
 * row: left/right changes the stored choice, the label is rebuilt on every
 * layout, and the actual model import happens when a level can hold it (or
 * immediately if a match is already running). */
static void LblPreferredCharacter(void* ud, char* out, int max)
{
	int selected = MpPedModelSelected();
	int model = selected;

	(void)ud;

	if (selected == MP_PED_MODEL_JERICHO && !MpPedModelSelectable(MP_PED_MODEL_JERICHO))
		model = MpPedModelEffective(selected);

	if (!MpPedModelSelectable(MP_PED_MODEL_JERICHO))
		snprintf(out, max, "Preferred Character: %s (Jericho unavailable)",
			MpPedModelName(model));
	else
		snprintf(out, max, "Preferred Character: %s", MpPedModelName(model));
}

static int AdjPreferredCharacter(void* ud, int dir)
{
	int model = MpPedModelSelected();
	int i;

	(void)ud;

	if (dir == 0)
		return 1;

	for (i = 0; i < MP_PED_MODEL_COUNT; i++)
	{
		model = (model + dir + MP_PED_MODEL_COUNT) % MP_PED_MODEL_COUNT;

		if (MpPedModelSelectable(model))
		{
			MpSetPedModel(model);
			break;
		}
	}

	return 1;
}

static JER_FE_ITEM kJmpOptionsItems[] ={
	{ "Change Name",    NULL, NULL, NULL,       NULL,       -1, 0 },
	{ NULL, LblPreferredCharacter, NULL, NULL, AdjPreferredCharacter, -1, 0 },
	{ NULL, LblEnforce, NULL, NULL, AdjEnforce, -1,       0 },
	{ NULL, LblStrict,  NULL, NULL, AdjStrict,  -1,       0 },
	{ NULL, LblPort,    NULL, NULL, AdjPort,    -1,       0 },
	{ "Back",           NULL, NULL, NULL,       NULL,      -1,     1 },
};
static const JER_FE_MENU kJmpOptionsMenu = { "mp.jmpoptions", kJmpOptionsItems, 6, NULL, NULL };

/* Resolve every mp menu by id and point the submenu rows at what they got. The
 * item arrays are written with -1 and wired HERE, because the registered index
 * depends on what every OTHER module registered first: carhacks activates before
 * mp and registers its car-select menu, so mp's own order was one index off and
 * the LAN row opened the root menu again. */
static void MpResolveMenus(void)
{
	int i;

	for (i = 0; i < M_COUNT; i++)
	{
		gMenuIdx[i] = jer_frontend_find(kMenuIds[i]);

		if (gMenuIdx[i] < 0 && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] frontend menu '%s' is NOT registered\n", kMenuIds[i]);
	}

	kRootItems[0].submenu = gMenuIdx[M_ONLINE];
	kRootItems[1].submenu = gMenuIdx[M_LAN];
	kRootItems[2].submenu = gMenuIdx[M_JMP_OPTIONS];
	kLanItems[0].submenu = gMenuIdx[M_HOST];
	kLanItems[1].submenu = gMenuIdx[M_JOIN];
	kJmpOptionsItems[0].submenu = gMenuIdx[M_NAME];

	gMenusResolved = 1;
}

/* ------------------------------------------------------------------ */
/* Dynamic menus (join / lobby / name)                                 */
/* ------------------------------------------------------------------ */
static JER_FE_ITEM gJoinItems[JER_FE_MAX_ITEMS];
static JER_FE_MENU gJoinMenu = { "mp.join", gJoinItems, 0, NULL, NULL };
static int  gJoinRows;			/* how many of gJoinMenu's items are SERVER rows:
				 * the rest are the message row and the two pinned
				 * controls, which the table must not paint over */

static JER_FE_ITEM gLobbyItems[JER_FE_MAX_ITEMS];
static JER_FE_MENU gLobbyMenu = { "mp.lobby", gLobbyItems, 0, NULL, NULL };
static int gLobbyBuiltCount = -1;	/* player count the lobby was last built with */
static char gLobbyMoreLabel[40];
static int gLobbyJoinState = -1;	/* join state the lobby was last built with */
static char gLobbyLabel[MP_MAX_PLAYERS][40];
static char gLobbyStatus[64];		/* "Connecting to <addr>..." / "(could not connect)" */

static JER_FE_ITEM gNameItems[JER_FE_MAX_ITEMS];
static JER_FE_MENU gNameMenu = { "mp.name", gNameItems, 0, NULL, NULL };

/* manual LAN address: the field, Connect, Back -- exactly the three ManualOnEnter
 * builds, so the count here is the count there */
static JER_FE_ITEM gManualItems[3];
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

	/* The SCREEN owns its scan. The LAN row used to be the only thing that opened
	 * the discovery socket, so the browser could sit on screen with nothing ever
	 * arriving (and be impossible to test); arming it here means entering by any
	 * route starts listening. The guard keeps a rebuild -- which re-runs this --
	 * from restarting the scan clock. */
	if (!gJoinBrowsing)
	{
		gJoinScanStart = MpNowMs();
		gJoinScanning = 1;
		gJoinBrowsing = 1;

		MpDiscoveryStart(0);
	}

	ns = MpDiscoveryCount();
	/* one beacon interval is the shortest wait before believing the LAN is
	 * really empty -- a beacon already in flight still counts */
	scanning = (gJoinScanStart != 0) && (MpNowMs() - gJoinScanStart) < MP_JOIN_SCAN_MS;
	gJoinScanning = scanning;

	/* The rows carry NO label. The engine draws nothing for them and still
	 * navigates and highlights them, which is what lets the table be painted on
	 * them: a menu row is ONE proportional string, so five columns cannot be
	 * padded into line (see TableDraw). */
	for (i = 0; i < ns && k < JER_FE_MAX_ITEMS - 2; i++)
	{
		if (MpDiscoveryGet(i) == NULL)
			continue;

		gJoinItems[k].label = NULL;
		gJoinItems[k].get_label = NULL;
		gJoinItems[k].userdata = (void*)(intptr_t)i;
		gJoinItems[k].on_activate = ActJoinServer;
		gJoinItems[k].on_adjust = NULL;
		gJoinItems[k].submenu = -1;
		gJoinItems[k].is_back = 0;
		k++;
	}

	gJoinRows = k;

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

	/* manual address -- its own submenu: there is somewhere to TYPE here, and a
	 * DNS name would never fit on a single row with one adjust axis */
	gJoinItems[k].label = NULL;
	gJoinItems[k].label = "Manual Connect";
	gJoinItems[k].userdata = NULL;
	gJoinItems[k].on_activate = NULL;
	gJoinItems[k].on_adjust = NULL;
	gJoinItems[k].submenu = gMenuIdx[M_MANUAL];
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
			snprintf(gLobbyStatus, sizeof(gLobbyStatus), "(Could not connect!)");

		gLobbyItems[k].label = gLobbyStatus;
		gLobbyItems[k].get_label = NULL;
		gLobbyItems[k].userdata = NULL;
		gLobbyItems[k].on_activate = NULL;
		gLobbyItems[k].on_adjust = NULL;
		gLobbyItems[k].submenu = -1;
		gLobbyItems[k].is_back = 0;
		k++;
	}

	/* Player rows, PAGED: with the pool at 16 players a single page cannot hold
	 * them, and a truncated list is how players 10..16 become invisible. perPage
	 * loses one row to the page turner while there is more than one page. */
	{
		int rows = JER_FE_MAX_ITEMS - 3;	/* status / Start Match / Leave */
		int active = 0, perPage, pages, first, seen = 0;

		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			if (gMp.players[i].active)
				active++;
		}

		perPage = (rows > 1) ? rows - 1 : 1;	/* one row is the page turner */

		if (active <= rows)
			perPage = rows;			/* everything fits: no turner needed */

		pages = (perPage > 0) ? (active + perPage - 1) / perPage : 1;

		if (pages < 1)
			pages = 1;

		if (gLobbyPage >= pages)
			gLobbyPage = 0;

		if (gLobbyPage < 0)
			gLobbyPage = 0;

		first = gLobbyPage * perPage;

		for (i = 0; i < MP_MAX_PLAYERS && k < JER_FE_MAX_ITEMS - 3; i++)
		{
			if (!gMp.players[i].active)
				continue;

			if (seen++ < first)
				continue;

			if (seen > first + perPage)
				break;

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

		if (pages > 1 && k < JER_FE_MAX_ITEMS - 2)
		{
			snprintf(gLobbyMoreLabel, sizeof(gLobbyMoreLabel),
				"More players (%d/%d)", gLobbyPage + 1, pages);

			gLobbyItems[k].label = gLobbyMoreLabel;
			gLobbyItems[k].get_label = NULL;
			gLobbyItems[k].userdata = NULL;
			gLobbyItems[k].on_activate = ActLobbyNextPage;
			gLobbyItems[k].on_adjust = NULL;
			gLobbyItems[k].submenu = -1;
			gLobbyItems[k].is_back = 0;
			k++;
		}
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
/* The address field, Connect, Back. Built on entry so the label picks up the seeded
 * address. The field is TYPED: Cross starts the keyboard, Cross again (or Enter)
 * keeps it, Escape undoes -- see MpUiManualType. */
static void ManualOnEnter(void* ud)
{
	int k = 0;

	(void)ud;

	MpManualSeed();

	gManualItems[k].label = NULL;
	gManualItems[k].get_label = LblManualField;
	gManualItems[k].userdata = NULL;
	gManualItems[k].on_activate = ActManualEdit;
	gManualItems[k].on_adjust = NULL;
	gManualItems[k].submenu = -1;
	gManualItems[k].is_back = 0;
	k++;

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

/* ------------------------------------------------------------------ */
/* The Online browser (the discovery server's list)                    */
/* ------------------------------------------------------------------ */
/* Five columns, which is the whole reason jer_text.h exists: a row's label is ONE
 * proportional string in the engine's fixed 256-unit box, and no amount of
 * padding lines columns up in a proportional font. So a server row carries an
 * EMPTY label (the engine draws nothing for it and still navigates and
 * highlights it) and the table is painted from JER_EVENT_FRONTEND_DRAW, once the
 * engine has drawn the screen -- see mp.c's MpOnFrontendDraw. Drawing from
 * JER_EVENT_FRAME does not work: that fires BEFORE DrawScreen and the menu box
 * lands on top of anything drawn there.
 *
 * The column offsets sit inside that box and the text is drawn in the FRONTEND's
 * own face at the console's size, so the screen reads as one screen. The menu asks
 * for a wide box (row_w) so five columns have somewhere to go, and for a tighter
 * pitch (row_step) so more servers fit -- with the last two items pinned to the
 * bottom (bottom_items), Manual and Back stop moving every time the list is
 * rebuilt. The gap under the headings is deliberately bigger than the pitch: the
 * headings are a header, not another row. */
#define TABLE_SCALE		100	/* the console's size, in the frontend's face */
#define TABLE_COL_NAME		0
#define TABLE_COL_PLAYERS	196
#define TABLE_COL_MODE		268
#define TABLE_COL_CITY		434
#define TABLE_COL_PING		528
#define TABLE_NAME_CHARS	15	/* what the NAME column holds before it is cut */
#define TABLE_ROW_W		560	/* the menu's row width; the columns above fit it */
#define TABLE_ROW_TOP		210	/* where the ROSTER starts. The stock 190 sits barely 24
					 * units under the screen title, which left nowhere for
					 * a heading line: it landed ON the title */
#define TABLE_ROW_STEP		28	/* the pitch BETWEEN servers. Tighter than this and the
					   rows stop reading as rows: at 100% the console's own
					   line pitch is 26, so 24 had them sitting on each other */
#define TABLE_HEAD_GAP		38	/* headings -> first server. Deliberately LARGER than
					 * the pitch above, so the column headings read as
					 * headings and not as another server row */
#define TABLE_ROW_BOTTOM	474	/* where the pinned controls sit: the stock 462 leaves a
					 * wider margin than a full-screen browser wants, and
					 * these read as the bottom of the SCREEN */

static JER_FE_ITEM gOnlineItems[JER_FE_MAX_ITEMS];
/* The heading is the screen's name, as the design calls for: it is the one piece
 * of this screen the engine draws (rows are ours). row_w asks the engine for a box
 * wide enough for five columns -- see the layout note above. */
static JER_FE_MENU gOnlineMenu = { "mp.online", gOnlineItems, 0, NULL, NULL,
				   "JERICHO DISCOVERY SERVER", NULL, NULL, TABLE_ROW_W };
static int gOnlineRev = -1;		/* the list revision these rows were built from */
static int gOnlineRows;			/* how many of item_count are SERVER rows: the rows
					 * above the two fixed ones (Manual, Back),
					 * which is what the draw loop must stop at */
static char gOnlineApplied[160];	/* the master URL already handed to the client */

/* Hand the config's address to the fetcher, at most once per distinct value.
 * MpOnlineSetMaster RESOLVES, and this runs on a menu press -- the same place,
 * and for the same reason, that a join resolves its host.
 *
 * Returns 1 when it handed the address over (the set already refreshed), so the
 * caller does not open a second socket for the same visit. */
static int OnlineApplyConfig(void)
{
	if (strcmp(gOnlineApplied, gMp.config.masterUrl) == 0)
		return 0;

	snprintf(gOnlineApplied, sizeof(gOnlineApplied), "%s", gMp.config.masterUrl);
	MpOnlineSetMaster(gMp.config.masterUrl);

	return 1;
}

/* The discovery status, said where it FITS. The menu row that carries it is capped
 * at 32 characters, which is why "contacting the discovery server..." arrives cut in
 * half; the console rows (the same place the version toast lands) have room for the
 * whole sentence, and the player is told when the list arrives -- or that the server
 * cannot be reached at all, which is the case worth knowing about. */
static int gOnlineAnnounced = -1;

static void OnlineSay(const char* fmt, ...)
{
	char text[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);

	/* The whole sentence, which the 32-character menu row cannot hold. The console
	 * mirrors its rows into the session log ("[console] ..."), so this is also what
	 * an automated run reads -- the row itself lives about two seconds. */
	jer_console_log("%s", text);
}

static void OnlineAnnounce(void)
{
	int st = MpOnlineState();

	if (st == gOnlineAnnounced)
		return;

	gOnlineAnnounced = st;

	switch (st)
	{
	case MP_ONLINE_CONNECTING:
		OnlineSay("discovery: contacting %s", gMp.config.masterUrl);
		break;
	case MP_ONLINE_READY:
		OnlineSay("discovery: %d server(s) from %s",
			MpOnlineCount(), gMp.config.masterUrl);
		break;
	case MP_ONLINE_FAILED:
		OnlineSay("discovery: cannot reach %s", gMp.config.masterUrl);
		break;
	default:
		break;
	}
}

/* Why the list is empty. A player staring at an empty table should not have to
 * guess whether the fetch is still running or it failed. There is deliberately no
 * text for MP_ONLINE_OFF: an unset address draws the browser with no entries (see
 * OnlineOnEnter), so that case never asks. */
static const char* OnlineStateText(void)
{
	switch (MpOnlineState())
	{
	case MP_ONLINE_CONNECTING:
	case MP_ONLINE_READING:			return "(contacting the discovery server...)";
	case MP_ONLINE_FAILED:			return "(the discovery server is unavailable)";
	default:				return "(no servers listed)";
	}
}

static void OnlineRowClear(int k)
{
	gOnlineItems[k].label = NULL;
	gOnlineItems[k].get_label = NULL;
	gOnlineItems[k].userdata = NULL;
	gOnlineItems[k].on_activate = NULL;
	gOnlineItems[k].on_adjust = NULL;
	gOnlineItems[k].submenu = -1;
	gOnlineItems[k].is_back = 0;
}

static int ActJoinOnline(void* ud)
{
	int idx = (int)(intptr_t)ud;
	const MP_ONLINE_SERVER* s = MpOnlineGet(idx);

	/* Say exactly what we are joining: a listed address is the one thing here
	 * that can be stale, and the only symptom is "connection lost". */
	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] joining listed '%s' %s:%d (%d/%d, %s, city %d)\n",
			s != NULL ? s->name : "?", s != NULL ? s->host : "?", s != NULL ? s->port : -1,
			s != NULL ? s->players : -1, s != NULL ? s->maxPlayers : -1,
			MpModeName(s != NULL ? s->gamemode : MP_GAMEMODE_TAKEADRIDE), s != NULL ? s->city : -1);

	/* asynchronous, exactly like the LAN rows: the lobby shows progress while
	 * the socket connects instead of the menu freezing */
	if (s != NULL && MpBeginJoinAsync(s->host, s->port))
		jer_frontend_open(gMenuIdx[M_LOBBY]);

	return 1;
}

/* Say the master's greeting, once per distinct text.
 *
 * A CHAT line, not a status line: chat lives far longer on screen (20 s full plus a
 * 3 s fade, against 4 s + 1 s) and is drawn even while the console is toggled off,
 * so a reply to "where is the server list?" is actually readable. The colour is the
 * master's own ("motdColor": "#RRGGBB" in its config), and a long line WRAPS onto
 * the next row -- jer_console wraps at measured width and tears at a space -- so an
 * operator can write a sentence instead of a label. */
static void OnlineMotdSay(void)
{
	char buf[224];
	JER_CONSOLE_SEG seg;
	unsigned char r, g, b;

	if (MpOnlineMotd()[0] == '\0')
		return;

	if (gOnlineMotdSaid == MpOnlineMotdRev())
		return;					/* already said */

	gOnlineMotdSaid = MpOnlineMotdRev();

	MpOnlineMotdColor(&r, &g, &b);

	/* "MASTER:" so the line is attributable: the same corner carries chat, and a
	 * bare sentence there would read as something a player said. */
	snprintf(buf, sizeof(buf), "MASTER: %s", MpOnlineMotd());

	seg.text = buf;
	seg.r = r;
	seg.g = g;
	seg.b = b;
	seg.ambient = 0;

	jer_console_chat(&seg, 1);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] master motd: %s\n", MpOnlineMotd());
}

static void OnlineOnEnter(void* ud)
{
	int i, k = 0, count;

	(void)ud;

	/* A fresh look every time the screen opens: a stale list is worse than none,
	 * and the fetch is asynchronous -- the rows appear as they arrive. Skipped on
	 * the visit that just set the address, because that already fetched. */
	if (!OnlineApplyConfig())
		MpOnlineSetWatching(1);        /* fetch now; the tick stops it on the way out */

	/* The master's greeting, in the colour its config asks for. It arrives WITH the
	 * listing (same body), so on a cold open it is usually not here yet -- the tick
	 * says it the moment it lands, which is why this is a call, not a one-off. */
	OnlineMotdSay();

	count = MpOnlineCount();

	for (i = 0; i < count && k < JER_FE_MAX_ITEMS - 2; i++)
	{
		OnlineRowClear(k);
		gOnlineItems[k].userdata = (void*)(intptr_t)i;
		gOnlineItems[k].on_activate = ActJoinOnline;
		k++;
	}

	gOnlineRows = k;		/* how many rows above are servers */

	/* An unset discovery server is not a state to print a sentence about -- there is
	 * simply nothing to list, and the browser's columns above say so better than a
	 * message row can. The other states still speak up: contacting and unavailable
	 * are things going WRONG, which the player cannot see from an empty table. */
	if (count == 0 && MpOnlineState() != MP_ONLINE_OFF)
	{
		OnlineRowClear(k);
		gOnlineItems[k].label = OnlineStateText();
		k++;
	}

	/* Manual address: the way in for a host that is not listed (yet), or one
	 * behind a name the player already knows. The row does NOT print the address:
	 * it is a door to the typing screen, and an address the player is about to
	 * replace is noise -- it also read as though the field already held their
	 * target. The field screen shows what is actually set. */
	OnlineRowClear(k);
	gOnlineItems[k].label = "Manual address ...";
	gOnlineItems[k].submenu = gMenuIdx[M_MANUAL];
	k++;

	/* Hosting online: NOT WIRED UP YET, and it was worse than nothing to offer the
	 * row - pressing it went nowhere, with no way to tell a missing feature from a
	 * broken one. It says so instead. The LAN row (M_LAN) is the real host path. */
	OnlineRowClear(k);
	gOnlineItems[k].label = "Hosting In-Game: Coming Soon ...";
	gOnlineItems[k].on_activate = NULL;	/* a statement, not a button */
	k++;

	OnlineRowClear(k);
	gOnlineItems[k].label = "Back";
	gOnlineItems[k].is_back = 1;
	k++;

	gOnlineMenu.item_count = k;

	/* what this list was built from, so MpUiTick rebuilds only on a real change */
	gOnlineRev = MpOnlineRevision();
}

/* --- the server table, and its two sources ---------------------------
 * ONE table: the Online screen lists what the discovery server knows, the LAN
 * screen lists the beacons this machine can hear. They are deliberately separate
 * lists -- a LAN row arrived on the wire, an online row is what another machine
 * was told -- but the columns, the box and the drawing are the same, so they are
 * the same code over a normalised row.
 */
typedef struct TABLE_ROW
{
	char name[MP_NAME_MAX + 8];	/* room for the LIVE marker a LAN row carries */
	int  players, maxPlayers;
	int  rules;
	int  gamemode;
	int  city;
	int  pingMs;
} TABLE_ROW;

static int TableRowSource(int online, int index, TABLE_ROW* out)
{
	if (online)
	{
		const MP_ONLINE_SERVER* s = MpOnlineGet(index);

		if (s == NULL)
			return 0;

		snprintf(out->name, sizeof(out->name), "%.*s", TABLE_NAME_CHARS, s->name);
		out->players = s->players;
		out->maxPlayers = s->maxPlayers;
		out->rules = s->rules;
		out->gamemode = s->gamemode;
		out->city = s->city;
		out->pingMs = s->pingMs;		/* measured, or 0 = not yet */
		return 1;
	}

	{
		MP_SERVER* s = MpDiscoveryGet(index);

		if (s == NULL)
			return 0;

		/* A LAN row says whether the host has STARTED, which a joiner wants to
		 * know and an online row has no equivalent of (that list is live by
		 * definition). It rides the NAME cell, clipped to make room, so the five
		 * columns stay five. */
		snprintf(out->name, sizeof(out->name), "%.*s%s", TABLE_NAME_CHARS - 5,
			s->hostName, s->inProgress ? " LIVE" : "");
		out->players = s->players;
		out->maxPlayers = s->maxPlayers;
		out->rules = s->rules;
		out->gamemode = s->gamemode;
		out->city = s->city;
		out->pingMs = 0;			/* no measurement for a beacon row */
		return 1;
	}
}

/* The table. Called from the frontend draw hook (after the engine has drawn the
 * screen), and only while the menu that shows it is on screen. */
static void TableDraw(int online)
{
	int count, rows, item_rows, i, x, y, w, h, headY = 0, firstRowY = 0;

	if (!gInFrontend || gMpCtx == NULL)
		return;

	if (!MpMenuIs(online ? M_ONLINE : M_JOIN))
		return;

	count = online ? gOnlineMenu.item_count : gJoinMenu.item_count;
	item_rows = online ? gOnlineRows : gJoinRows;
	rows = online ? MpOnlineCount() : MpDiscoveryCount();

	/* Stop at the rows that ARE servers: item_count also carries the message row
	 * and the two pinned controls, and a server row painted over one of those
	 * would be two strings in one row. g*Rows was set by the loop that built them. */
	if (item_rows < rows)
		rows = item_rows;

	/* Where the heading line goes: above the LIST AREA, not above row 0 -- with an
	 * empty list, row 0 IS the first pinned control. jer_frontend_list_top is the
	 * one thing that can say where the list starts when it has no rows at all. */
	if (jer_frontend_row_rect(0, &x, &y, &w, &h))
	{
		headY = jer_frontend_list_top();

		if (headY <= 0)
			headY = y;		/* no list area to ask (older engine): row 0 */

		headY -= TABLE_HEAD_GAP;
	}

	/* where the first ROSTER row actually landed: the engine may have centred the
	 * list in the band above the controls, so this is not necessarily row_top */
	if (rows > 0 && jer_frontend_row_rect(0, &x, &y, &w, &h))
		firstRowY = y;

	/* NO early return on an empty list: the browser still has to LOOK like a browser.
	 * The heading line and the columns are drawn whether or not there are rows, so an
	 * unset discovery server is an empty table rather than a sentence sitting where a
	 * table should be. The row loop below simply does not run. */
	if (count <= 0)
		return;

	/* One line the first time a table is painted: the only thing an automated run can
	 * check about a screen whose whole point is what it LOOKS like (the pixels are
	 * checked by hand). It carries the GEOMETRY -- pitch, gaps, the heading's y and
	 * the roster's -- because those are what get tuned, and the background art makes
	 * them unmeasurable from a screenshot. An empty browser reports them too; its row
	 * count is then 0, which is itself the answer. */
	if (MpDebugOn() && gMpCtx != NULL)
	{
		static int loggedOnline = 0, loggedOnlineRows = 0;
		static int loggedLan = 0, loggedLanRows = 0;
		int* logged = online ? &loggedOnline : &loggedLan;
		int* loggedRows = online ? &loggedOnlineRows : &loggedLanRows;

		/* The first paint, and then the first paint WITH ROWS -- a different thing,
		 * because the list is usually still empty when the screen opens and the fetch
		 * lands a moment later. The row count in the second line is the one that says
		 * the screen is actually populated. */
		if (!*logged || (!*loggedRows && rows > 0))
		{
			*logged = 1;

			if (rows > 0)
				*loggedRows = 1;

			gMpCtx->jer_log(gMpCtx,
				"[mp] %s table drawn: %d row(s) of %d, columns at x=%d,%d,%d,%d,%d "
				"(scale %d, pitch %d, head gap %d, heading y %d, roster y %d)\n",
				online ? "online" : "LAN", rows, count,
				TABLE_COL_NAME, TABLE_COL_PLAYERS, TABLE_COL_MODE,
				TABLE_COL_CITY, TABLE_COL_PING, TABLE_SCALE,
				TABLE_ROW_STEP, TABLE_HEAD_GAP, headY, firstRowY);
		}
	}

	/* The headings sit ABOVE the first LIST row by more than the pitch: they are a
	 * header, not another row. headY was measured from the LIST AREA up above, so an
	 * empty list cannot drag it down to the first pinned control. Orange, like the
	 * engine's own menu headings, and passed at full brightness because the ink is
	 * halved on the way to the font (a "mid grey" 190 renders as 95 and reads as
	 * smudge next to the rows). */
	if (headY > 0)
	{
		int hy = headY;

		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_NAME,    hy, "SERVER NAME", 255, 190, 90);
		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_PLAYERS, hy, "PLAYERS",     255, 190, 90);
		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_MODE,    hy, "GAMEMODE",    255, 190, 90);
		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_CITY,    hy, "CITY",        255, 190, 90);
		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_PING,    hy, "PING",        255, 190, 90);
	}

	for (i = 0; i < rows; i++)
	{
		TABLE_ROW r;
		char cell[24];

		if (!TableRowSource(online, i, &r) || !jer_frontend_row_rect(i, &x, &y, &w, &h))
			continue;

		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_NAME, y, r.name, 235, 235, 235);

		snprintf(cell, sizeof(cell), "%d/%d", r.players, r.maxPlayers);
		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_PLAYERS, y, cell, 235, 235, 235);

		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_MODE, y, MpModeName(r.gamemode), 235, 235, 235);
		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_CITY, y, MpCityLabel(r.city), 235, 235, 235);

		if (r.pingMs > 0)
			snprintf(cell, sizeof(cell), "%d", r.pingMs);
		else if (r.pingMs == MP_ONLINE_PING_FAILED)
			snprintf(cell, sizeof(cell), "x");	/* listed, but nothing answers */
		else
			snprintf(cell, sizeof(cell), "-");	/* not measured yet */

		jer_text_draw_at(JER_TEXT_FE, TABLE_SCALE, x + TABLE_COL_PING, y, cell, 235, 235, 235);
	}
}

void MpUiDrawOnline(void)	{ TableDraw(1); }
void MpUiDrawJoin(void)		{ TableDraw(0); }

static void WireDynamic(void)
{
	/* The tables' layout, asked of the engine rather than assumed: a wide box for
	 * five columns, a tight pitch so the list holds as many servers as it can, and
	 * the last two rows (Manual address, Back) pinned to the bottom so they do not
	 * move every time the list is rebuilt. Both browsers show the same table, so
	 * both ask for the same thing. */
	gJoinMenu.row_w = TABLE_ROW_W;
	gJoinMenu.row_top = TABLE_ROW_TOP;
	gJoinMenu.row_step = TABLE_ROW_STEP;
	gJoinMenu.bottom_items = 2;
	gJoinMenu.row_bottom = TABLE_ROW_BOTTOM;

	/* TOP-ANCHORED, not centred. Centring a list inside the band is right for a
	 * menu of controls, and wrong for this one: the column headings are pinned to
	 * the band's TOP, so a centred list leaves a hole under them that grows as the
	 * server count shrinks -- measured at 1 row, the heading sat at y=172 and the
	 * first server at y=286, four row pitches of nothing between them. Anchored to
	 * row_top the first server is at 210, one heading gap (38) under the headings,
	 * and every row after it steps by TABLE_ROW_STEP from there. */
	gJoinMenu.list_align = 0;

	gOnlineMenu.row_w = TABLE_ROW_W;
	gOnlineMenu.row_top = TABLE_ROW_TOP;
	gOnlineMenu.row_step = TABLE_ROW_STEP;
	gOnlineMenu.bottom_items = 3;
	gOnlineMenu.row_bottom = TABLE_ROW_BOTTOM;
	gOnlineMenu.list_align = 0;

	gJoinMenu.on_enter = JoinOnEnter;
	gLobbyMenu.on_enter = LobbyOnEnter;
	gNameMenu.on_enter = NameOnEnter;
	gManualMenu.on_enter = ManualOnEnter;
	gOnlineMenu.on_enter = OnlineOnEnter;
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
	/* The browser is the only thing that may talk to the master for a listing.
	 * Starting is the browser's OWN business (OnlineOnEnter, where the screen knows
	 * it is up); what the tick adds is the other half -- noticing that it LEFT, by
	 * any route (Back, Escape, a breadcrumb, a session starting), and stopping the
	 * request and any pending retry on the next frame. A launch that never opens the
	 * browser therefore never speaks to the master at all. */
	{
		/* The screen starts the watch (OnlineOnEnter) but the engine does not
		 * report it as CURRENT until after the enter callback, so an immediate
		 * "not on screen" would cancel the fetch it had just begun -- which is
		 * exactly what left the table empty while the master logged the request.
		 * Only a screen that has been away for a moment ends the watch. */
		static unsigned long offSince;

		if (MpMenuIs(M_ONLINE))
		{
			offSince = 0;

			/* STARTING cannot be left to on_enter: the engine runs it on screen
			 * SETUP, and a screen that is already current when the module registers
			 * never gets one -- which left the roster empty with the master never
			 * being asked. SetWatching is idempotent, so this is one fetch per
			 * visit, from whichever of the two paths gets there first. */
			MpOnlineSetWatching(1);	/* idempotent: one fetch per visit */
		}
		else if (offSince == 0)
			offSince = MpNowMs() ? MpNowMs() : 1;
		else if ((MpNowMs() - offSince) >= 500)
			MpOnlineSetWatching(0);
	}

	/* MP_TEST_ONLINE=1: open the online browser on the first frontend frame, so the
	 * master's listing -- and the motd it carries -- can be checked headlessly. Test
	 * only: nothing reads the variable unless it is set, and the browser is otherwise
	 * reachable only by hand. */
	{
		const char* onlineProbe = getenv("MP_TEST_ONLINE");

		if (onlineProbe != NULL && onlineProbe[0] != '\0' && onlineProbe[0] != '0'
			&& gInFrontend && gMpCtx != NULL && gMenuIdx[M_ONLINE] >= 0
			&& !MpMenuIs(M_ONLINE))
		{
			static unsigned long openSince;

			if (openSince == 0)
				openSince = MpNowMs() ? MpNowMs() : 1;
			else if ((MpNowMs() - openSince) >= 1500)
			{
				gMpCtx->jer_log(gMpCtx,
					"[mp] MP_TEST_ONLINE: opening the online browser (menu %d)\n",
					gMenuIdx[M_ONLINE]);
				jer_frontend_open(gMenuIdx[M_ONLINE]);
				openSince = MpNowMs() ? MpNowMs() : 1;	/* retried if the engine refused */
			}
		}
	}

	OnlineAnnounce();

	/* The greeting lands WITH the listing, i.e. after the screen is already up, so
	 * the say lives here as well as in on_enter (which usually runs first and finds
	 * nothing yet). */
	if (MpMenuIs(M_ONLINE))
		OnlineMotdSay();

	/* One line when the frontend first shows: how many menus the registry holds,
	 * and where mp's own landed. The two numbers differing from mp's enum order is
	 * exactly the "a submenu row pushes a breadcrumb and opens the wrong screen"
	 * bug, so it is worth a line rather than a session of guessing. */
	{
		static int probed = 0;

		if (!probed && gInFrontend && gMpCtx != NULL)
		{
			probed = 1;
			gMpCtx->jer_log(gMpCtx,
				"[mp] frontend: %d menu(s) registered; mp.root=%d mp.lan=%d mp.host=%d mp.mode=%d mp.join=%d mp.lobby=%d\n",
				jer_frontend_menu_count(), gMenuIdx[M_ROOT], gMenuIdx[M_LAN],
				gMenuIdx[M_HOST], gMenuIdx[M_MODE], gMenuIdx[M_JOIN], gMenuIdx[M_LOBBY]);
		}
	}

	/* The Single Player / Multiplayer menu owed by the take-a-ride confirm, opened
	 * HERE and not from inside the hook (see gModeMenuArmed). Retried for a few
	 * frames, and a failure is logged -- "the prompt never appeared" with no trace
	 * at all is what made this hard to see in the first place. */
	if (gModeMenuArmed > 0)
	{
		int want = gMenuIdx[M_MODE];

		gModeMenuArmed--;

		if (want >= 0 && jer_frontend_current_menu() == want)
		{
			gModeMenuArmed = 0;	/* it took */
		}
		else if (gInFrontend)
		{
			MpUiOpenModeMenu();

			if (gModeMenuArmed == 0 && gMpCtx != NULL && jer_frontend_current_menu() != want)
				gMpCtx->jer_log(gMpCtx,
					"[mp] the Single Player / Multiplayer menu did not open (idx %d, on screen %d)\n",
					want, jer_frontend_current_menu());
		}
	}

	if (gLobbyBuiltCount >= 0 && gMp.playerCount != gLobbyBuiltCount)
		jer_frontend_refresh();

	/* the lobby's "Connecting to ..." row clears itself the moment the join
	 * resolves -- accepted (READY) or refused/failed */
	if (MpMenuIs(M_LOBBY) && MpJoinState() != gLobbyJoinState)
		jer_frontend_refresh();

	/* The address field holds the keyboard only while its own menu is up. Left any
	 * other way -- Back/Triangle, or the engine popping the menu -- the flag would
	 * stay set and the keys would keep feeding a field nobody can see. Committing
	 * here is also the self-healing half of the grab. */
	if (gManualEdit && !MpMenuIs(M_MANUAL))
		MpUiManualCommit();

	/* TEST LEVER (MP_TEST_MANUALADDR=<host>[:port]): the address field needs a
	 * KEYBOARD, which the harness does not have, so this fills the field and
	 * presses Connect the way the menu does -- which is what makes "does a typed
	 * address parse, and does the resolver reach it?" a line in a headless run
	 * instead of something only a human at the menu can check. It goes through the
	 * SAME ActManualConnect the row uses, so it exercises the field's parsing
	 * rather than bypassing it. Inert unless set. */
	{
		static int manualTested = 0;
		const char* mt = getenv("MP_TEST_MANUALADDR");

		if (mt != NULL && !manualTested && gInFrontend)
		{
			manualTested = 1;

			snprintf(gManualIp, sizeof(gManualIp), "%s", mt);
			gManualSeed = 1;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] test: manual address <- '%s' (MP_TEST_MANUALADDR)\n", gManualIp);

			ActManualConnect(NULL);
		}
	}

	/* The Join screen is rebuilt only by its on_enter, which the engine runs
	 * on setup/refresh alone -- without this the server list would be frozen
	 * at whatever had been found when the player opened it. */
	if (MpMenuIs(M_JOIN))
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

	/* The Online browser's fetch. Non-blocking by construction, so it is safe
	 * every frame, and it has to run whether or not the screen is up: the reply
	 * is what the screen is waiting for. */
	MpOnlinePoll();

	/* Rebuild the Online rows only when the list they were built from changed --
	 * the same rule the LAN browser follows, and for the same reason: rebuilding
	 * every frame would fight the engine's selection. */
	if (MpMenuIs(M_ONLINE) && MpOnlineRevision() != gOnlineRev)
		jer_frontend_refresh();

	/* TEST LEVER (MP_TEST_OPENMENU=<menu id>): open a named frontend menu once the
	 * frontend is up. A module menu has no headless driver otherwise, so a screen
	 * that has to be LOOKED at (the Online table) cannot be reached in an
	 * automated run without one. Deferred and retried like gModeMenuArmed: opening
	 * a screen from inside the hook that wants it does not take. */
	{
		static int menuTries = -1;
		static int menuOpened = 0;
		const char* want = getenv("MP_TEST_OPENMENU");

		if (want != NULL && menuTries < 0)
			menuTries = 60;		/* ~1 s at 60 fps */

		if (menuTries > 0 && gInFrontend && gMenusResolved)
		{
			int idx = jer_frontend_find(want);

			menuTries--;

			if (idx >= 0 && jer_frontend_current_menu() == idx)
			{
				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] test: opened '%s' (MP_TEST_OPENMENU)\n", want);

				menuTries = 0;		/* it took */
			}
			else if (idx >= 0)
			{
				/* open ONCE: jer_frontend_open pushes a screen stack entry and runs
				 * the menu's on_enter, so opening every retry would stack visits and
				 * re-fetch. From here we only wait for the switch to take. */
				if (menuOpened == 0)
				{
					menuOpened = 1;
					jer_frontend_open(idx);
				}
			}
			else
			{
				menuTries = 0;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] test: no menu '%s' to open (MP_TEST_OPENMENU)\n", want);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Lower-left info overlay: who joined/left + the chat prompt          */
/* ------------------------------------------------------------------ */
void MpNotify(const char* text)
{
	int len, pos;

	if (text == NULL || text[0] == '\0')
		return;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] info: %s\n", text);

	len = (int)strlen(text);

	/* WRAP at MP_NOTIFY_WRAP so a long notice is not clipped at the screen
	 * edge. Each wrapped line takes its own slot in the lower-left log and the
	 * draw stacks them, so the message reads across several rows. Same width
	 * and same break rules as jer_error(): prefer a space in the latter half,
	 * otherwise hard-break (a long token must still wrap). */
	for (pos = 0; pos < len; )
	{
		int take = len - pos;
		int slot;

		if (take > MP_NOTIFY_WRAP)
		{
			int cut = MP_NOTIFY_WRAP;

			while (cut > MP_NOTIFY_WRAP / 2 && text[pos + cut] != ' ')
				cut--;

			take = (cut > MP_NOTIFY_WRAP / 2) ? cut : MP_NOTIFY_WRAP;
		}

		if (take <= 0)
			take = 1;

		slot = gMp.notifyNext % MP_NOTIFY_MAX;
		memcpy(gMp.notifyText[slot], text + pos, (size_t)take);
		gMp.notifyText[slot][take] = 0;
		gMp.notifyUntil[slot] = MpNowMs() + MP_NOTIFY_MS;
		gMp.notifyNext = (slot + 1) % MP_NOTIFY_MAX;

		if (MpDebugOn() && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] notify row '%s'\n", gMp.notifyText[slot]);

		pos += take;

		while (pos < len && text[pos] == ' ')	/* drop the space we broke at */
			pos++;
	}
}

/* ------------------------------------------------------------------ */
/* The engine status console                                          */
/* ------------------------------------------------------------------ */
/* Join/leave and chat go into the scrolling stream rather than the transient
 * amber toasts: the console keeps them readable (and greppable), and a chat
 * line is drawn even while the console itself is toggled off. */
void MpConsoleLine(const char* fmt, ...)
{
	char b[MP_NOTIFY_TEXT_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);

	jer_console_line(b);
}

void MpConsoleChat(const char* name, int colorOn, int r, int g, int b, const char* text,
		   const unsigned char* ink)
{
	char head[MP_NAME_MAX + 12];	/* room for an "[Admin] " tag */
	JER_CONSOLE_SEG segs[2];

	if (text == NULL || text[0] == '\0')
		return;

	/* A speaker's name is drawn in their own colour -- whatever they picked for
	 * their car. With custom colour OFF there is no colour to show ("original"),
	 * so it falls back to cream.
	 *
	 * These are the colours as they should LOOK: jer_console halves every colour
	 * it passes, because the HQ font is drawn with the PSX texture filter on
	 * (x2). See jerConsoleInk. */
	if (!colorOn)
	{
		r = 255;
		g = 253;
		b = 208;	/* cream */
	}

	/* A line with NO speaker row is the SERVER talking -- a dedicated server's motd, or
	 * the operator's `say` -- so it is labelled as such. It used to print as "?", which
	 * reads as a player with a broken name. */
	snprintf(head, sizeof(head), "%s: ",
		(name != NULL && name[0] != '\0') ? name : "SERVER");

	segs[0].text = head;
	segs[0].r = (unsigned char)r;
	segs[0].g = (unsigned char)g;
	segs[0].b = (unsigned char)b;
	segs[0].ambient = 0;

	/* The message itself is a flat near-white, so the name is what carries the
	 * colour. Not `ambient`: chat does not dim with age the way status does. */
	segs[1].text = text;
	segs[1].r = 240;
	segs[1].g = 240;
	segs[1].b = 240;
	segs[1].ambient = 0;

	/* The SERVER's own voice carries its colour (a dedicated server's motd, said in
	 * the ink its config asks for): both runs then read in that one colour, so the
	 * line looks like a notice instead of like somebody talking. */
	if (ink != NULL)
	{
		segs[0].r = segs[1].r = ink[0];
		segs[0].g = segs[1].g = ink[1];
		segs[0].b = segs[1].b = ink[2];
	}

	jer_console_chat(segs, 2);
}


/* The pre-remap projection, kept only so the throttled debug line can show both
 * numbers - "did the widescreen remap fire, and by how much" is otherwise a
 * screenshot question. */
static int gMpTagRawX;

/* The other players' name, drawn ABOVE them: over the car's roof when they are
 * driving, over their head when they are on foot. Yaw-only projection (the
 * camera pitch is small enough not to matter for a label). */
static int MpProjectWorldToScreen(int wx, int wy, int wz, int* sx, int* sy, int* depth)
{
	int camx, camy, camz, camyaw;
	float yaw, s, c, dx, dy, dz, rx, rz, f;
	int ox, oy;

	MpCameraPose(&camx, &camy, &camz, &camyaw);

	yaw = (float)(camyaw & 0xfff) * (6.2831853f / 4096.0f);
	s = sinf(yaw);
	c = cosf(yaw);

	dx = (float)(wx - camx);
	/* The Y axis is NEGATED relative to X/Z: the engine stores the car's Y as
	 * pos.vy = -where.t[1] (cars.c DrawCar) and camera_position.vy = -jcam.where.t[1]
	 * (camera.c:616), so a world point's vertical offset from the camera is
	 * -wy - camy = -(wy + camy) in the (down-positive) GTE frame, and screen y
	 * follows that sign. Computing dy = wy + camy keeps the tag 320 units ABOVE the
	 * roof on screen instead of ~600 units too high. */
	dy = (float)(wy + camy);
	dz = (float)(wz - camz);

	/* Camera space is inv_camera_matrix = RotY(camyaw) (yaw only), matching the engine's
	 * LIBGTE RotMatrixY (LIBGTE.C:696): a point directly ahead maps to +z, and
	 *   cam_x = c*dx + s*dz   (right)
	 *   cam_z = -s*dx + c*dz  (forward)
	 * The first cut had these mirrored (c*dx - s*dz / s*dx + c*dz), which is RotY(-yaw):
	 * "in front of the camera" then read as "behind", so a nametag only survived the
	 * rz < 64 reject when its player was BEHIND you. */
	rx = dx * c + dz * s;
	rz = -dx * s + dz * c;

	if (rz < 64.0f || rz > 8000.0f)
	{
		if (MpDebugOn() && gMpCtx != NULL)
		{
			static unsigned long lastProjMs;

			if ((MpNowMs() - lastProjMs) > 2000)
			{
				lastProjMs = MpNowMs();
				gMpCtx->jer_log(gMpCtx,
					"[mp] nametag: reject depth cam=%d,%d,%d yaw=%d -> world %d,%d,%d rx=%.0f rz=%.0f\n",
					camx, camy, camz, camyaw, wx, wy, wz, rx, rz);
			}
		}
		return 0;		/* behind the camera / too far to be worth drawing */
	}

	f = 520.0f / rz;
	ox = 160 + (int)(rx * f);
	oy = 120 - (int)(dy * f);

	/* WIDESCREEN. Everything above projects into the MAPPED viewport, but
	 * PrintString draws in the 320x240 buffer, so at 16:9 the tag would sit off its
	 * car (further, the wider the aspect - the reported bug). Bring the projection
	 * back the way the engine brings the sun back (sky.c); identity at 4:3. */
	gMpTagRawX = ox;
	ox = jer_screen_remap_x(ox);

	/* The SIDES: the remapped x spans the whole visible width (0..320 is the edge
	 * of a 16:9 screen too), and a car whose centre has just left it still has half
	 * its body on screen. Culling at the buffer edge dropped the tag while the car
	 * was plainly visible, so keep it out to ~a car's half-width past the edge and
	 * pin it to the edge instead. */
	if (ox < -60 || ox > 380 || oy < 6 || oy > 232)
		return 0;

	if (ox < 6)
		ox = 6;
	else if (ox > 308)
		ox = 308;

	if (sx != NULL) *sx = ox;
	if (sy != NULL) *sy = oy;
	if (depth != NULL) *depth = (int)rz;

	return 1;
}

/* The single colour seam for a nametag. A future team/gamemode can claim the
 * colour here and every caller follows; until then a player's own colour wins,
 * and the default is white (readable against the world, unlike the game's
 * suit colours which are dark). */
static void MpNameTagColour(const MP_PLAYER* p, unsigned char* r, unsigned char* g, unsigned char* b)
{
	if (p->colorOn)
	{
		*r = (unsigned char)p->colorR;
		*g = (unsigned char)p->colorG;
		*b = (unsigned char)p->colorB;
	}
	else
	{
		*r = 250;
		*g = 250;
		*b = 250;
	}
}

/* Nametag constants: full-size near (the 0.275f default), half-size far. */
#define MP_TAG_SCALE_MAX	0.275f
#define MP_TAG_SCALE_MIN	0.138f

static void MpDrawNameTags(void)
{
	int k;

	for (k = 0; k < MP_MAX_PLAYERS; k++)
	{
		MP_PLAYER* p = &gMp.players[k];
		int wx, wy, wz, wh, sx, sy, depth;
		unsigned char r, g, b;
		float scale;
		char name[MP_NAME_MAX];

		if (!p->active || p->isLocal)
			continue;

		/* The anchor. A car's hd.where.t[1] is UP-positive; a pedestrian's
		 * position.vy is DOWN-positive (ground - 130). The engine converts with
		 * the negation hd.where.t[1] = -ped.vy (pedest.c ChangeCarPlayerToPed),
		 * so do the same here. */
		if (p->carId >= 0)
		{
			MpCarPose(p->carId, &wx, &wy, &wz, &wh);
			wy += 320;	/* float the label over the roof */
		}
		else if (p->pedLastMs != 0 && p->ped != NULL)
		{
			wx = p->pedX;
			wy = -p->pedY + 260;	/* over the head */
			wz = p->pedZ;
		}
		else
		{
			continue;	/* no car and no stand-in ped yet */
		}

		/* anchor-level trace, so "hook not firing" and "remote behind the camera"
		 * are distinguishable from the log */
		if (MpDebugOn() && gMpCtx != NULL)
		{
			static unsigned long lastAnchorMs;

			if ((MpNowMs() - lastAnchorMs) > 2000)
			{
				lastAnchorMs = MpNowMs();
				gMpCtx->jer_log(gMpCtx,
					"[mp] nametag: player %d anchor world %d,%d,%d\n",
					p->id, wx, wy, wz);
			}
		}

		if (!MpProjectWorldToScreen(wx, wy, wz, &sx, &sy, &depth))
			continue;

		MpNameTagColour(p, &r, &g, &b);

		/* Distance-scale the tag: full near, half-size far. */
		scale = 700.0f / (float)depth;
		if (scale > MP_TAG_SCALE_MAX)
			scale = MP_TAG_SCALE_MAX;
		else if (scale < MP_TAG_SCALE_MIN)
			scale = MP_TAG_SCALE_MIN;

		snprintf(name, sizeof(name), "%s", (p->name[0] != '\0') ? p->name : "?");

		/* Centre the name on the anchor: StringWidth is full-size (0.275f). */
		{
			int w = (int)((float)StringWidth(name) * (scale / 0.275f));

			SetTextColour(r, g, b);
			PrintStringHiresScaledSpaced(name, sx - w / 2, sy, scale, 0.0f);
		}

		/* one throttled line so "did the tag project and what did it look like"
		 * is answerable from the log rather than a screenshot */
		if (MpDebugOn() && gMpCtx != NULL)
		{
			static unsigned long lastTagMs;

			if ((MpNowMs() - lastTagMs) > 2000)
			{
				lastTagMs = MpNowMs();
				gMpCtx->jer_log(gMpCtx,
					"[mp] nametag: %s at screen %d,%d (raw x %d) depth %d scale %.3f colour %d,%d,%d\n",
					name, sx, sy, gMpTagRawX, depth, scale, r, g, b);
			}
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
		PrintString(gMp.notifyText[idx], 8, 172 + row * 10);	/* 6 px up with the chat block */
		row++;
	}

	MpDrawNameTags();

	/* The chat prompt: the line being typed, with a cursor. The ENGINE console
	 * draws it now, on its own bottom row (jer_console_input), which also keeps
	 * it visible while the console itself is toggled off -- what you are typing
	 * is never invisible. MpUiDrawOverlay runs from DRAW_OVERLAY, which the
	 * engine fires only while the world is stepping, so setting it here IS the
	 * proof it shows while driving. */
	if (gMp.chatOpen)
	{
		char line[MP_NOTIFY_TEXT_MAX + 2];

		snprintf(line, sizeof(line), "%s_", gMp.chatBuf);
		jer_console_input(line);

		if (gMpCtx != NULL && MpDebugOn())
		{
			static unsigned long lastMs;

			if ((MpNowMs() - lastMs) > 1000)
			{
				lastMs = MpNowMs();
				gMpCtx->jer_log(gMpCtx, "[mp] chat: prompt drawn (bottom-left, in game)\n");
			}
		}
	}
	else
	{
		jer_console_input(NULL);
	}

	return JER_RESULT_CONTINUE;
}

void MpChatOpen(void)
{
	if (gMpCtx != NULL && MpDebugOn())
		gMpCtx->jer_log(gMpCtx, "[mp] chat: prompt open (type; Enter to send, Esc to cancel)\n");

	gMp.chatOpen = 1;
	gMp.chatBuf[0] = '\0';
	gMp.chatSwallowText = 1;	/* the key that opened this also arrives as text */
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
	jer_frontend_register_menu(&gOnlineMenu);
	jer_frontend_register_menu(&kHostMenu);
	jer_frontend_register_menu(&kHostSetMenu);
	jer_frontend_register_menu(&gJoinMenu);
	jer_frontend_register_menu(&gLobbyMenu);
	jer_frontend_register_menu(&kJmpOptionsMenu);
	jer_frontend_register_menu(&gNameMenu);
	jer_frontend_register_menu(&gManualMenu);

	/* resolve the real registered index of each menu (see MpResolveMenus) */
	MpResolveMenus();

	/* Preflight the frontend character row once, so the log says whether Jericho
	 * is available even before the player opens JMP Options. This is the same
	 * probe the row itself uses; it loads Havana's model table if needed. */
	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] frontend character: Jericho %s\n",
			MpPedModelSelectable(MP_PED_MODEL_JERICHO) ? "selectable" : "unavailable");

	jer_frontend_set_main_entry("mp.root");

	/* gManualIp is the source of truth now, so there is nothing to render here --
	 * but the SEED still belongs at registration, so a discovered LAN game can fill
	 * the field in before the player ever opens it. */
	MpManualSeed();

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] registered %d frontend menus (main entry mp.root)\n",
			jer_frontend_menu_count());
}
