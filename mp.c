/*
 * mp.c -- JERICHO Multiplayer: module entry, config and player registry.
 *
 * A compiled-in deep mod (no `runtime` in mod.toml) so it can read/write the
 * game's globals (GameLevel, wantedTimeOfDay, Pads[], car_data[], player[]).
 *
 * Responsibilities here:
 *   - module registration + lifecycle hooks (BOOT / SHUTDOWN)
 *   - persistent settings (JERICHO/CONFIG/mp.ini): port, player name,
 *     host name, beacon interval, lobby mod-check mode
 *   - the network-player registry: which CAR_DATA slot each participant
 *     drives, so we can tell our players apart from genuine NPC traffic
 *
 * Transport lives in mp_net.c, the frontend overlay in mp_ui.c and the
 * session sync in mp_session.c.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_frontend.h"
#include "jer_pause_menu.h"
#include "jer_colour.h"		/* a player's suit colour is a canonical JER_COLOUR */
#include "jer_ped_palette.h"	/* a player's own Tanner in their own colour */
#include "jer_npc.h"		/* JerNpc: the stand-in we drew for a remote player */

/* The chat key needs PsyX's debug-key hook, but NOT its header: PsyX_public.h and
 * SDL_scancode.h both pull in math/SDL defines that collide with the game's in
 * this TU (a C4005 'M_PI': macro redefinition). So the one symbol is declared
 * here -- matching GameDebugKeysHandlerFunc, and C-linkage because PsyX_public.h
 * wraps the declaration in extern "C" -- and the two scancodes are named
 * constants (SDL2 USB-HID indices, fixed). */
typedef void (*MpDebugKeysFn)(int nKey, char down);
typedef void (*MpTextInputFn)(const char* buf);

#ifdef __cplusplus
extern "C" {
#endif
extern MpDebugKeysFn g_dbg_gameDebugKeys;
extern MpTextInputFn g_cfg_gameOnTextInput;
#ifdef __cplusplus
}
#endif

#define MP_KEY_CHAT_OPEN	23	/* SDL_SCANCODE_T */
#define MP_KEY_CHAT_SEND	40	/* SDL_SCANCODE_RETURN */
#define MP_KEY_CHAT_CANCEL	41	/* SDL_SCANCODE_ESCAPE */

#include "driver2.h"
#include "main.h"
#include "mission.h"
#include "pad.h"
#include "players.h"
#include "cars.h"
#include "camera.h"
#include "glaunch.h"
#include "state.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mp.h"
#include "mp_bot.h"	/* the test bot; testing only, inert unless MP_BOT is set */

MP_STATE gMp;
JERICHO_CONTEXT* gMpCtx;

/* Is MP_DEBUG set? Resolved ONCE and cached. The module asks this from the
 * per-frame, per-poll and per-message paths (and a getenv there is a libc call
 * per ask), while the environment cannot change once the game is running. This
 * is the ONLY place that reads it -- grep for one site. */
int MpDebugOn(void)
{
	static int resolved = 0;
	static int on = 0;

	if (!resolved)
	{
		const char* dbg = getenv("MP_DEBUG");

		on = (dbg != NULL);
		resolved = 1;
	}

	return on;
}

/* Boolean test levers that the per-frame paths read (the map census in MpOnFrame,
 * the pause census in the overlay). Resolved once, for the same reason: an
 * environment value cannot change while the game runs, and these are tested every
 * frame. `cache` is a module-owned int initialised to -1. */
static int MpLeverFlag(const char* name, int* cache)
{
	if (*cache < 0)
		*cache = (getenv(name) != NULL);

	return *cache;
}

static int gTestMapOn = -1;
static int gTestPauseOn = -1;

/* ------------------------------------------------------------------ */
/* Default player name                                                 */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */



/* ------------------------------------------------------------------ */
/* Player registry                                                     */
/* ------------------------------------------------------------------ */






/* Set when a lost connection must land the player on the main menu; the
 * engine decides where its own exit goes, so we jump on the next frontend
 * frame instead of trusting it. */
static int gReturnToMenu;

/* mp's own in-game player list. A network session must NOT open the engine
 * pause: that freezes the whole simulation on THIS machine while everyone else
 * keeps driving, so the two states disagree the moment play resumes. The START
 * press opens this instead -- a non-freezing overlay. */
static int gMpShowPlayers;

/* Leave the whole match: back to the main frontend with a notice. Used when
 * the host ends the game or the server connection is lost. */
void MpReturnToFrontend(void)
{
	/* The single choke point for "the session is over, go back to the frontend":
	 * log it here as well as at the callers, so any future path that returns to
	 * the frontend is visible in the log without having to remember to add one. */
	MpConnEvent("returning to the frontend", -1, "the session is over");

	MpSessionReset();
	MpResetPlayers();

	/* The session is over (a lost connection, a refusal, the host leaving):
	 * forget it completely, so nothing can launch into it again. Without this
	 * the player was left sitting in a menu whose START then began the DEFAULT
	 * city with no host -- the "dropped into Chicago with a vehicle" report. */
	MpClientDisconnect();
	gMp.role = MP_ROLE_NONE;
	gMp.connected = 0;
	gMp.running = 0;
	gMpShowPlayers = 0;	/* the overlay only belongs to a live match */

	/* the ENGINE's own way out of a gameplay session: it tears down the level,
	 * stops the music/sfx and returns to the frontend (the same path the pause
	 * menu's Exit takes). Do NOT just SetState -- that leaves sounds playing.
	 * The engine lands where it likes (often the middle of the stock chain that
	 * launched the level), so ask for the main menu explicitly. */
	if (!gInFrontend)
	{
		gReturnToMenu = 1;
		EndGame(GAMEMODE_QUIT);
	}
	else
	{
		jer_frontend_goto(0);	/* already in a menu: back to the main one */
	}
}
void MpCameraPose(int* x, int* y, int* z, int* yaw)
{
	if (x != NULL) *x = camera_position.vx;
	if (y != NULL) *y = camera_position.vy;
	if (z != NULL) *z = camera_position.vz;
	if (yaw != NULL) *yaw = camera_angle.vy;
}

/* Engine globals the frontend flow needs, kept here so the UI file does not
 * have to pull in the engine's type preamble. */
int MpGetGameLevel(void)
{
	return GameLevel;
}

void MpSetSubGame(int n)
{
	gSubGameNumber = n;
}

/* The stock city screen was confirmed. While a LAN session is being set up we
 * take it over: ask Singleplayer or Multiplayer BEFORE the stock flow
 * continues to time-of-day/car select. `defer` stops the stock continuation
 * and we open our mode menu instead. */
static int MpOnFrontendConfirm(void* userdata, void* args)
{
	JER_ARGS_FRONTEND* fe = (JER_ARGS_FRONTEND*)args;

	(void)userdata;

	if (gMp.role != MP_ROLE_NONE && !gMp.running && fe->defer == 0)
	{
		fe->defer = 1;
		MpUiOpenModeMenu();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] city confirmed - asking Single Player / Multiplayer\n");
	}

	return JER_RESULT_CONTINUE;
}

/* A client join is live only once the host has welcomed us: without that a
 * frontend START must NOT launch a level (there would be no host to play
 * with, and it used to start the default city instead). */
static int MpClientSessionLive(void)
{
	return gMp.role == MP_ROLE_CLIENT && gMp.connected &&
		gMp.localPlayerId >= 0 && MpJoinState() == MP_JOIN_READY;
}

/* The frontend's START GAME press. When we are hosting a LAN session we take
 * it over: the stock city/map/time/car screens the host just walked through
 * have filled in the level globals, so adopt those and start the match
 * (broadcast + local launch) instead of the single-player state change. */
static int MpOnMpFrontend(void* userdata, void* args)
{
	JER_ARGS_MP_FRONTEND* fe = (JER_ARGS_MP_FRONTEND*)args;

	(void)userdata;

	if (fe->action != JER_MP_FE_START)
		return JER_RESULT_CONTINUE;

	if (gMp.role == MP_ROLE_HOST)
	{
		/* the host starts the match once; a later press has nothing to do */
		if (!gMp.running)
		{
			gMp.city = GameLevel;

			if (wantedTimeOfDay >= 0)
				gMp.timeOfDay = wantedTimeOfDay;
			if (wantedWeather >= 0)
				gMp.weather = wantedWeather;

			MpStartMatch();
		}

		fe->claimed = 1;
		return JER_RESULT_CONTINUE;
	}

	if (gMp.role == MP_ROLE_CLIENT)
	{
		/* This must NOT be gated on !gMp.running.
		 *
		 * Joining a game that is already in progress sets running the moment the
		 * WELCOME lands -- that is what "a live game" means -- so a gate on
		 * !running refused the press, the stock start-game path ran instead, and
		 * GameType was still 0. 0 is GAME_MISSION, and glaunch.c's
		 * `case GAME_MISSION: RunMissionLadder(1)` is the mission ladder: the
		 * client launched Undercover mission 1 instead of joining the session.
		 * The session state that matters here is the JOIN state, not running. */
		if (MpClientSessionLive())
		{
			MpClientLaunch();
		}
		else
		{
			jer_error("Not connected to a server");

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] START ignored: no live session\n");
		}

		fe->claimed = 1;
		return JER_RESULT_CONTINUE;
	}

	return JER_RESULT_CONTINUE;
}




/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */






/* ------------------------------------------------------------------ */
/* Lifecycle hooks                                                     */
/* ------------------------------------------------------------------ */

static int gAutoHostStart;	/* MP_AUTOSTART=host: auto-launch when N players are in */
static int gAutoHostFrames;
static int gAutoHostTarget = 2;	/* players (incl. us) to wait for before starting */
static int gAutoHostSettle;	/* frames the target has been met for */
static int gHostInWorld;	/* the host actually reached the running level */

extern int gBootSuppressLevel;	/* main.c: a module takes the level launch over */
extern void MpNoteLocalPad(int pad);	/* mp_session.c: the pad driving our own car */

/* The host quit the match? Only once it has really BEEN in the world: at the
 * start of a match `running` is set a frame or two before gInFrontend clears,
 * and keying on `running && gInFrontend` alone would tear the fresh session
 * down during that transition. */
static int MpHostLeftMatch(void)
{
	if (gMp.role != MP_ROLE_HOST || !gMp.running)
	{
		gHostInWorld = 0;	/* not hosting a live match */
		return 0;
	}

	if (!gInFrontend)
	{
		gHostInWorld = 1;	/* we are in the level */
		return 0;
	}

	return gHostInWorld;		/* in the frontend AGAIN -> it left */
}

/* -host [port] / -join <ip>[:port] command-line shortcuts (JER_EVENT_CMDLINE),
 * the argv equivalent of MP_AUTOSTART. Handy for launching two instances on
 * one machine during development. */
static int MpOnCmdLine(void* userdata, void* args)
{
	JER_ARGS_CMDLINE* cl = (JER_ARGS_CMDLINE*)args;
	int i;

	(void)userdata;

	if (cl == NULL || cl->argc <= 1)
		return JER_RESULT_CONTINUE;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] cmdline: %d arg(s)\n", cl->argc);

	/* Log the whole line, not just the count: a wrong argument is invisible
	 * otherwise, and the game's own -help text (which prints on -help/-h/--help/-?)
	 * is the fastest way to see the flags it knows. */
	if (gMpCtx != NULL)
	{
		char line[256];
		int k, used = 0;

		line[0] = 0;

		for (k = 0; k < cl->argc && used < (int)sizeof(line) - 4; k++)
		{
			int wrote = snprintf(line + used, sizeof(line) - (size_t)used, "%s%s",
				k > 0 ? " " : "", cl->argv[k] != NULL ? cl->argv[k] : "(null)");

			if (wrote <= 0)
				break;

			used += wrote;
		}

		gMpCtx->jer_log(gMpCtx, "[mp] cmdline: %s\n", line);
	}


	for (i = 1; i < cl->argc; i++)
	{
		if (!strcmp(cl->argv[i], "-host"))
		{
			if (i + 1 < cl->argc && cl->argv[i + 1][0] != '-')
				gMp.config.port = atoi(cl->argv[++i]);

			if (gMp.config.port < 1024 || gMp.config.port > 65535)
				gMp.config.port = MP_DEFAULT_PORT;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] -host on port %d\n", gMp.config.port);

			/* -host means: bring a match up by itself, no frontend walking. The car
			 * comes from -mpcar; the CITY is adopted later, at match start. -level
			 * only stores gBootLevel (a static in main.c) and GameLevel is assigned
			 * after the argument loop -- so reading GameLevel here still sees the
			 * default and the host launched Chicago while -level said havana. */
			gMp.autoSession = 1;
			gAutoHostStart = 1;
			gAutoHostTarget = 2;

			/* Take the level launch over: the session loads the level when the
			 * match starts, so the engine's own -level entry must not fire. Booting
			 * one here and another at the start is what made a match "restart with
			 * different weather" the moment a client joined -- and let the
			 * frontend's city win over the session's. */
			gBootSuppressLevel = 1;

			MpBeginHost();
			if (gMp.timeOfDay < 0 && wantedTimeOfDay >= 0)
				gMp.timeOfDay = wantedTimeOfDay;
			if (gMp.weather < 0 && wantedWeather >= 0)
				gMp.weather = wantedWeather;
		}
		else if (!strcmp(cl->argv[i], "-mpcar"))
		{
			/* Our own vehicle for the match. The engine's -car cannot be used
			 * here: it is a dependent option that requires -level, and -level
			 * boots straight into a city -- which must never be given to a
			 * joining client. */
			if (i + 1 < cl->argc && cl->argv[i + 1][0] != '-')
			{
				const char* v = cl->argv[++i];
				const char* sel = v;
				int city = -1;

				/* Optional CITY prefix: `-mpcar 1:9` is HAVANA's model 9 (a
				 * cross-city car -- the point of the prefix, since the same number
				 * is a different vehicle in another city). Without it the pick is
				 * the session's own city. */
				{
					const char* q = v;

					while (*q >= '0' && *q <= '9')
						q++;

					if (*q == ':' && q > v)
					{
						city = atoi(v);

						if (city < 0 || city > 3)
							city = -1;

						sel = q + 1;
					}
				}

				/* Accept either a raw model number or "slotN" (1..10), the
				 * frontend's own per-city slot. A slot is resolved against the
				 * session's city at launch (see MpLaunchLocal), because GameLevel is
				 * still the default here -- reading carNumLookup now would use the
				 * wrong city, the same trap the -host comment above records. */
				if (strncmp(sel, "slot", 4) == 0)
				{
					gMp.config.car = atoi(sel + 4);
					gMp.config.carIsSlot = 1;
					city = -1;	/* a slot is always the session's city */
				}
				else
				{
					gMp.config.car = atoi(sel);
					gMp.config.carIsSlot = 0;
				}

				gMp.config.carCity = city;

				/* Make the choice durable. Without this the slot-ness is never
				 * written, so on the next launch `car` would be read back as a
				 * MODEL number (see MpConfigLoad). */
				MpConfigSave();

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] -mpcar %s (car=%d slot=%d city=%d)\n",
						v, gMp.config.car, gMp.config.carIsSlot, gMp.config.carCity);
			}
		}
		else if (!strcmp(cl->argv[i], "-join"))
		{
			char ip[64];
			int port = gMp.config.port;

			snprintf(ip, sizeof(ip), "127.0.0.1");

			if (i + 1 < cl->argc && cl->argv[i + 1][0] != '-')
			{
				const char* s = cl->argv[++i];
				const char* colon = NULL;
				const char* q;

				/* find the LAST ':' by hand rather than with strrchr: the
				 * game's PSX string shim declares strrchr with a C++ signature,
				 * so calling it from here emits an external the CRT cannot
				 * satisfy (LNK2001 on ?strrchr@@...) */
				for (q = s; *q != '\0'; q++)
				{
					if (*q == ':')
						colon = q;
				}

				if (colon != NULL)
				{
					size_t n = (size_t)(colon - s);

					if (n > 0 && n < sizeof(ip))
					{
						memcpy(ip, s, n);
						ip[n] = '\0';
					}

					port = atoi(colon + 1);
				}
				else
				{
					snprintf(ip, sizeof(ip), "%s", s);
				}
			}

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] -join %s:%d\n", ip, port);

			/* MP_TEST_FRONTEND_JOIN keeps the MENUS: without it a join is the
			 * unattended path ("no menus: launch as soon as we are in"), a client
			 * never reaches the vehicle select, and "does a client reach the car
			 * screen and its roster?" is unanswerable headlessly. With it the
			 * client takes the route a human takes: WELCOME -> MpUiOpenCarSelect
			 * (screen 14) -> the menu -> Ride, which hands the launch back through
			 * JER_EVENT_MP_FRONTEND. See MpTestFrontendJoin. */
			{
				const char* feJoin = MpTestFrontendJoin();
				int menuJoin = (feJoin != NULL && feJoin[0] != '\0' && feJoin[0] != '0');

				if (!menuJoin)
					gMp.autoSession = 1;	/* no menus: launch as soon as we are in */
				else if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] -join %s:%d WITH the menus (MP_TEST_FRONTEND_JOIN): the car screen will be offered\n",
						ip, port);
			}

			MpBeginJoinAsync(ip, port);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* Headless dev affordance: MP_AUTOSTART=host[:PORT] | join[:IP[:PORT]]
 * brings a session up at boot so the transport can be exercised without
 * the frontend menus (mirrors the cainescrossfire scripted debug driver). */
static void MpAutostartFromEnv(void)
{
	const char* a = getenv("MP_AUTOSTART");

	if (a == NULL || a[0] == '\0')
		return;

	if (strncmp(a, "host", 4) == 0)
	{
		if (a[4] == ':')
			gMp.config.port = atoi(a + 5);

		if (gMp.config.port < 1024 || gMp.config.port > 65535)
			gMp.config.port = MP_DEFAULT_PORT;

		gMpCtx->jer_log(gMpCtx, "[mp] autostart host on port %d\n", gMp.config.port);
		MpBeginHost();

		if (getenv("MP_AUTOJOIN_START") != NULL)
		{
			const char* t = getenv("MP_AUTOJOIN_START");

			gAutoHostStart = 1;
			gAutoHostTarget = (t != NULL && t[0] != '\0') ? atoi(t) : 2;
			if (gAutoHostTarget < 2)
				gAutoHostTarget = 2;
		}
	}
	else if (strncmp(a, "join", 4) == 0)
	{
		char ip[64];
		int port = gMp.config.port;

		snprintf(ip, sizeof(ip), "%s", "127.0.0.1");

		if (a[4] == ':')
		{
			const char* s = a + 5;
			const char* colon = strchr(s, ':');

			if (colon != NULL)
			{
				int iplen = (int)(colon - s);

				if (iplen > 0 && iplen < (int)sizeof(ip))
				{
					memcpy(ip, s, (size_t)iplen);
					ip[iplen] = '\0';
				}

				port = atoi(colon + 1);
			}
			else if (s[0] != '\0')
			{
				snprintf(ip, sizeof(ip), "%s", s);
			}
		}

		gMpCtx->jer_log(gMpCtx, "[mp] autostart join %s:%d\n", ip, port);
		MpBeginJoinAsync(ip, port);
	}
}

/* ------------------------------------------------------------------ */
/* Chat: opening the prompt from the keyboard                         */
/* ------------------------------------------------------------------ */
/* The pad is busy driving, so chat opens on a KEYBOARD key. PsyX hands a module
 * a raw scancode through g_dbg_gameDebugKeys -- the only such hook -- and it is a
 * single slot, so we CHAIN whatever was there rather than clobber it; a module
 * that installed its own handler keeps working.
 *
 * `T` is chosen because the default PsyX keyboard map does not bind it to a pad
 * button (X/V/Z/C, LSHIFT, the arrows, SPACE and RETURN are all taken), so
 * opening chat does not also steer the car. ESCAPE closes the prompt again. */
static MpDebugKeysFn gMpPrevDebugKeys = NULL;
static MpTextInputFn gMpPrevTextInput = NULL;

static void MpOnTextInput(const char* text);	/* our handler; defined below */

/* Take/release PsyX's single text-input slot, CHAINING whoever had it. Idempotent
 * on purpose: the frame hook re-asserts it every frame, so any path that forgot
 * to release self-heals instead of leaving the keyboard grabbed. */
static void MpChatGrabKeyboard(int on)
{
	if (on)
	{
		if (g_cfg_gameOnTextInput != MpOnTextInput)
		{
			gMpPrevTextInput = g_cfg_gameOnTextInput;
			g_cfg_gameOnTextInput = MpOnTextInput;
		}
	}
	else if (g_cfg_gameOnTextInput == MpOnTextInput)
	{
		g_cfg_gameOnTextInput = gMpPrevTextInput;
		gMpPrevTextInput = NULL;
	}
}

/* PsyX hands every typed character here while the prompt is up. NULL is the
 * BACKSPACE key (PsyX's convention for it -- there is no character). Everything
 * else appends, up to the buffer, terminator included. */
static void MpOnTextInput(const char* text)
{
	if (!gMp.chatOpen)
	{
		/* not ours: pass it on untouched */
		if (gMpPrevTextInput != NULL)
			gMpPrevTextInput(text);
		return;
	}

	if (text == NULL)		/* backspace: drop the last character */
	{
		size_t n = strlen(gMp.chatBuf);

		if (n > 0)
			gMp.chatBuf[n - 1] = '\0';
		return;
	}

	/* Enter may arrive here as text on some backends; treat it as SEND. */
	if (text[0] == '\r' || text[0] == '\n')
	{
		MpChatSendText(gMp.chatBuf);
		MpChatGrabKeyboard(0);
		return;
	}

	{
		size_t have = strlen(gMp.chatBuf);
		size_t room = sizeof(gMp.chatBuf) - 1 - have;
		size_t i;

		for (i = 0; text[i] != '\0' && i < room; i++)
			gMp.chatBuf[have + i] = text[i];

		gMp.chatBuf[have + i] = '\0';
	}
}

static void MpOnDebugKey(int nKey, char down)
{
	if (gMpPrevDebugKeys != NULL)
		gMpPrevDebugKeys(nKey, down);

	if (!down)
		return;

	/* In a live match only: while the frontend is up the engine's own text fields
	 * (and every other module) want the keyboard. */
	if (!MpIsActive() || !gMp.running)
		return;

	if (nKey == MP_KEY_CHAT_OPEN && !gMp.chatOpen)
	{
		MpChatOpen();
		MpChatGrabKeyboard(1);	/* the prompt owns the keyboard while it is up */
	}
	else if (nKey == MP_KEY_CHAT_SEND && gMp.chatOpen)
	{
		/* Enter sends: the owner echoes the line and the host fans it out. */
		MpChatSendText(gMp.chatBuf);
		MpChatGrabKeyboard(0);
	}
	else if (nKey == MP_KEY_CHAT_CANCEL && gMp.chatOpen)
	{
		gMp.chatOpen = 0;
		gMp.chatBuf[0] = '\0';
		MpChatGrabKeyboard(0);
	}
}

static int MpOnBoot(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	/* Chat needs the keyboard while the pad drives: take PsyX's one debug-key slot,
	 * CHAINING any existing handler so nothing else is starved. */
	gMpPrevDebugKeys = g_dbg_gameDebugKeys;
	g_dbg_gameDebugKeys = MpOnDebugKey;

	MpNetStart();

	gMpCtx->jer_log(gMpCtx, "[mp] multiplayer ready (port %d, name '%s', firstRun=%d, build %04x, mods %04x)\n",
		gMp.config.port, gMp.config.playerName, !gMp.config.firstNameSet,
		MpBuildHash(), MpModHash());

	MpAutostartFromEnv();

	return JER_RESULT_CONTINUE;
}

static int MpOnShutdown(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	MpLeaveSession();
	MpNetShutdown();

	return JER_RESULT_CONTINUE;
}

/* Service the sockets every frame (frontend and in-game both fire FRAME),
 * and again at the top of the world step for the lockstep hand-off. */
static void MpLogPlayerList(void);

static int MpOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	MpNetPoll(0);
	MpUiTick();

	/* MP_DEBUG: this hook runs whether or not the world is stepping, so its own
	 * counter can tell "the sim stopped" from "the module stopped". gMp.frame is
	 * incremented by the lockstep, which is exactly what is in question. */
	{
		static unsigned long ticks;

		if (MpDebugOn() && gMpCtx != NULL && (++ticks % 120) == 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] framehook: tick %lu mpframe %lu running %d connected %d frontend %d\n",
				ticks, gMp.frame, gMp.running, gMp.connected, gInFrontend);
	}

	/* MP_DEBUG: echo the engine's notice ROWS, so the WRAP can be checked from
	 * the log without eyes on the screen -- a wrapped message is several
	 * entries, one per drawn line. Log-only, so it cannot change behaviour. */
	if (MpDebugOn() && gMpCtx != NULL)
	{
		static int lastNotices = -1;
		int n = jer_error_count();

		if (n != lastNotices)
		{
			int k;

			lastNotices = n;

			for (k = 0; k < n; k++)
				gMpCtx->jer_log(gMpCtx, "[mp] notice[%d] = '%s'\n", k, jer_error_at(k));
		}
	}

	/* Deferred work, done on a FRAME rather than inside the poll that noticed it
	 * -- calling into the state machine from a message handler is re-entrant and
	 * crashed the client. FRAME and not PRE_SIM: a joining client is sitting in
	 * the FRONTEND, and PRE_SIM only runs in game, so the request was set and
	 * then never acted on. */
	/* Chat owns PsyX's single text-input slot exactly while the prompt is up.
	 * Re-asserted every frame so a missed release self-heals. */
	MpChatGrabKeyboard(gMp.chatOpen ? 1 : 0);

	/* Test lever: MP_TEST_CHATKEY=<secs> feeds the chat KEY into our own handler
	 * once, that many seconds after the match goes live, so the open path can be
	 * exercised with no keyboard (the harness has none). Inert unless set. */
	{
		static unsigned long fireAtMs = 0;
		const char* s = MpTestChatKey();

		if (s != NULL && gMpCtx != NULL)
		{
			if (fireAtMs == 0 && gMp.running)
				fireAtMs = MpNowMs() + (unsigned long)(atoi(s) * 1000);

			if (fireAtMs != 0 && fireAtMs != (unsigned long)-1 && MpNowMs() >= fireAtMs)
			{
				const char* comma;

				fireAtMs = (unsigned long)-1;	/* once */
				gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_CHATKEY -> chat key press\n");
				MpOnDebugKey(MP_KEY_CHAT_OPEN, 1);

				/* MP_TEST_CHATKEY=<secs>,<text>: type the text through the real
				 * character handler, then one BACKSPACE, and log what landed. */
				comma = strchr(s, ',');
				if (comma != NULL)
				{
					const char* t = comma + 1;

					for (; *t != '\0'; t++)
					{
						char one[2];

						one[0] = *t;
						one[1] = '\0';
						MpOnTextInput(one);
					}

					MpOnTextInput(NULL);	/* backspace */
					gMpCtx->jer_log(gMpCtx, "[mp] test: chat buffer now '%s'\n", gMp.chatBuf);

					gMpCtx->jer_log(gMpCtx, "[mp] test: chat SEND\n");
					MpOnDebugKey(MP_KEY_CHAT_SEND, 1);
				}
			}
		}
	}

	if (gMp.pendingLaunch)
	{
		gMp.pendingLaunch = 0;

		if (gMp.autoSession && MpJoinState() == MP_JOIN_READY)
			MpClientLaunch();
	}

	/* a peer with no car joined a live match: build it here, on a frame */
	if (gMp.pendingSpawn)
	{
		gMp.pendingSpawn = 0;

		/* BOTH sides: a client can also defer a remote car it could not name at
		 * level init (the level's mission header is not parsed yet) and build it
		 * here once the level is up. */
		if (gMp.running)
			MpSpawnLateJoiners();
	}

	/* Test lever: dump the player list, so the roster/player-row formatting can
	 * be exercised without a human opening any screen.
	 *
	 * MP_MAP used to force the map open -- the same mistake the pause lever made:
	 * the engine then draws a map whose state was never set up, and the screen
	 * goes red. It only logs now. */
	if (MpLeverFlag("MP_MAP", &gTestMapOn) && gMp.running)
		MpLogPlayerList();

	/* A connection loss asked for the main menu: take it as soon as the engine
	 * is back in the frontend, so the player cannot be left in the middle of
	 * the menu chain that started the match. */
	if (gReturnToMenu && gInFrontend)
	{
		gReturnToMenu = 0;
		jer_frontend_goto(0);
	}

	/* The host quit the match: it is back in the frontend while the session
	 * is still live, so tell the clients and tear everything down. */
	if (MpHostLeftMatch())
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] host left the match - notifying clients\n");

		MpHostByeAll();
		MpLeaveSession();
	}

	/* A LAN session is one player per machine. The stock frontend's
	 * multiplayer chain leaves NumPlayers at 2 for its split-screen flow,
	 * which makes the car select ask for a SECOND player's car -- hold it at
	 * 1 for the whole host/join flow (the log line tells us if this was the
	 * cause). */
	if (gMp.role != MP_ROLE_NONE && !gMp.running && NumPlayers != 1)
	{
		NumPlayers = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] LAN flow: forced NumPlayers back to 1\n");
	}

	/* MP_AUTOSTART=host: launch once MP_AUTOJOIN_START players are in (a
	 * straggler would otherwise be rejected: a running game is INPROGRESS),
	 * or after ~30 s regardless so a short-handed test still starts. */
	if (gAutoHostStart && !gMp.running)
	{
		++gAutoHostFrames;

		if (gMp.playerCount >= gAutoHostTarget)
			++gAutoHostSettle;
		else
			gAutoHostSettle = 0;

		if ((gAutoHostSettle > 60) || gAutoHostFrames > 900)
		{
			gAutoHostStart = 0;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] autostart: starting match with %d player(s) (target %d)\n",
					gMp.playerCount, gAutoHostTarget);

			MpStartMatch();
		}
	}

	/* diagnostic: where the module thinks every player car is, and whether the
	 * engine is actually simulating it (list= is membership of the engine's
	 * own active_car_list, which StepCars walks). MP_DEBUG-gated: it is a scan
	 * plus a 12-field line per player every 2s, for a dev only. */
	if (MpDebugOn() && gMp.running && gMpCtx != NULL && (gMp.frame % 60) == 0)
	{
		int i;

		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			MP_PLAYER* p = &gMp.players[i];

			if (!p->active || p->carId < 0)
				continue;

			{
				CAR_DATA* cd = &car_data[p->carId];
				int inList = 0, k;

				for (k = 0; k < num_active_cars; k++)
				{
					if (active_car_list[k] == cd)
						inList = 1;
				}

				gMpCtx->jer_log(gMpCtx, "[mp] pose: player %d local=%d car %d at %d,%d,%d spd=%d ct=%d pad=%d hnd=%d vy=%d list=%d/%d\n",
					p->id, p->isLocal, p->carId,
					cd->hd.where.t[0], cd->hd.where.t[1], cd->hd.where.t[2],
					cd->hd.speed,
					cd->controlType,
					cd->ai.padid != NULL ? *cd->ai.padid : -99,
					cd->hndType,
					cd->st.n.linearVelocity[1],
					inList, num_active_cars);
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

/* In-game the DRAW_OVERLAY event fires on every drawn frame -- including while
 * the game is PAUSED, where StepSim (and so JER_EVENT_FRAME) does not run.
 * Servicing the socket here is what keeps a paused session alive: we keep
 * sending our keepalive and keep draining the peer's. */
extern void SetTextColour(unsigned char Red, unsigned char Green, unsigned char Blue);
extern int  PrintString(char* string, int x, int y);
extern int  gDrawPauseMenus;

/* A live match must keep simulating while the pause menu is up, but the engine
 * freezes the world (pauseflag blocks its StepSim). StepSim is an EXPORTED
 * engine symbol, so we drive the world ourselves from the overlay hook, which
 * fires on every drawn frame including while paused. */
extern void StepSim(void);
extern int  pauseflag;
extern void UnPauseSound(void);	/* the engine pauses music/sfx with the world */

/* While the pause menu is up, list who is connected down the left side.
 *
 * The host leads the list and is cyan, because it is the one machine that owns
 * the session. Everyone else follows in ascending player id -- the same order the
 * car slots are handed out in, so the list reads the way the match is built. Each
 * row is the player's name and index, the vehicle they are in (-1 = on foot) and
 * their round trip as the host measures it. */
/* The same rows the pause menu draws, without drawing anything: enough to check
 * names, slots and ping from a log, and safe to call from anywhere. */
static void MpLogPlayerList(void)
{
	int id;

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		CAR_DATA* cp;
		int veh = -1;

		if (p == NULL)
			continue;

		if (p->carId >= 0 && p->carId < MAX_CARS)
		{
			cp = &car_data[p->carId];

			if (cp->controlType != CONTROL_TYPE_NONE)
				veh = cp->ap.model;
		}

		if (gMpCtx != NULL)
		{
			MP_PEER_STATS st;
			char net[72];

			if (!p->isLocal && MpPeerStats(p->id, &st) && st.linkMs > 0)
			{
				unsigned long rxS = st.rxBytes * 1000UL / st.linkMs;
				unsigned long txS = st.txBytes * 1000UL / st.linkMs;

				snprintf(net, sizeof(net), "%d ms  rx %luB/s  tx %luB/s  loss %d%%",
					st.pingMs, rxS, txS, st.lossPct);
			}
			else
			{
				snprintf(net, sizeof(net), "%d ms", p->pingMs);
			}

			gMpCtx->jer_log(gMpCtx, "[mp] list: (build %04x mods %04x) %s #%d  car %d  slot %d  %s%s\n",
				MpBuildHash(), MpModHash(),
				p->name, p->id, veh, p->carId, net, p->isHost ? "  (host)" : "");
		}
	}
}

static void MpDrawPlayerList(void)
{
	int id, row = 0;

	/* Top-LEFT corner and HIGH up: the pause menu's items are drawn around the
	 * middle of the screen, and the row grew a delivery column, so the list needs
	 * the room. (Was x=8, y=56, then (4,24) -- still crowded the menu.) */
	SetTextColour(170, 170, 170);
	PrintString((char*)"-- PLAYERS --", 4, 6);

	/* The build identity, right under the title. These are the SAME numbers the
	 * startup log prints and the ones strict_version compares, so two players can
	 * confirm at a glance that they are on the same build (a mismatch is the most
	 * common cause of "we don't see each other"). */
	{
		char ident[72];

		snprintf(ident, sizeof(ident), "build %04x  mods %04x",
			MpBuildHash(), MpModHash());

		SetTextColour(140, 140, 140);
		PrintString(ident, 4, 14);
	}

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		CAR_DATA* cp;
		char line[160];
		char net[72];
		int veh = -1;
		int y;

		if (p == NULL)
			continue;

		/* -1 means on foot: either we have no car slot for them, or the car is
		 * standing there with nobody driving it */
		if (p->carId >= 0 && p->carId < MAX_CARS)
		{
			cp = &car_data[p->carId];

			if (cp->controlType != CONTROL_TYPE_NONE)
				veh = cp->ap.model;
		}

		{
			MP_PEER_STATS st;

			if (!p->isLocal && MpPeerStats(p->id, &st) && st.linkMs > 0)
			{
				unsigned long rxS = st.rxBytes * 1000UL / st.linkMs;
				unsigned long txS = st.txBytes * 1000UL / st.linkMs;

				/* stall/delivery %, not "packet loss": the fraction of
				 * frames in which nothing arrived from them. */
				snprintf(net, sizeof(net), "%d ms  rx %luB/s  tx %luB/s  loss %d%%",
					st.pingMs, rxS, txS, st.lossPct);
			}
			else
			{
				snprintf(net, sizeof(net), "%d ms  (local)", p->pingMs);
			}

			/* TWO lines per player -- name/vehicle, then the link readout
			 * indented under it. The screen is only ~40 characters wide, so
			 * one row carrying the name AND the rates AND the loss would run
			 * off the right edge (which is exactly what it did). */
			snprintf(line, sizeof(line), "%s #%d  car %d", p->name, p->id, veh);
		}

		y = 26 + row * 20;
		row++;

		if (p->isHost)
			SetTextColour(0, 255, 255);	/* the host, cyan */
		else
			SetTextColour(200, 200, 200);

		PrintString(line, 4, y);

		SetTextColour(150, 150, 150);
		PrintString(net, 10, y + 10);

		/* so the list can be checked without eyes on the screen */
		if (MpDebugOn() && gMpCtx != NULL)
		{
			static unsigned long lastListMs;

			if ((MpNowMs() - lastListMs) > 2000)
			{
				lastListMs = MpNowMs();
				gMpCtx->jer_log(gMpCtx, "[mp] list: (build %04x mods %04x) %s | %s\n",
					MpBuildHash(), MpModHash(), line, net);
			}
		}
	}
}

static int MpOnDrawOverlay(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	if (gMp.role == MP_ROLE_NONE)
		return JER_RESULT_CONTINUE;

	MpNetPoll(0);

	/* A LIVE MATCH DOES NOT FREEZE FOR THE PAUSE MENU. The engine freezes the world
	 * while its pause menu is open (pauseflag) and does not run JER_EVENT_FRAME --
	 * where our net tick lives -- while frozen, and it pauses the audio with it.
	 * So while a session is up and paused we step the world, run our own tick, and
	 * undo the audio pause once on the way in. The menu is still the engine's. */
	{
		static int wasPaused;

		if (gMp.running && pauseflag != 0)
		{
			if (!wasPaused)
			{
				wasPaused = 1;
				UnPauseSound();
			}

			StepSim();
			MpLockstepFrame();
		}
		else
		{
			wasPaused = 0;
		}
	}

	/* the player list / MP options ride on the engine's pause menu */
	gMpShowPlayers = (gMp.running && pauseflag != 0) ? 1 : 0;

	/* MP_PAUSE logs the list for a test, and deliberately does NOT force the
	 * engine's pause flag. It used to set gDrawPauseMenus = 1 every frame, which
	 * makes the engine draw pause menus whose state was never set up -- a wild
	 * pointer, and an access violation. Forcing another subsystem's state from a
	 * test lever is not a shortcut, it is a new bug.
	 *
	 *     draw  = what the player sees (needs the pause menu actually open)
	 *     log   = the rows, which is what the test is checking anyway */
	if (MpLeverFlag("MP_PAUSE", &gTestPauseOn) && gMp.running)
		MpLogPlayerList();

	if (gDrawPauseMenus || gMpShowPlayers)
		MpDrawPlayerList();

	/* Bottom-left HOST / CLIENT tag, always on while a session exists: with two
	 * windows side by side on one machine (or two machines) there is otherwise no
	 * way to tell which is which -- and the test is precisely about the two roles
	 * behaving differently. */
	{
		const char* who = MpIsHost() ? "HOST" :
			(gMp.role == MP_ROLE_CLIENT ? "CLIENT" : "MP");
		char conn0[120];
		char conn1[120];

		if (MpIsHost())
			SetTextColour(0, 255, 255);
		else
			SetTextColour(255, 200, 0);

		PrintString((char*)who, 8, 226);

		/* What the connection is DOING, right here on screen -- the line to read
		 * out when something looks wrong on one machine only: which peer, how
		 * long it has been in its current stage, and what happened last. Two
		 * short lines because one long one runs off a 320 px screen. MpConnEvent
		 * logs this same text, so it can also simply be grepped for. */
		MpConnLineText(conn0, sizeof(conn0), 0);
		MpConnLineText(conn1, sizeof(conn1), 1);

		if (conn0[0] != 0)
		{
			SetTextColour(190, 190, 190);
			PrintString(conn0, 8, 208);

			if (conn1[0] != 0)
				PrintString(conn1, 8, 217);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* JERICHO-HOOK: the engine pause freezes the simulation -- which in a network
 * session freezes only THIS machine. Everyone else keeps driving, so the moment
 * play resumes the two disagree. In a live match the START press must therefore
 * not open it: claim the press (JER_RESULT_STOP, so the engine pause never
 * opens and pauseflag is never set) and toggle our own non-freezing player list.
 * A press while a session is only being set up, and stock single-player, keep
 * the normal pause. */
/* ------------------------------------------------------------------ */
/* The MP pause menu.                                                  */
/*                                                                     */
/* Small on purpose for now: it exists so the one thing you always      */
/* want when something goes wrong -- a written-down account of what the */
/* connection was doing -- is one button away, and so the per-player    */
/* options later have a home that is not bolted onto the engine's own   */
/* menu. jer_pause_menu_register collects this under "Modules".         */
static int MpMenuWriteDiag(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;

	MpDiagDump("asked for from the pause menu");
	MpNotify("...mp_diag.txt written next to the game");

	return JER_PAUSE_QUIT_NONE;
}

/* ------------------------------------------------------------------ */
/* A player's character, in that player's colour                       */
/*                                                                    */
/* The engine recolours the CLUT rows for ONE ped at a time, and its   */
/* selection PERSISTS until it is changed -- so this has to speak for   */
/* every ped it is asked about, including saying "stock colours" for   */
/* one that is not ours. Getting that wrong is how a mod ends up       */
/* painting a civilian.                                                */
/*                                                                    */
/* `on` is off by default: off means the character keeps exactly the    */
/* colours the game gave it, which is also what a ped we do not own     */
/* gets.                                                               */
static int MpOnPedDraw(void* userdata, void* args)
{
	JER_ARGS_PED_DRAW* a = (JER_ARGS_PED_DRAW*)args;
	int i, on = 0, r = 0, g = 0, b = 0;

	(void)userdata;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	/* No ped at all: clear, because the last selection sticks. */
	if (a->ped == NULL)
	{
		jer_ped_palette_select(-1);
		return JER_RESULT_CONTINUE;
	}

	if ((void*)a->ped == MpLocalPedPtr())
	{
		/* ours: the config is the truth, not the network table */
		on = gMp.config.colorOn;
		r = gMp.config.colorR;
		g = gMp.config.colorG;
		b = gMp.config.colorB;
	}
	else
	{
		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			MP_PLAYER* p = &gMp.players[i];

			if (!p->active || p->ped == NULL)
				continue;

			if ((void*)p->ped == a->ped)
			{
				on = p->colorOn;
				r = p->colorR;
				g = p->colorG;
				b = p->colorB;
				break;
			}
		}
	}

	if (!on)
	{
		jer_ped_palette_select(-1);
		return JER_RESULT_CONTINUE;
	}

	/* The player's suit colour in JERICHO's canonical colour space (jer_colour.h):
	 * the config / wire carries r,g,b, and jer_colour_make clamps and names it so
	 * this draw path never has to think about the engine's two packings. Each
	 * distinct colour gets its own palette rows; identical colours share them. */
	jer_ped_palette_select(jer_ped_palette_team_colour(jer_colour_make(r, g, b), MP_COLOR_STRENGTH));

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Player options: the colour editor                                   */
/*                                                                    */
/* One toggle and three sliders. The toggle comes first because it is  */
/* the important one: OFF (the default) keeps the game's own colours,  */
/* and a player who never touches any of this sees nothing change.     */
#define MP_COLOR_STEP 16

static void MpColorLabelOn(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Custom colour: %s", gMp.config.colorOn ? "ON" : "OFF (original)");
}

static int MpColorToggleOn(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gMp.config.colorOn = gMp.config.colorOn ? 0 : 1;
	MpConfigSave();

	return 0;
}

static void MpColorLabelR(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Red: %d", gMp.config.colorR);
}

static void MpColorLabelG(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Green: %d", gMp.config.colorG);
}

static void MpColorLabelB(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Blue: %d", gMp.config.colorB);
}

static int MpColorAdjust(int* which, int dir)
{
	*which += dir * MP_COLOR_STEP;

	if (*which < 0)
		*which = 0;
	if (*which > 255)
		*which = 255;

	MpConfigSave();

	return 0;
}

static int MpColorAdjustR(void* ud, int dir) { (void)ud; return MpColorAdjust(&gMp.config.colorR, dir); }
static int MpColorAdjustG(void* ud, int dir) { (void)ud; return MpColorAdjust(&gMp.config.colorG, dir); }
static int MpColorAdjustB(void* ud, int dir) { (void)ud; return MpColorAdjust(&gMp.config.colorB, dir); }

/* ------------------------------------------------------------------ */
/* Change car: the Multiplayer pause page's vehicle picker             */
/* ------------------------------------------------------------------ */
/* Two round-robin rows and an apply row, the same shape as mpColorMenu. A row is
 * a cycler, not a list, because the pause menu's item set is fixed when the page
 * is built and a module menu cannot nest a second level (jer_pause_menu.h) -- so
 * a long roster is reached by cycling, not by listing.
 *
 * The CARS are a city's frontend roster, read from the same two engine arrays the
 * stock car screen and carhacks' own picker use: CarAvailability[city][slot] says
 * whether the slot is offered, carNumLookup[city][slot] is its MODEL NUMBER.
 *
 * The CITIES come from carhacks when it is installed (a session can legitimately
 * mix cities' car data; that is what carhacks is for) and from the session's own
 * city alone when it is not. */
#define MPCC_MAX_CITIES	6
#define MPCC_MAX_CARS	12
#define MPCC_SLOTS	10		/* frontend slots per city (carNumLookup[city][0..9]) */

static int mpCcCities[MPCC_MAX_CITIES];
static int mpCcCityCount;
static int mpCcCityIdx;
static int mpCcModels[MPCC_MAX_CARS];
static int mpCcModelCount;
static int mpCcModelIdx;

/* Which city the picker is showing: an index into mpCcCities. -1 (or an empty
 * list) means the session's own city. */
static int MpCcCity(void)
{
	if (mpCcCityCount <= 0)
		return gMp.city;

	if (mpCcCityIdx < 0 || mpCcCityIdx >= mpCcCityCount)
		mpCcCityIdx = 0;

	return mpCcCities[mpCcCityIdx];
}

/* Fill the roster for the currently selected city. Safe to call any time; called
 * when the page opens and after the city row changes. */
static void MpCcRebuild(void)
{
	extern int CarAvailability[4][10];
	extern char carNumLookup[4][10];
	int city, slot;

	/* The city list: carhacks' answer when it has one, else just the session
	 * city. Rebuilt every time so a carhacks set that lands mid-session shows up. */
	{
		int was = MpCcCity();
		int n;

		mpCcCityCount = 0;
		n = MpCarQueryCities(mpCcCities, MPCC_MAX_CITIES);

		if (n <= 0)
		{
			mpCcCities[0] = (gMp.city >= 0 && gMp.city < 4) ? gMp.city : 0;
			mpCcCityCount = 1;
			mpCcCityIdx = 0;
		}
		else
		{
			mpCcCityCount = (n > MPCC_MAX_CITIES) ? MPCC_MAX_CITIES : n;
			mpCcCityIdx = 0;

			/* keep showing the same city across a rebuild when we still can */
			for (slot = 0; slot < mpCcCityCount; slot++)
			{
				if (mpCcCities[slot] == was)
				{
					mpCcCityIdx = slot;
					break;
				}
			}
		}
	}

	city = MpCcCity();
	mpCcModelCount = 0;

	for (slot = 0; slot < MPCC_SLOTS && mpCcModelCount < MPCC_MAX_CARS; slot++)
	{
		/* `== 0` is carhacks' and the stock screen's own test for "not offered
		 * in this city"; -1 is a slot the level has no car for. */
		if (CarAvailability[city][slot] == 0)
			continue;

		mpCcModels[mpCcModelCount++] = (int)(unsigned char)carNumLookup[city][slot];
	}

	if (mpCcModelIdx < 0 || mpCcModelIdx >= mpCcModelCount)
		mpCcModelIdx = 0;
}

static void MpCcLabelCity(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "City: %s", MpCarCityName(MpCcCity()));
}

static int MpCcAdjustCity(void* ud, int dir)
{
	(void)ud;

	if (mpCcCityCount > 0)
		mpCcCityIdx = (mpCcCityIdx + dir + mpCcCityCount) % mpCcCityCount;

	mpCcModelIdx = 0;
	MpCcRebuild();		/* the new city brings its own cars */

	return JER_PAUSE_QUIT_NONE;
}

static void MpCcLabelCar(void* ud, char* out, int max)
{
	(void)ud;

	if (mpCcModelCount <= 0)
	{
		snprintf(out, max, "Car: (no cars offered for this city)");
		return;
	}

	snprintf(out, max, "Car: %d/%d  (%s model %d)",
		mpCcModelIdx + 1, mpCcModelCount,
		MpCarCityName(MpCcCity()), mpCcModels[mpCcModelIdx]);
}

static int MpCcAdjustCar(void* ud, int dir)
{
	(void)ud;

	if (mpCcModelCount > 0)
		mpCcModelIdx = (mpCcModelIdx + dir + mpCcModelCount) % mpCcModelCount;

	return JER_PAUSE_QUIT_NONE;
}

static int MpCcApply(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	if (mpCcModelCount <= 0)
		return JER_PAUSE_QUIT_NONE;

	MpChangeCar(MpCcCity(), mpCcModels[mpCcModelIdx]);

	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM mpChangeCarItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */
	{ NULL, MpCcLabelCity, MpCcAdjustCity, NULL, NULL, 1 },
	{ NULL, MpCcLabelCar, MpCcAdjustCar, NULL, NULL, 1 },
	{ "Respawn as this car", NULL, MpCcApply, NULL, NULL, 0 },
};

/* A JER_PAUSE_MENU's item_count, taken FROM the array.
 *
 * The count is a separate field and the engine builds the submenu EAGERLY, so a
 * count larger than the array walks off its end -- the documented way opening the
 * Multiplayer page access-violates. Deriving it makes that unrepresentable rather
 * than something a later edit has to remember. */
#define MP_MENU_ITEMS(a)	(int)(sizeof(a) / sizeof((a)[0]))

static const JER_PAUSE_MENU mpChangeCarMenu =
{ "Change car", mpChangeCarItems, MP_MENU_ITEMS(mpChangeCarItems) };

static const JER_PAUSE_MENU_ITEM mpColorItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */
	{ NULL, MpColorLabelOn, MpColorToggleOn, NULL, NULL, 0 },
	{ NULL, MpColorLabelR, MpColorAdjustR, NULL, NULL, 1 },
	{ NULL, MpColorLabelG, MpColorAdjustG, NULL, NULL, 1 },
	{ NULL, MpColorLabelB, MpColorAdjustB, NULL, NULL, 1 },
};

static const JER_PAUSE_MENU mpColorMenu =
{ "My colour", mpColorItems, MP_MENU_ITEMS(mpColorItems) };

static int MpOnPauseMenu(void* userdata, void* args)
{
	JER_ARGS_PAUSE_MENU* pm = (JER_ARGS_PAUSE_MENU*)args;

	(void)userdata;

	if (pm == NULL || pm->action != JER_PAUSE_OPEN)
		return JER_RESULT_CONTINUE;

	/* While the chat prompt is up, START is the player typing RETURN -- claim it,
	 * so the engine pause does not open over the prompt. */
	if (gMp.chatOpen)
		return JER_RESULT_STOP;

	if (!gMp.running)
		return JER_RESULT_CONTINUE;

	/* Let the ENGINE open its pause menu -- do not claim START. The world keeps
	 * running because MpOnDrawOverlay steps it while paused, and our player list
	 * (and MP options) ride on top of the menu the player expects to see. */
	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] pause menu opened; the world keeps running\n");

	/* Refresh the Change car picker now, not while it is being drawn: the roster
	 * reads the city list (which a carhacks set may have changed since the last
	 * time the menu was opened) and the city's frontend car list. */
	MpCcRebuild();

	return JER_RESULT_CONTINUE;
}

static int MpOnPreSim(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	/* MP_DEBUG: is this hook still being called at all? A session whose sim silently
	 * stops here looks alive (polls continue on the FRAME hook, remote cars keep
	 * being adopted) while nothing owns a car any more. */
	if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % 120) == 0)
		gMpCtx->jer_log(gMpCtx,
			"[mp] presim: frame %lu running %d connected %d frontend %d\n",
			gMp.frame, gMp.running, gMp.connected, gInFrontend);

	if (gMp.running)
		MpLockstepFrame();
	else if (gMp.listenersUp || gMp.connected)
		MpNetPoll(0);

	return JER_RESULT_CONTINUE;
}


/* Apply each player car's input. Our own car may be driven by the test bot
 * (mp_bot.c); a remote car gets the pad its owner replicated. */
static int MpOnNetInput(void* userdata, void* args)
{
	JER_ARGS_NET_INPUT* in = (JER_ARGS_NET_INPUT*)args;
	MP_PLAYER* p;
	int carId;

	(void)userdata;

	if (!gMp.running)
		return JER_RESULT_CONTINUE;

	carId = (int)((CAR_DATA*)in->car - car_data);
	p = MpGetPlayerByCar(carId);

	if (p != NULL)
	{
		if (p->isLocal)
		{
			if (MpBotEnabled())
			{
				if ((gMp.frame % 60) == 0 && gMpCtx != NULL)
				{
					CAR_DATA* me = &car_data[carId];
					gMpCtx->jer_log(gMpCtx,
						"[mp] both: local %d,%d,%d spd=%d | remote(1) %d,%d,%d spd=%d\n",
						me->hd.where.t[0], me->hd.where.t[1], me->hd.where.t[2], me->hd.speed,
						car_data[1].hd.where.t[0], car_data[1].hd.where.t[1], car_data[1].hd.where.t[2], car_data[1].hd.speed);
				}

				in->pad = MpBotPadForLocalCar();		/* the test bot drives OUR car (mp_bot.c) */
				in->handled = 1;
			}

			/* Whatever is driving our car -- the player's pad or the test bot's --
			 * is what we replicate. See MpLocalPad. */
			MpNoteLocalPad(in->pad);
		}
		else if (!p->isLocal)
		{
			/* The owner's per-frame state is authoritative and is ADOPTED in
			 * MpHandleCarState, so the engine must not also drive this car from a
			 * replicated pad -- the two would fight. Replicated input is kept only
			 * as a FALLBACK for a snapshot gap: if we have not adopted this car for
			 * a while, let the engine carry it on the input we last had, so a brief
			 * stall coasts instead of freezing. */
			if (gMp.frame - p->lastStateFrame > MP_INPUT_FALLBACK_FRAMES)
			{
				in->pad = MpInputForPlayer(p->id);
				in->handled = 1;

				if (MpDebugOn() && (gMp.frame % 60) == 0 && gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] netinput: car %d <- player %d pad %#x (fallback)\n",
						carId, p->id, in->pad);
			}
			else
			{
				in->pad = 0;	/* adopted this frame: hands off, let the owner win */
				in->handled = 1;
			}
		}
		/* local car, no bot: leave the stock pad (handled stays 0) */
	}
	else if ((gMp.frame % 300) == 0 && gMpCtx != NULL)
	{
		gMpCtx->jer_log(gMpCtx, "[mp] netinput: car %d has no player row\n", carId);
	}

	return JER_RESULT_CONTINUE;
}

/* Drive OUR OWN PEDESTRIAN with the test bot, exactly as MpOnNetInput drives our
 * car: JER_EVENT_PED_INPUT fires right before ProcessTannerPad, so writing the
 * pad here IS a pad press. It exists because the on-foot path otherwise has no
 * motion at all in a headless run -- a Tanner standing at the spawn point tests
 * the messages, not the walking, and a peer's stand-in is only ever seen
 * standing still. */
/* The pause menu's ANSWER, in a live match.
 *
 * JERICHO hands a module the action before the engine runs it (JER_EVENT_GAME_QUIT,
 * fired in main.c), and that hook is what makes a multiplayer Restart possible at
 * all: the stock one calls EndGame(GAMEMODE_RESTART) and rebuilds the level, which
 * in a match takes every OTHER player's session down with it -- one player's
 * "restart" is not theirs to do to everyone.
 *
 * So mp CLAIMS restart and does the soft reset instead: this player back at the
 * level's own start, in their car, repaired, wanted level cleared, session intact.
 * Claiming means the engine runs none of its endings, so we also unpause -- the
 * pause menu has already closed itself by the time this fires.
 *
 * Quit and the rest of the codes are left alone: leaving the match is a real thing
 * a player should be able to do from the menu. */
static int MpOnGameQuit(void* userdata, void* args)
{
	JER_ARGS_GAME_QUIT* q = (JER_ARGS_GAME_QUIT*)args;

	(void)userdata;

	if (q == NULL || !gMp.running)
		return JER_RESULT_CONTINUE;

	/* The JER_PAUSE_QUIT_* values mirror the engine's menu quit codes
	 * (jer_pause_menu.h), and this event carries the engine's. */
	if (q->code != JER_PAUSE_QUIT_RESTART)
		return JER_RESULT_CONTINUE;

	if (!MpSoftRestart())
		return JER_RESULT_CONTINUE;

	pauseflag = 0;
	UnPauseSound();

	return JER_RESULT_STOP;
}

static int MpOnPedInput(void* userdata, void* args)
{
	JER_ARGS_PED_INPUT* in = (JER_ARGS_PED_INPUT*)args;

	(void)userdata;

	if (in == NULL || !gMp.running || !MpBotEnabled())
		return JER_RESULT_CONTINUE;

	/* Only OUR player's pad. player[0] is always us -- every machine runs its one
	 * local player in engine slot 0 and the remote players live in the higher
	 * slots the module inits (see MpFollowLocalCar). */
	if ((PLAYER*)in->player != &player[0])
		return JER_RESULT_CONTINUE;

	in->pad = MpBotTannerPad();

	return JER_RESULT_CONTINUE;
}

/* After a level starts, log the player cars so the spawn is verifiable. */
/* JERICHO-HOOK: the frontend's idle timer.
 *
 * The frontend boots the attract demo after ~30 s without input, and a host
 * sitting in its lobby waiting for players is idle by definition. The demo
 * launched a level nobody asked for; that load blocks the main thread for its
 * whole duration, so every player who was joining went quiet, hit the idle
 * timeout and dropped -- leaving the host alone in a demo it never asked for.
 *
 * Suppress it for as long as a session or a lobby exists. role is NONE for stock
 * single-player and for the stock split-screen path, which keep their demo. */
static int MpOnFrontendIdle(void* userdata, void* args)
{
	JER_ARGS_FRONTEND_IDLE* idle = (JER_ARGS_FRONTEND_IDLE*)args;

	(void)userdata;

	if (idle != NULL && gMp.role != MP_ROLE_NONE)
		idle->suppress = 1;

	return JER_RESULT_CONTINUE;
}

/* JERICHO-HOOK: the multiplayer map.
 *
 * The stock drawer loops `for (i = 0; i < NumPlayers; i++)` and calls
 * DrawPlayerDot(pos, -dir, ...) -- a blip carrying BOTH position and facing.
 * We hold NumPlayers at 1 so the renderer stays single-view, so that loop only
 * ever draws our own blip and nobody else appears on the map at all.
 *
 * Draw the same blip for every other player here, in the engine's own walk of
 * colours, so a full-screen player can see where everyone is and which way
 * they are pointing. */
extern void WorldToMultiplayerMap(VECTOR* in, VECTOR* out);
extern void DrawPlayerDot(VECTOR* pos, short rot, u_char r, u_char g, u_char b, int flags);


static int MpOnGameStart(void* userdata, void* args)
{
	int i;

	(void)userdata;
	(void)args;

	if (!MpIsActive() || gMpCtx == NULL)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];
		CAR_DATA* cp;

		if (!p->active || p->carId < 0)
			continue;

		cp = &car_data[p->carId];
		gMpCtx->jer_log(gMpCtx, "[mp] car: player %d slot %d controlType=%d apmodel=%d carmodel=%d want=%d loaded=%d pos=%d,%d,%d\n",
			p->id, p->carId, cp->controlType, cp->ap.model,
			(cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS) ? residentCarModels[cp->ap.model] : -1,
			(PlayerStartInfo[p->carId] != NULL) ? PlayerStartInfo[p->carId]->model : -9,
			(cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS && gCarCleanModelPtr[cp->ap.model] != NULL) ? 1 : 0,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2]);
	}

	/* The host used to broadcast a "meeting point" ('JPSW') here and every
	 * machine lined up on it. RETIRED: both machines already place both cars at
	 * the engine's own deterministic spawn, and a client gathers itself beside
	 * the host in MpHandleCarState (see ARCHITECTURE section 4). */

	return JER_RESULT_CONTINUE;
}

/* Diagnostics: log inbound addon-bridge payloads when MP_DEBUG is set. */
static int MpOnNetRecv(void* userdata, void* args)
{
	JER_ARGS_NET_RECV* r = (JER_ARGS_NET_RECV*)args;

	(void)userdata;

	if (MpDebugOn() && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] bridge recv '%s' from peer %d (%d bytes)\n",
			(r->channel != NULL) ? r->channel : "?", r->peer, r->len);

	return JER_RESULT_CONTINUE;
}

/* The level-launch hook: apply the host's time of day and weather. */
static int MpOnLevelLaunch(void* userdata, void* args)
{
	JER_ARGS_LEVEL_LAUNCH* l = (JER_ARGS_LEVEL_LAUNCH*)args;

	(void)userdata;

	if (MpIsActive())
	{
		l->timeOfDay = gMp.timeOfDay;
		l->weather = gMp.weather;
	}

	return JER_RESULT_CONTINUE;
}

static const JER_PAUSE_MENU_ITEM mpPauseItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */
	{ "Change car", NULL, NULL, NULL, &mpChangeCarMenu, 0 },
	{ "My colour", NULL, NULL, NULL, &mpColorMenu, 0 },
	{ "Write diagnostics now", NULL, MpMenuWriteDiag, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU mpPauseMenu =
{ "Multiplayer", mpPauseItems, MP_MENU_ITEMS(mpPauseItems) };

JER_MODULE_ENTRY(jer_module_mp_entry)(JERICHO_CONTEXT* ctx)
{
	gMpCtx = ctx;

	/* No crash DIALOGS, process-wide.
	 *
	 * The engine already writes JERICHO.dmp when it faults, which is the useful
	 * part. What it also does is let Windows raise a modal "has stopped working"
	 * box -- and that box blocks the process from exiting. On a machine testing
	 * over the network that is worse than useless: the crash looks like a hang,
	 * anything waiting on the process waits forever, and a run that was supposed
	 * to be unattended needs a human to click OK. Same dump, no dialog. */
	MpSuppressCrashDialogs();

	ctx->jer_register_module(ctx, "mp", "Multiplayer", "0.1.0", "JERICHO",
		"LAN multiplayer: host/join, UDP discovery, JERICHO addon net bridge, "
		"owner-authoritative car sync (each machine drives its own car and "
		"broadcasts its state; peers adopt it verbatim), and in-match chat.",
		"", JERICHO_SDK_VERSION);

	memset(&gMp, 0, sizeof(gMp));
	gMp.role = MP_ROLE_NONE;
	gMp.localPlayerId = 0;

	MpConfigLoad();
	MpResetPlayers();
	MpUiInit();		/* register the native frontend menus */

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, MpOnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_SHUTDOWN, MpOnShutdown, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, MpOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, MpUiDrawOverlay, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, MpOnDrawOverlay, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_MP_FRONTEND, MpOnMpFrontend, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND, MpOnFrontendConfirm, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CMDLINE, MpOnCmdLine, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PRE_SIM, MpOnPreSim, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_NET_RECV, MpOnNetRecv, NULL, 100);
	ctx->jer_register_hook(ctx, JER_EVENT_LEVEL_LAUNCH, MpOnLevelLaunch, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_NET_SPAWN, MpOnNetSpawn, NULL, 0);
	/* The engine's OWN car-to-car contact. Registered here (and not read from a
	 * proximity probe on some later frame) because this fires while the pair's
	 * pre-impact velocities are still in place, which is the only moment the
	 * closing speed can be read before our own engine absorbs it. */
	ctx->jer_register_hook(ctx, JER_EVENT_COLLISION, MpOnCarContact, NULL, 0);
	/* The resident car models: a match asks for enough distinct cars to seat every
	 * player. Registered at the default priority -- nothing else here competes for
	 * the level's spare resident slots. */
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DATA_SOURCE, MpOnCarDataSource, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_NET_INPUT, MpOnNetInput, NULL, 0);
	/* The same bot, for the on-foot player: JER_EVENT_PED_INPUT is the car's
	 * NET_INPUT for Tanner (the engine fires it right before ProcessTannerPad). */
	ctx->jer_register_hook(ctx, JER_EVENT_PED_INPUT, MpOnPedInput, NULL, 0);
	/* The pause menu's action, before the engine runs it: a live match turns
	 * Restart into a soft reset rather than a level reload (see MpOnGameQuit). */
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_QUIT, MpOnGameQuit, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, MpOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_MAP, MpOnDrawMap, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_IDLE, MpOnFrontendIdle, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, MpOnPauseMenu, NULL, 0);

	/* A player's own character, in their own colour -- as a DEFAULT. Priority
	 * -1000 makes us run FIRST among the ped-palette consumers, so mp selects a
	 * player's preferred colour and any mod that selects afterwards (at a normal
	 * priority) wins for the characters it claims. mp carries a preference, not a
	 * last word. */
	ctx->jer_register_hook(ctx, JER_EVENT_PED_DRAW, MpOnPedDraw, NULL, -1000);

	/* Our own page in the pause screen ("Modules" -> "Multiplayer"). NOTE: the
	 * declared item_count must equal the array length -- the engine builds the
	 * submenu eagerly, so a count that is too large reads past the end. */
	jer_pause_menu_register(&mpPauseMenu);
}
