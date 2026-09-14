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

const char* MpDefaultPlayerName(void)
{
	static char name[MP_NAME_MAX];
	const char* env = getenv("USERNAME");

	if (env == NULL || env[0] == '\0')
		env = getenv("USER");

	if (env != NULL && env[0] != '\0')
		snprintf(name, sizeof(name), "%s", env);
	else
		snprintf(name, sizeof(name), "Player");

	return name;
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

void MpConfigLoad(void)
{
	const char* s;

	memset(&gMp.config, 0, sizeof(gMp.config));

	gMp.config.port = jer_config_get_int("mp", "port", MP_DEFAULT_PORT);
	if (gMp.config.port < 1024 || gMp.config.port > 65535)
		gMp.config.port = MP_DEFAULT_PORT;

	gMp.config.beaconMs = jer_config_get_int("mp", "beacon_ms", MP_BEACON_INTERVAL_MS);
	if (gMp.config.beaconMs < 250 || gMp.config.beaconMs > 10000)
		gMp.config.beaconMs = MP_BEACON_INTERVAL_MS;

	gMp.config.keepaliveMs = jer_config_get_int("mp", "keepalive_ms", MP_KEEPALIVE_INTERVAL_MS);
	if (gMp.config.keepaliveMs < MP_KEEPALIVE_MIN_MS || gMp.config.keepaliveMs > 30000)
		gMp.config.keepaliveMs = MP_KEEPALIVE_INTERVAL_MS;

	gMp.config.modCheck = jer_config_get_int("mp", "mod_check", MP_MODCHECK_OFF);
	if (gMp.config.modCheck < 0 || gMp.config.modCheck > MP_MODCHECK_EXACT)
		gMp.config.modCheck = MP_MODCHECK_OFF;

	/* An empty default means "not set yet" -> first run: prompt for a name. */
	s = jer_config_get_str("mp", "player_name", "");
	if (s != NULL && s[0] != '\0')
	{
		snprintf(gMp.config.playerName, sizeof(gMp.config.playerName), "%s", s);
		gMp.config.firstNameSet = 1;
	}
	else
	{
		snprintf(gMp.config.playerName, sizeof(gMp.config.playerName), "%s", MpDefaultPlayerName());
		gMp.config.firstNameSet = 0;
	}

	s = jer_config_get_str("mp", "host_name", "");
	if (s != NULL && s[0] != '\0')
		snprintf(gMp.config.hostName, sizeof(gMp.config.hostName), "%s", s);
	else
		snprintf(gMp.config.hostName, sizeof(gMp.config.hostName), "%s's game", gMp.config.playerName);
}

void MpConfigSave(void)
{
	jer_config_set_int("mp", "port", gMp.config.port);
	jer_config_set_int("mp", "beacon_ms", gMp.config.beaconMs);
	jer_config_set_int("mp", "keepalive_ms", gMp.config.keepaliveMs);
	jer_config_set_int("mp", "mod_check", gMp.config.modCheck);
	jer_config_set_str("mp", "player_name", gMp.config.playerName);
	jer_config_set_str("mp", "host_name", gMp.config.hostName);
}

/* ------------------------------------------------------------------ */
/* Player registry                                                     */
/* ------------------------------------------------------------------ */

MP_PLAYER* MpLocalPlayer(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gMp.players[i].active && gMp.players[i].isLocal)
			return &gMp.players[i];
	}

	return NULL;
}

MP_PLAYER* MpGetPlayer(int id)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gMp.players[i].active && gMp.players[i].id == id)
			return &gMp.players[i];
	}

	return NULL;
}

MP_PLAYER* MpGetPlayerByCar(int carId)
{
	int i;

	if (carId < 0)
		return NULL;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gMp.players[i].active && gMp.players[i].carId == carId)
			return &gMp.players[i];
	}

	return NULL;
}

int MpIsPlayerCar(int carId)
{
	return MpGetPlayerByCar(carId) != NULL;
}

/* The overlay needs a car's pose, but only mp.c pulls in cars.h -- expose a
 * tiny accessor rather than leaking CAR_DATA into the UI file. */
void MpCarPose(int carId, int* x, int* y, int* z, int* heading)
{
	if (carId < 0 || carId >= MAX_CARS)
	{
		if (x != NULL) *x = 0;
		if (y != NULL) *y = 0;
		if (z != NULL) *z = 0;
		if (heading != NULL) *heading = 0;
		return;
	}

	if (x != NULL) *x = car_data[carId].hd.where.t[0];
	if (y != NULL) *y = car_data[carId].hd.where.t[1];
	if (z != NULL) *z = car_data[carId].hd.where.t[2];
	if (heading != NULL) *heading = car_data[carId].hd.direction;
}

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
	 * menu's Exit takes). Do NOT just SetState -- that leaves sounds playing. */
	if (!gInFrontend)
		EndGame(GAMEMODE_QUIT);
	else
		jer_frontend_goto(0);	/* already in a menu: back to the main one */
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

	if (fe->action == JER_MP_FE_START && !gMp.running &&
	    (gMp.role == MP_ROLE_HOST || gMp.role == MP_ROLE_CLIENT))
	{
		if (gMp.role == MP_ROLE_HOST)
		{
			gMp.city = GameLevel;

			if (wantedTimeOfDay >= 0)
				gMp.timeOfDay = wantedTimeOfDay;
			if (wantedWeather >= 0)
				gMp.weather = wantedWeather;

			MpStartMatch();
		}
		else if (MpClientSessionLive())
		{
			/* client: the host already started, we just launch into it */
			MpClientLaunch();
		}
		else
		{
			/* no live session (connection lost, refused, never welcomed): swallow
			 * the press rather than launching into nothing */
			jer_error("Not connected to a server");

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] START ignored: no live session\n");

			fe->claimed = 1;
			return JER_RESULT_CONTINUE;
		}

		fe->claimed = 1;
	}

	return JER_RESULT_CONTINUE;
}

MP_PLAYER* MpAddPlayer(int id, const char* name, int isLocal)
{
	int i;
	MP_PLAYER* p = NULL;

	/* reuse the row with this id if present */
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gMp.players[i].active && gMp.players[i].id == id)
		{
			p = &gMp.players[i];
			break;
		}
	}

	if (p == NULL)
	{
		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			if (!gMp.players[i].active)
			{
				p = &gMp.players[i];
				memset(p, 0, sizeof(*p));
				p->active = 1;
				p->carId = -1;
				p->padId = -1;
				p->id = id;
				gMp.playerCount++;
				break;
			}
		}
	}

	if (p == NULL)
		return NULL;	/* registry full */

	snprintf(p->name, sizeof(p->name), "%s", name != NULL ? name : "Player");
	p->isLocal = isLocal;
	p->connected = 1;

	return p;
}

void MpRemovePlayer(int id)
{
	MP_PLAYER* p = MpGetPlayer(id);

	if (p == NULL)
		return;

	/* let the player know who left (never ourselves) */
	if (!p->isLocal)
		MpNotifyf("%s left", p->name);

	/* take their car out of the world so it does not sit there parked with
	 * nobody driving it; the slot is recycled by the engine */
	if (!p->isLocal && p->carId >= 0 && p->carId < MAX_CARS)
	{
		car_data[p->carId].controlType = CONTROL_TYPE_NONE;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] removed player %d's car (slot %d)\n", p->id, p->carId);
	}

	memset(p, 0, sizeof(*p));
	p->carId = -1;
	p->padId = -1;

	if (gMp.playerCount > 0)
		gMp.playerCount--;
}

void MpResetPlayers(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		memset(&gMp.players[i], 0, sizeof(gMp.players[i]));
		gMp.players[i].carId = -1;
		gMp.players[i].padId = -1;
	}

	gMp.playerCount = 0;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

int MpIsActive(void)
{
	return gMp.role != MP_ROLE_NONE;
}

int MpIsHost(void)
{
	return gMp.role == MP_ROLE_HOST;
}

/* A short digest of the enabled-module manifest (id + version), used in
 * beacons and the handshake so mismatched lobbies are visible up front. */
unsigned short MpModHash(void)
{
	JER_MODULE_INFO info[32];
	int n = jer_module_list(info, 32);
	int i;
	unsigned int h = 2166136261u;	/* FNV-1a */

	for (i = 0; i < n; i++)
	{
		const char* s;

		if (!info[i].enabled)
			continue;

		for (s = info[i].id; s != NULL && *s != '\0'; s++)
		{
			h ^= (unsigned char)*s;
			h *= 16777619u;
		}

		h ^= '@';
		h *= 16777619u;

		for (s = info[i].version; s != NULL && *s != '\0'; s++)
		{
			h ^= (unsigned char)*s;
			h *= 16777619u;
		}

		h ^= ';';
		h *= 16777619u;
	}

	return (unsigned short)((h ^ (h >> 16)) & 0xFFFF);
}

/* The enabled-module manifest (id + version) exchanged in the handshake. */
int MpBuildManifest(MP_MOD_INFO* out, int max)
{
	JER_MODULE_INFO info[32];
	int n = jer_module_list(info, 32);
	int i, count = 0;

	for (i = 0; i < n && count < max; i++)
	{
		if (!info[i].enabled)
			continue;

		memset(&out[count], 0, sizeof(out[count]));
		snprintf(out[count].id, MP_MOD_ID_MAX, "%s", info[i].id != NULL ? info[i].id : "");
		snprintf(out[count].version, MP_MOD_VER_MAX, "%s", info[i].version != NULL ? info[i].version : "");
		out[count].enabled = 1;
		count++;
	}

	return count;
}

/* Digest of the game build (JERICHO_BUILD_VERSION) so peers on different
 * exes are refused before anything else happens. */
unsigned short MpBuildHash(void)
{
	const char* s = JERICHO_BUILD_VERSION;
	unsigned int h = 2166136261u;

	for (; s != NULL && *s != '\0'; s++)
	{
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}

	return (unsigned short)((h ^ (h >> 16)) & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* Lifecycle hooks                                                     */
/* ------------------------------------------------------------------ */

static int gAutoHostStart;	/* MP_AUTOSTART=host: auto-launch when N players are in */
static int gAutoHostFrames;
static int gAutoHostTarget = 2;	/* players (incl. us) to wait for before starting */
static int gAutoHostSettle;	/* frames the target has been met for */
static int gHostInWorld;	/* the host actually reached the running level */

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

			MpBeginHost();
		}
		else if (!strcmp(cl->argv[i], "-join"))
		{
			char ip[64];
			int port = gMp.config.port;

			snprintf(ip, sizeof(ip), "127.0.0.1");

			if (i + 1 < cl->argc && cl->argv[i + 1][0] != '-')
			{
				const char* s = cl->argv[++i];
				const char* colon = strrchr(s, ':');

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

			MpBeginJoinAsync(ip, port);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* Headless dev affordance: MP_AUTOSTART=host[:PORT] | join[:IP[:PORT]]
 * brings a session up at boot so the transport can be exercised without
 * the frontend menus (mirrors the combatd2 scripted debug driver). */
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
static int MpOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	MpNetPoll(0);
	MpUiTick();

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

	/* diagnostic: where the module thinks every player car is */
	if (gMp.running && gMpCtx != NULL && (gMp.frame % 60) == 0)
	{
		int i;

		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			MP_PLAYER* p = &gMp.players[i];

			if (!p->active || p->carId < 0)
				continue;

			gMpCtx->jer_log(gMpCtx, "[mp] pose: player %d local=%d car %d at %d,%d,%d spd=%d\n",
				p->id, p->isLocal, p->carId,
				car_data[p->carId].hd.where.t[0], car_data[p->carId].hd.where.t[1], car_data[p->carId].hd.where.t[2],
				car_data[p->carId].hd.speed);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* In-game the DRAW_OVERLAY event fires on every drawn frame -- including while
 * the game is PAUSED, where StepSim (and so JER_EVENT_FRAME) does not run.
 * Servicing the socket here is what keeps a paused session alive: we keep
 * sending our keepalive and keep draining the peer's. */
static int MpOnDrawOverlay(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	if (gMp.role != MP_ROLE_NONE)
		MpNetPoll(0);

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
			/* Remote cars are pure network puppets: no local driving at all --
			 * their transform is placed from the owner's car-state each frame. */
			in->pad = 0;
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
		gMpCtx->jer_log(gMpCtx, "[mp] car: player %d slot %d controlType=%d model=%d pos=%d,%d,%d\n",
			p->id, p->carId, cp->controlType, cp->ap.model,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2]);
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
}
