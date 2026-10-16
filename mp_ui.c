/*
 * mp_ui.c -- Multiplayer frontend menus, registered as REAL frontend screens
 * via jer_frontend.h (native buttons + navigation, no overlay).
 *
 * Menu tree:
 *   mp.root     LAN | Split-Screen | Back
 *   mp.lan      Host Game | Join Game | Options | Back
 *   mp.host     Take a Ride | Back
 *   mp.mode     Single Player | Multiplayer | Back          (the take-a-ride question)
 *   mp.join     <LAN servers...> / Manual IP / Back
 *   mp.lobby    <players...> / Start Match (host) or Leave / Back
 *   mp.options  Change Name | Enforce Mods | Port / Back
 *   mp.name     10 character slots / Done / Back
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
	M_OPTIONS,
	M_NAME,
	M_MANUAL,
	M_COUNT
};

/* Each mp menu's id, in the enum's order, and the index it ACTUALLY registered at
 * (-1 = not registered). */
static const char* const kMenuIds[M_COUNT] = {
	"mp.root", "mp.lan", "mp.online", "mp.host", "mp.mode", "mp.join",
	"mp.lobby", "mp.options", "mp.name", "mp.manual"
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
static int  gManualPort;
static int  gManualEdit;		/* the field owns the keyboard right now */
static int  gManualSeed;		/* seeded from a discovered host yet? */

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
static void LblEnforce(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Enforce Mods: < %s >", kModCheckNames[gMp.config.modCheck % 3]); }
static void LblStrict(void* ud, char* o, int n)  { (void)ud; snprintf(o, n, "Strict Version: < %s >", gMp.config.strictVersion ? "On" : "Off"); }
static void LblPort(void* ud, char* o, int n)    { (void)ud; snprintf(o, n, "Port: %d", gMp.config.port); }
static void LblManualIp(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Manual address: %s ...", gManualIp); }
static void LblManualGo(void* ud, char* o, int n) { (void)ud; snprintf(o, n, "Connect to %s", gManualIp); }

/* The field itself. It says it is taking keystrokes and shows a caret, so "why does
 * nothing happen when I press keys" answers itself. */
static void LblManualField(void* ud, char* o, int n)
{
	(void)ud;

	snprintf(o, n, "%s %s%s", gManualEdit ? "Address (typing):" : "Address:",
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

	/* asynchronous: the lobby shows "Connecting to <addr>..." while the
	 * socket connects, instead of the menu freezing for up to 5 s */
	if (s != NULL && MpBeginJoinAsync(s->ip, s->port))
		jer_frontend_open(gMenuIdx[M_LOBBY]);

	return 1;
}

/* "host" or "host:port" -> the two parts. Returns 1 on success, 0 if the text after
 * the colon is not a usable port -- so a typo is REFUSED rather than silently dialled
 * on the configured port. No colon at all leaves `*port` alone. */
static int MpManualParse(const char* text, char* host, size_t hostLen, int* port)
{
	const char* colon = strrchr(text, ':');
	size_t n;
	int p, d;

	if (colon == NULL)
	{
		if (text[0] == '\0')
			return 0;

		snprintf(host, hostLen, "%s", text);
		return 1;
	}

	n = (size_t)(colon - text);

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

	memcpy(host, text, n);
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
	int port = gManualPort > 0 ? gManualPort : gMp.config.port;

	(void)ud;

	if (!MpManualParse(gManualIp, host, sizeof(host), &port))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] manual address '%s' does not parse (want host or host:port)\n", gManualIp);

		jer_error("Use an address or host:port, not %s", gManualIp);
		return 1;
	}

	gManualPort = port;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] joining manual %s:%d\n", host, port);

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
	jer_frontend_open(gMenuIdx[M_OPTIONS]);
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
	{ "Split-Screen", NULL, NULL, ActSplitScreen, NULL, -1,    0 },
	{ "Back",         NULL, NULL, NULL,           NULL, -1,    1 },
};
static const JER_FE_MENU kRootMenu = { "mp.root", kRootItems, 4, NULL, NULL };

static JER_FE_ITEM kLanItems[] =
{
	{ "Host Game", NULL, NULL, ActHostGame, NULL, -1,    0 },
	{ "Join Game", NULL, NULL, ActJoinEnter, NULL, -1,    0 },
	{ "Options",   NULL, NULL, NULL,         NULL, -1, 0 },
	{ "Back",      NULL, NULL, NULL,         NULL, -1,        1 },
};
static const JER_FE_MENU kLanMenu = { "mp.lan", kLanItems, 4, NULL, NULL };

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

static JER_FE_ITEM kOptionsItems[] =
{
	{ "Change Name",    NULL, NULL, NULL,       NULL,       -1, 0 },
	{ NULL, LblEnforce, NULL, NULL, AdjEnforce, -1,       0 },
	{ NULL, LblStrict,  NULL, NULL, AdjStrict,  -1,       0 },
	{ NULL, LblPort,    NULL, NULL, AdjPort,    -1,       0 },
	{ "Back",           NULL, NULL, NULL,       NULL,      -1,     1 },
};
static const JER_FE_MENU kOptionsMenu = { "mp.options", kOptionsItems, 5, NULL, NULL };

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
	kLanItems[0].submenu = gMenuIdx[M_HOST];
	kLanItems[1].submenu = gMenuIdx[M_JOIN];
	kLanItems[2].submenu = gMenuIdx[M_OPTIONS];
	kOptionsItems[0].submenu = gMenuIdx[M_NAME];

	gMenusResolved = 1;
}

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

	/* manual address -- its own submenu: there is somewhere to TYPE here, and a
	 * DNS name would never fit on a single row with one adjust axis */
	gJoinItems[k].label = NULL;
	gJoinItems[k].get_label = LblManualIp;
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
 * The column offsets sit inside that box and the text is drawn at 55% of the
 * console's size, which is what fits five columns where one console line fits
 * one. Both numbers are answers a screenshot gives, not constants of nature. */
#define ONLINE_SCALE		55
#define ONLINE_COL_NAME		4
#define ONLINE_COL_PLAYERS	86
#define ONLINE_COL_MODE		112
#define ONLINE_COL_CITY		188
#define ONLINE_COL_PING		240
#define ONLINE_NAME_CHARS	15	/* what the NAME column holds before it is cut */

static JER_FE_ITEM gOnlineItems[JER_FE_MAX_ITEMS];
/* The heading is the screen's name, as the design calls for: it is the one piece
 * of this screen the engine draws (rows are ours). */
static JER_FE_MENU gOnlineMenu = { "mp.online", gOnlineItems, 0, NULL, NULL,
				   "JERICHO DISCOVERY SERVER" };
static int gOnlineRev = -1;		/* the list revision these rows were built from */
static char gOnlineApplied[160];	/* the master URL already handed to the client */

/* Hand the config's address to the fetcher, at most once per distinct value.
 * MpOnlineSetMaster RESOLVES, and this runs on a menu press -- the same place,
 * and for the same reason, that a join resolves its host. */
static void OnlineApplyConfig(void)
{
	if (strcmp(gOnlineApplied, gMp.config.masterUrl) == 0)
		return;

	snprintf(gOnlineApplied, sizeof(gOnlineApplied), "%s", gMp.config.masterUrl);
	MpOnlineSetMaster(gMp.config.masterUrl);
}

/* Why the list is empty. A player staring at an empty table should not have to
 * guess whether the address is unset, the fetch is still running, or it failed. */
static const char* OnlineStateText(void)
{
	switch (MpOnlineState())
	{
	case MP_ONLINE_OFF:			return "(no discovery server set)";
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
			MpModeName(s != NULL ? s->rules : 0), s != NULL ? s->city : -1);

	/* asynchronous, exactly like the LAN rows: the lobby shows progress while
	 * the socket connects instead of the menu freezing */
	if (s != NULL && MpBeginJoinAsync(s->host, s->port))
		jer_frontend_open(gMenuIdx[M_LOBBY]);

	return 1;
}

static void OnlineOnEnter(void* ud)
{
	int i, k = 0, count;

	(void)ud;

	OnlineApplyConfig();

	/* A fresh look every time the screen opens: a stale address is worse than
	 * none, and the fetch is asynchronous -- the rows appear as they arrive. */
	MpOnlineRefresh();

	count = MpOnlineCount();

	for (i = 0; i < count && k < JER_FE_MAX_ITEMS - 2; i++)
	{
		OnlineRowClear(k);
		gOnlineItems[k].userdata = (void*)(intptr_t)i;
		gOnlineItems[k].on_activate = ActJoinOnline;
		k++;
	}

	if (count == 0)
	{
		OnlineRowClear(k);
		gOnlineItems[k].label = OnlineStateText();
		k++;
	}

	/* Manual address: the way in for a host that is not listed (yet), or one
	 * behind a name the player already knows. */
	OnlineRowClear(k);
	gOnlineItems[k].get_label = LblManualIp;
	gOnlineItems[k].submenu = gMenuIdx[M_MANUAL];
	k++;

	OnlineRowClear(k);
	gOnlineItems[k].label = "Back";
	gOnlineItems[k].is_back = 1;
	k++;

	gOnlineMenu.item_count = k;

	/* what this list was built from, so MpUiTick rebuilds only on a real change */
	gOnlineRev = MpOnlineRevision();
}

/* The table. Called from the frontend draw hook (after the engine has drawn the
 * screen), and only while this menu is on screen. */
void MpUiDrawOnline(void)
{
	int count, servers, i, x, y, w, h;

	if (!gInFrontend || gMpCtx == NULL)
		return;

	if (!MpMenuIs(M_ONLINE) || gOnlineMenu.item_count <= 0)
		return;

	count = gOnlineMenu.item_count;
	servers = MpOnlineCount();

	/* One line the first time the table is actually painted with rows: it is the
	 * only thing an automated run can check about a screen whose whole point is
	 * what it LOOKS like (the pixels are checked by hand). */
	if (servers > 0 && MpDebugOn() && gMpCtx != NULL)
	{
		static int logged = 0;

		if (!logged)
		{
			logged = 1;
			gMpCtx->jer_log(gMpCtx,
				"[mp] online table drawn: %d server row(s) of %d, five columns at x=%d,%d,%d,%d,%d (scale %d)\n",
				servers, count, ONLINE_COL_NAME, ONLINE_COL_PLAYERS, ONLINE_COL_MODE,
				ONLINE_COL_CITY, ONLINE_COL_PING, ONLINE_SCALE);
		}
	}

	/* the column headings, one row pitch above the first row. Orange, like the
	 * engine's own menu headings, and passed at full brightness because the ink is
	 * halved on the way to the font (a "mid grey" 190 renders as 95 and reads as
	 * smudge next to the rows). */
	if (servers > 0 && jer_frontend_row_rect(0, count, &x, &y, &w, &h))
	{
		int hy = y - 16;

		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_NAME,    hy, "SERVER NAME", 255, 190, 90);
		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_PLAYERS, hy, "PLAYERS",     255, 190, 90);
		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_MODE,    hy, "GAMEMODE",    255, 190, 90);
		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_CITY,    hy, "CITY",        255, 190, 90);
		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_PING,    hy, "PING",        255, 190, 90);
	}

	for (i = 0; i < servers && i < count; i++)
	{
		const MP_ONLINE_SERVER* s = MpOnlineGet(i);
		char cell[24];
		char name[ONLINE_NAME_CHARS + 1];

		if (s == NULL || !jer_frontend_row_rect(i, count, &x, &y, &w, &h))
			continue;

		snprintf(name, sizeof(name), "%.*s", ONLINE_NAME_CHARS, s->name);
		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_NAME, y, name, 235, 235, 235);

		snprintf(cell, sizeof(cell), "%d/%d", s->players, s->maxPlayers);
		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_PLAYERS, y, cell, 235, 235, 235);

		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_MODE, y, MpModeName(s->rules), 235, 235, 235);
		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_CITY, y, MpCityLabel(s->city), 235, 235, 235);

		if (s->pingMs > 0)
			snprintf(cell, sizeof(cell), "%d", s->pingMs);
		else
			snprintf(cell, sizeof(cell), "-");	/* not measured yet */

		jer_text_draw_at(1, ONLINE_SCALE, x + ONLINE_COL_PING, y, cell, 235, 235, 235);
	}
}

static void WireDynamic(void)
{
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
		const char* want = getenv("MP_TEST_OPENMENU");

		if (want != NULL && menuTries < 0)
			menuTries = 60;		/* ~1 s at 60 fps */

		if (menuTries > 0 && gInFrontend && gMenusResolved)
		{
			int idx = jer_frontend_find(want);

			menuTries--;

			if (idx >= 0 && jer_frontend_current_menu() == idx)
			{
				menuTries = 0;	/* it took */
			}
			else if (idx >= 0)
			{
				jer_frontend_open(idx);
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

void MpConsoleChat(const char* name, int colorOn, int r, int g, int b, const char* text)
{
	char head[MP_NAME_MAX + 4];
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

	snprintf(head, sizeof(head), "%s: ", (name != NULL && name[0] != '\0') ? name : "?");

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

	if (ox < 6 || ox > 308 || oy < 6 || oy > 232)
		return 0;

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
		PrintString(gMp.notifyText[idx], 8, 178 + row * 10);
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
	jer_frontend_register_menu(&kOptionsMenu);
	jer_frontend_register_menu(&gNameMenu);
	jer_frontend_register_menu(&gManualMenu);

	/* resolve the real registered index of each menu (see MpResolveMenus) */
	MpResolveMenus();

	jer_frontend_set_main_entry("mp.root");

	/* gManualIp is the source of truth now, so there is nothing to render here --
	 * but the SEED still belongs at registration, so a discovered LAN game can fill
	 * the field in before the player ever opens it. */
	MpManualSeed();

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] registered %d frontend menus (main entry mp.root)\n",
			jer_frontend_menu_count());
}
