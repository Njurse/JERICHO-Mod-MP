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

#include "driver2.h"
#include "main.h"
#include "mission.h"
#include "pad.h"
#include "players.h"
#include "cars.h"
#include "camera.h"
#include "overmap.h"	/* gMapXOffset/gMapYOffset for the multiplayer map */
#include "glaunch.h"
#include "state.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mp.h"

MP_STATE gMp;
JERICHO_CONTEXT* gMpCtx;

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

/* Leave the whole match: back to the main frontend with a notice. Used when
 * the host ends the game or the server connection is lost. */
void MpReturnToFrontend(void)
{
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

				/* Accept either a raw model number or "slotN" (1..10), the
				 * frontend's own per-city slot. A slot is resolved against the
				 * session's city at launch (see MpLaunchLocal), because GameLevel is
				 * still the default here -- reading carNumLookup now would use the
				 * wrong city, the same trap the -host comment above records. */
				if (strncmp(v, "slot", 4) == 0)
				{
					gMp.config.car = atoi(v + 4);
					gMp.config.carIsSlot = 1;
				}
				else
				{
					gMp.config.car = atoi(v);
					gMp.config.carIsSlot = 0;
				}

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] -mpcar %s (car=%d slot=%d)\n",
						v, gMp.config.car, gMp.config.carIsSlot);
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

			gMp.autoSession = 1;	/* no menus: launch as soon as we are in */
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

static int MpOnBoot(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

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

	/* Deferred work, done on a FRAME rather than inside the poll that noticed it
	 * -- calling into the state machine from a message handler is re-entrant and
	 * crashed the client. FRAME and not PRE_SIM: a joining client is sitting in
	 * the FRONTEND, and PRE_SIM only runs in game, so the request was set and
	 * then never acted on. */
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

		if (MpIsHost() && gMp.running)
			MpSpawnLateJoiners();
	}

	/* Test lever: hold the in-game map open, so the multiplayer-map blip hook
	 * can be exercised without a human pressing the map button. */
	if (getenv("MP_MAP") != NULL && gMp.running)
	/* MP_MAP used to force the map open -- the same mistake the pause lever made: the engine then draws a map whose state was never set up, and the screen goes red. It only logs now. */
	if (getenv("MP_MAP") != NULL && gMp.running)
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
	 * own active_car_list, which StepCars walks) */
	if (gMp.running && gMpCtx != NULL && (gMp.frame % 60) == 0)
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
			gMpCtx->jer_log(gMpCtx, "[mp] list: %s #%d  car %d  slot %d  %d ms%s",
				p->name, p->id, veh, p->carId, p->pingMs, p->isHost ? "  (host)" : "");
	}
}

static void MpDrawPlayerList(void)
{
	int id, row = 0;

	SetTextColour(170, 170, 170);
	PrintString((char*)"-- PLAYERS --", 8, 56);

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		CAR_DATA* cp;
		char line[80];
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

		snprintf(line, sizeof(line), "%s #%d  car %d  %d ms",
			p->name, p->id, veh, p->pingMs);

		y = 68 + row * 10;
		row++;

		if (p->isHost)
			SetTextColour(0, 255, 255);	/* the host, cyan */
		else
			SetTextColour(200, 200, 200);

		PrintString(line, 8, y);

		/* so the list can be checked without eyes on the screen */
		if (getenv("MP_DEBUG") != NULL && gMpCtx != NULL)
		{
			static unsigned long lastListMs;

			if ((MpNowMs() - lastListMs) > 2000)
			{
				lastListMs = MpNowMs();
				gMpCtx->jer_log(gMpCtx, "[mp] list: %s\n", line);
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

	/* MP_PAUSE logs the list for a test, and deliberately does NOT force the
	 * engine's pause flag. It used to set gDrawPauseMenus = 1 every frame, which
	 * makes the engine draw pause menus whose state was never set up -- a wild
	 * pointer, and an access violation. Forcing another subsystem's state from a
	 * test lever is not a shortcut, it is a new bug.
	 *
	 *     draw  = what the player sees (needs the pause menu actually open)
	 *     log   = the rows, which is what the test is checking anyway */
	if (getenv("MP_PAUSE") != NULL && gMp.running)
		MpLogPlayerList();

	if (gDrawPauseMenus)
		MpDrawPlayerList();

	return JER_RESULT_CONTINUE;
}

static int MpOnPreSim(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	if (gMp.running)
		MpLockstepFrame();
	else if (gMp.listenersUp || gMp.connected)
		MpNetPoll(0);

	return JER_RESULT_CONTINUE;
}


/* Drive each player car from the synchronized input set (lockstep). With
 * MP_TESTDRIVE set, the remote players' cars are driven by a canned pattern
 * (accelerate + alternating steer) so they move around on screen for testing,
 * and their position is logged. */
/* Test "AI player": drives the local car with a manoeuvre that changes every
 * 0.5-4 s (steer left/right, reverse, wheelspin, handbrake). Because the local
 * car is what we replicate, peers seeing it move proves the controls arrived. */
static int MpBotPad(void)
{
	static unsigned long nextChange;
	static int cur;

	unsigned long now = MpNowMs();

	if (now >= nextChange)
	{
		unsigned int r = (unsigned int)now ^ (unsigned int)(now >> 7);
		const char* name;

		switch (r % 6)
		{
			case 0:  cur = CAR_PAD_ACCEL | CAR_PAD_LEFT;      name = "accel+left";      break;
			case 1:  cur = CAR_PAD_ACCEL | CAR_PAD_RIGHT;     name = "accel+right";     break;
			case 2:  cur = CAR_PAD_ACCEL | CAR_PAD_WHEELSPIN; name = "accel+wheelspin"; break;
			case 3:  cur = CAR_PAD_BRAKE;                     name = "reverse";         break;
			case 4:  cur = CAR_PAD_ACCEL;                     name = "accel";           break;
			default: cur = CAR_PAD_ACCEL | CAR_PAD_HANDBRAKE; name = "accel+handbrake"; break;
		}

		nextChange = now + 500 + (r % 3500);	/* 0.5 - 4 s */

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] bot: %s (pad=%#x)\n", name, cur);
	}

	return cur;
}

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
		if (getenv("MP_TESTDRIVE") != NULL && p->isLocal)
		{
			if ((gMp.frame % 60) == 0 && gMpCtx != NULL)
			{
				CAR_DATA* me = &car_data[carId];
				gMpCtx->jer_log(gMpCtx,
					"[mp] both: local %d,%d,%d spd=%d | remote(1) %d,%d,%d spd=%d\n",
					me->hd.where.t[0], me->hd.where.t[1], me->hd.where.t[2], me->hd.speed,
					car_data[1].hd.where.t[0], car_data[1].hd.where.t[1], car_data[1].hd.where.t[2], car_data[1].hd.speed);
			}

			in->pad = MpBotPad();		/* the test AI drives OUR car */
			in->handled = 1;
		}
		else if (!p->isLocal)
		{
			/* Engine-driven: this car gets the pad its owner sent, and the
			 * engine simulates it exactly like a local one. That is what makes
			 * it a real car -- it collides, it takes damage, its wheels turn --
			 * instead of a transform we paste in every frame. With no input yet
			 * (a fresh joiner) it coasts, which is harmless. */
			in->pad = MpInputForPlayer(p->id);
			in->handled = 1;
		}
		/* local car, no bot: leave the stock pad (handled stays 0) */
	}
	else if ((gMp.frame % 300) == 0 && gMpCtx != NULL)
	{
		gMpCtx->jer_log(gMpCtx, "[mp] netinput: car %d has no player row\n", carId);
	}

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

	/* Every car now exists. The host's car is standing on a spawn point the
	 * level chose, which makes it the one meeting point both machines can
	 * agree on -- so it hands that spot out and everybody lines up on it.
	 * Without this each machine keeps its own take-a-ride spawn on opposite
	 * sides of the map and can never see the others. */
	if (MpIsHost())
	{
		MP_PLAYER* me = MpLocalPlayer();

		if (me != NULL && me->carId >= 0 && me->carId < MAX_CARS)
		{
			CAR_DATA* cp = &car_data[me->carId];
			MP_SPAWN s;

			s.x = cp->hd.where.t[0];
			s.y = cp->hd.where.t[1];
			s.z = cp->hd.where.t[2];
			s.heading = cp->hd.direction;

			MpPlaceSpawns(s.x, s.y, s.z, s.heading);
			MpHostBroadcast(MP_TAG_SPAWN, 0, &s, sizeof(s));

			gMpCtx->jer_log(gMpCtx, "[mp] meeting point %d,%d,%d heading %d sent\n",
				s.x, s.y, s.z, s.heading);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* Diagnostics: log inbound addon-bridge payloads when MP_DEBUG is set. */
static int MpOnNetRecv(void* userdata, void* args)
{
	JER_ARGS_NET_RECV* r = (JER_ARGS_NET_RECV*)args;

	(void)userdata;

	if (getenv("MP_DEBUG") != NULL && gMpCtx != NULL)
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

JER_MODULE_ENTRY(jer_module_mp_entry)(JERICHO_CONTEXT* ctx)
{
	gMpCtx = ctx;

	ctx->jer_register_module(ctx, "mp", "Multiplayer", "0.1.0", "JERICHO",
		"LAN multiplayer: host/join, UDP discovery, JERICHO addon net bridge, "
		"deterministic input lockstep with host state-resync fallback.",
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
	ctx->jer_register_hook(ctx, JER_EVENT_NET_INPUT, MpOnNetInput, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, MpOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_MAP, MpOnDrawMap, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_IDLE, MpOnFrontendIdle, NULL, 0);}
