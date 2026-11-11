/*
 * core/mp_frontend.c -- the launch and frontend flow.
 *
 * The stock city screen is confirmed here (Single Player / Multiplayer
 * question), the frontend's START GAME press is claimed for host and client,
 * the -host/-join/-mpcar command line and MP_AUTOSTART environment shortcuts
 * are parsed, and the remaining frontend hooks (entered / idle / draw) are
 * served. This is the module's "how a match gets from a menu to a running
 * level" half, before mp_session.c takes over the wire.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_frontend.h"
#include "jer_pause_menu.h"
#include "jer_colour.h"
#include "jer_ped_palette.h"
#include "jer_npc.h"
#include "jer_carexplode.h"
#include "jer_carglow.h"
#include "jer_carquery.h"
#include "jer_sound.h"

#include "driver2.h"
#include "system.h"		/* CITY_COUNT: the city registry the -mpcar city prefix checks */
#include "main.h"
#include "pause.h"
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
#include "jer_console.h"

/* The engine's map-shape boot flags (main.c, set by -mp). The host's own
 * Single Player / Multiplayer answer writes them through MpSetMapShape. */
extern int gBootMpLevel;	/* 1 = a multiplayer map, 0 = the full city */
extern int gBootMpArena;	/* which multiplayer map (0..MP_ARENA_MAX) */

/* The stock city screen was confirmed. Ask Singleplayer or Multiplayer BEFORE the
 * stock flow continues to time-of-day/car select -- for the LAN setup AND for a
 * plain singleplayer take-a-ride alike, since the answer also picks the MAP
 * (full city vs the city's multiplayer arena).
 *
 * TWO ways in, so this must be a DISJUNCTION. The GameType alone cannot carry it:
 * measured on the real host flow (Multiplayer -> LAN -> Host Game -> the city
 * screen), the confirm arrives with `gameType == GAME_MISSION` (0) -- the very
 * value the mission/cutscene screen carries. Gating on `gameType ==
 * GAME_TAKEADRIVE` alone therefore silently dropped the LAN path into single
 * player, and the guard stayed green because `MP_TEST_CITYCONFIRM` hardcodes
 * GAME_TAKEADRIVE. A session being set up is the reliable LAN signal
 * (MpBeginHost leaves `gMp.role != MP_ROLE_NONE`); a plain take-a-ride still
 * arrives as GAME_TAKEADRIVE. The mission path has neither -- no role, not
 * take-a-ride -- so it stays left alone.
 *
 * `defer` stops the stock continuation and we open our mode menu instead. */
int MpOnFrontendConfirm(void* userdata, void* args)
{
	JER_ARGS_FRONTEND* fe = (JER_ARGS_FRONTEND*)args;

	(void)userdata;

	if ((gMp.role != MP_ROLE_NONE || fe->gameType == GAME_TAKEADRIVE) &&
	    !gMp.running && fe->defer == 0)
	{
		fe->defer = 1;

		/* NOT MpUiOpenModeMenu() from in here: an open issued from inside this hook
		 * does not survive the engine finishing the screen switch, so the prompt
		 * silently never appeared. Arm it; the next frame opens it (see
		 * gModeMenuArmed in mp_ui.c). */
		MpUiArmModeMenu();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] city confirmed - asking Single Player / Multiplayer\n");
	}
	else if (gMpCtx != NULL)
	{
		/* Why the prompt did NOT open. Before this the only evidence was the
		 * ABSENCE of the line above -- which is the same evidence as "this hook
		 * never fired at all", and those are different bugs: a screen the hook
		 * does not serve (no session and not take-a-ride), versus a hook that is
		 * not registered. `role` is in here because it is half of the gate, and
		 * `gameType` ALONE is what made the LAN path look like a mission. */
		gMpCtx->jer_log(gMpCtx, "[mp] city confirmed - NOT asking (gameType=%d role=%d running=%d defer=%d)\n",
			fe->gameType, (int)gMp.role, (int)gMp.running, (int)fe->defer);
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
int MpOnMpFrontend(void* userdata, void* args)
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

			/* The stock per-city multiplayer level list picks the ARENA into
			 * gSubGameNumber (its buttons are SetVariable code 8, and only
			 * "Multiplayer" reaches that list). Take the arena from there so the
			 * session loads the map the player actually chose; the Single Player
			 * answer already set the shape to the full city (MpSetMapShape(0,0)),
			 * for which this is a no-op. */
			if (gBootMpLevel)
				gBootMpArena = gSubGameNumber & MP_SESSION_ARENA_MASK;

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
/* Lifecycle hooks                                                     */
/* ------------------------------------------------------------------ */

int gAutoHostStart;	/* MP_AUTOSTART=host: auto-launch when N players are in */
int gAutoHostFrames;
int gAutoHostTarget = 2;	/* players (incl. us) to wait for before starting */
int gAutoHostSettle;	/* frames the target has been met for */
int gHostInWorld;	/* the host actually reached the running level */

extern int gBootSuppressLevel;	/* main.c: a module takes the level launch over */

/* The host quit the match? Only once it has really BEEN in the world: at the
 * start of a match `running` is set a frame or two before gInFrontend clears,
 * and keying on `running && gInFrontend` alone would tear the fresh session
 * down during that transition. */
int MpHostLeftMatch(void)
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
int MpOnCmdLine(void* userdata, void* args)
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

						if (city < 0 || city >= CITY_COUNT)
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
void MpAutostartFromEnv(void)
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

/* Back in the menus FROM A MATCH (JER_EVENT_FRONTEND_ENTERED, glaunch.c): the
 * session is over for this machine, whoever we were in it.
 *
 * Only the HOST had a teardown for this (MpHostLeftMatch, polled per frame). A
 * CLIENT that chose Quit in the pause menu (MpOnGameQuit leaves Quit to the
 * engine) came back to the main menu still role CLIENT and still running, so
 * the next single-player Take a Ride was not single player: the city confirm
 * skipped the Single Player / Multiplayer question (it is gated on !running)
 * and MpOnMpFrontend's client branch claimed START -- rejoining the old session
 * or failing with "Not connected to a server" -- instead of the stock launch.
 * Leave the session here, and reset the map shape the mode question writes so
 * a later session starts from the full city. NumPlayers is left to the stock
 * frontend, which sets it per path. */
int MpOnFrontendEntered(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	if (gMp.role == MP_ROLE_NONE || !gMp.running)
		return JER_RESULT_CONTINUE;

	if (MpRelaunchActive())
		return JER_RESULT_CONTINUE;	/* a listen-host map switch: the session goes on */

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] back in the frontend from a match - leaving the session (role %d)\n",
			(int)gMp.role);

	if (gMp.role == MP_ROLE_HOST)
		MpHostByeAll();

	MpLeaveSession();	/* a client says its own bye in here */
	MpSetMapShape(0, 0);

	return JER_RESULT_CONTINUE;
}

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
int MpOnFrontendIdle(void* userdata, void* args)
{
	JER_ARGS_FRONTEND_IDLE* idle = (JER_ARGS_FRONTEND_IDLE*)args;

	(void)userdata;

	if (idle != NULL && gMp.role != MP_ROLE_NONE)
		idle->suppress = 1;

	return JER_RESULT_CONTINUE;
}

/* JERICHO-HOOK: the frontend has drawn the current screen.
 *
 * The one place a module can put text ON a menu row: JER_EVENT_FRAME fires
 * before DrawScreen, so anything drawn there is covered by the menu box. Both
 * server browsers draw their table here. */
int MpOnFrontendDraw(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	MpUiDrawOnline();	/* the discovery server's list */
	MpUiDrawJoin();		/* the LAN beacons' list */

	return JER_RESULT_CONTINUE;
}
