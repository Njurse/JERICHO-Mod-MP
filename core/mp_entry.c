/*
 * core/mp_entry.c -- JERICHO Multiplayer: module entry and lifecycle.
 *
 * A compiled-in deep mod (no `runtime` in mod.toml) so it can read/write the
 * game's globals (GameLevel, wantedTimeOfDay, Pads[], car_data[], player[]).
 *
 * This file is the module's spine:
 *   - the global state (gMp, gMpCtx) and the shared core state
 *   - the BOOT / SHUTDOWN lifecycle hooks
 *   - the engine accessors the frontend flow needs
 *   - the single JER_MODULE_ENTRY that registers every hook and the pause menu
 *
 * The hooks themselves live with their owning core/ module: the frontend flow
 * in mp_frontend.c, the per-frame drive in mp_frame.c, the chat keyboard in
 * mp_chat.c, the pause panel in mp_panel.c, the pause menu in mp_menu.c, and
 * the car/ped/game hooks in mp_enginehooks.c. Transport lives in mp_net.c, the
 * frontend overlay in mp_ui.c and the session sync in mp_session.c.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_frontend.h"
#include "jer_pause_menu.h"
#include "jer_colour.h"		/* a player's suit colour is a canonical JER_COLOUR */
#include "jer_ped_palette.h"	/* a player's own Tanner in their own colour */
#include "jer_npc.h"		/* JerNpc: the stand-in we drew for a remote player */
#include "jer_carexplode.h"	/* request bank 54 at entry; wreck is driven from the session */
#include "jer_carglow.h"	/* JER_EVENT_DRAW_WORLD: the fox / bomb-car glow */
#include "jer_carquery.h"	/* JER_CARQ_ROLES / _ROLEPICK: mp answers the join car select */
#include "jer_sound.h"		/* jer_sound_request_menu_bank: the join/leave sounds */

#include "driver2.h"
#include "system.h"		/* CITY_COUNT: the city registry MPCC_MAX_CITIES tracks */
#include "main.h"
#include "pause.h"	/* gJerPauseRestartAsRespawn: relabel the main menu's Restart in a session */
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

MP_STATE gMp;
JERICHO_CONTEXT* gMpCtx;

/* The engine's map-shape boot flags (main.c, set by -mp). The host's own
 * Single Player / Multiplayer answer writes them through MpSetMapShape, and the
 * session publishes them to clients, so they are read/written here too. */
extern int gBootMpLevel;	/* 1 = a multiplayer map, 0 = the full city */
extern int gBootMpArena;	/* which multiplayer map (0..MP_ARENA_MAX) */

/* Set when a lost connection must land the player on the main menu; the
 * engine decides where its own exit goes, so we jump on the next frontend
 * frame instead of trusting it. */
int gReturnToMenu;

/* mp's own in-game player list. A network session must NOT open the engine
 * pause: that freezes the whole simulation on THIS machine while everyone else
 * keeps driving, so the two states disagree the moment play resumes. The START
 * press opens this instead -- a non-freezing overlay. */
int gMpShowPlayers;
int gMpPauseOpened;	/* the player opened the engine PAUSE (not another menu) */

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

	/* Same as MpLeaveSession: the multiplayer flow set these globals, and the
	 * session is gone, so restore them for the next single-player ride. */
	GameType = GAME_TAKEADRIVE;
	NumPlayers = 1;

	gMpShowPlayers = 0;	/* the overlay only belongs to a live match */
	gMpPauseOpened = 0;

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

/* ------------------------------------------------------------------ */
/* Lifecycle hooks                                                     */
/* ------------------------------------------------------------------ */

int MpOnBoot(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	/* Chat needs the keyboard while the pad drives: take PsyX's one debug-key slot
	 * (MpChatInstall chains any existing handler so nothing else is starved). */
	MpChatInstall();

	MpNetStart();

	gMpCtx->jer_log(gMpCtx, "[mp] multiplayer ready (port %d, name '%s', firstRun=%d, build %04x, mods %04x)\n",
		gMp.config.port, gMp.config.playerName, !gMp.config.firstNameSet,
		MpBuildHash(), MpModHash());

	MpAutostartFromEnv();

	return JER_RESULT_CONTINUE;
}

int MpOnShutdown(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	MpLeaveSession();
	MpNetShutdown();

	return JER_RESULT_CONTINUE;
}

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

	/* The version is mod.toml's (0.24.0: MP_PROTO_VERSION 24, round lifecycle v2 --
	 * end reasons, damage on the carstate row, arenas 0..3, admin bit, late joiners and
	 * the out-of-round spectate, engine-style 3-2-1 and GO;
	 * 0.23.4 was round hold, pre-round repair, one centre line,
	 * the getaway's own damage (optional JPDM), Fox N Hounds digit timers, radio rotation;
	 * 0.23.3 was respawn hold at round end/countdown, join/leave sounds;
	 * 0.23.2 was role-car picks kept per connection, not saved;
	 * 0.23.1 was role cars at join, imported police cars, remote cop cones;
	 * 0.23.0 was Cops n Robbers, game mode 3, on proto 23;
	 * 0.22.3 was local bomb-beep without doppler, halved beep pace;
	 * 0.22.2 was countdown-start respawn and wreck-lock clear;
	 * 0.22.1 was countdown beep, centred glow, framework pre-game;
	 * 0.21.0 was the timer modes' round start; 0.20.0 came with MP_PROTO_VERSION 23).
	 * The core lets the manifest win over this argument (jerCtxRegisterModule), so
	 * this is only the fallback; it is kept equal to mod.toml rather than a stale
	 * "0.1.0" so the two never disagree in a log. Bump both together. */
	ctx->jer_register_module(ctx, "mp", "Multiplayer", "0.24.0", "JERICHO",
		"LAN multiplayer: host/join, UDP discovery, JERICHO addon net bridge, "
		"owner-authoritative car sync (each machine drives its own car and "
		"broadcasts its state; peers adopt it verbatim), and in-match chat.",
		"", JERICHO_SDK_VERSION);

	memset(&gMp, 0, sizeof(gMp));
	gMp.role = MP_ROLE_NONE;
	gMp.localPlayerId = 0;

	MpConfigLoad();
	MpResetPlayers();

	/* Resolve the discovery server ONCE, at activation. MpOnlineSetMaster runs
	 * getaddrinfo, which blocks -- and this is the one moment where that is
	 * affordable (module activation, behind a loading screen, once). The browser
	 * re-resolves on entry when the address has changed, but a HOST never opens
	 * the browser, and without this a host had no master to advertise on at all:
	 * every session was LAN-only however the config was set. */
	if (gMp.config.masterUrl[0] != 0)
		MpOnlineSetMaster(gMp.config.masterUrl);

	MpUiInit();		/* register the native frontend menus */

	/* Mission 23's tick and bang, appended to SFX before the car-import memtop.
	 * Asked here so any later timer-mode session has the samples. */
	jer_carexplode_request_banks();

	/* The frontend menu samples (VOICES2.BLK lump 0, the four JER_MENU_SFX_*),
	 * appended to SFX at every level load: the join/leave sounds play them. */
	jer_sound_request_menu_bank();

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, MpOnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_SHUTDOWN, MpOnShutdown, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, MpOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, MpUiDrawOverlay, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, MpOnDrawOverlay, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, jer_carglow_on_draw_world, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_DRAW_COLOR, MpOnCarDrawColor, NULL, 0);
	ctx->jer_register_hook(ctx, JER_CARQ_ROLES, MpOnCarqRoles, NULL, 0);	/* role cars: the join car select asks */
	ctx->jer_register_hook(ctx, JER_CARQ_ROLEPICK, MpOnCarqRolePick, NULL, 0);
	/* Local Super Turbo. Runs for every car ProcessCarPad steps; the handler
	 * itself ignores anything that is not this machine's seat. */
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_ENGINE, MpOnCarEngine, NULL, 0);
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
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_ENTERED, MpOnFrontendEntered, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRONTEND_DRAW, MpOnFrontendDraw, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, MpOnPauseMenu, NULL, 0);

	/* A player's own character, in their own colour -- as a DEFAULT. Priority
	 * -1000 makes us run FIRST among the ped-palette consumers, so mp selects a
	 * player's preferred colour and any mod that selects afterwards (at a normal
	 * priority) wins for the characters it claims. mp carries a preference, not a
	 * last word. */
	ctx->jer_register_hook(ctx, JER_EVENT_PED_DRAW, MpOnPedDraw, NULL, -1000);

	/* Our own page in the pause screen ("Modules" -> "Multiplayer"), plus the
	 * MP_DEEP_MENU depth proof when asked for. Both live in mp_menu.c. */
	MpMenuRegister();
}
