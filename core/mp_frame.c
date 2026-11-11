/*
 * core/mp_frame.c -- the per-frame drive.
 *
 * The frame hook (MpOnFrame) is the module's heartbeat: it services the net
 * every frame, re-asserts the session rules, drives the MP_WATCH forensics
 * counter, runs the deferred work and the test levers, and decides when a
 * match has ended. MpOnPreSim is the lockstep half (it steps the sim inside a
 * session) and MpOnDrawOverlay keeps a paused session simulating while the
 * engine pause is up, and shows the player-list panel.
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
#include "system.h"
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

/* The engine's frontend screen table, to answer "is the player back on the main
 * menu" from the frame hook without a dedicated getter. Screen 0 is the main
 * menu (the title/demo screen). Forward-declared: pointer comparison needs no
 * layout, and the type is defined in FEmain.c. */
struct PSXSCREEN;
extern PSXSCREEN* pCurrScreen;
extern PSXSCREEN PsxScreens[];

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
static int gTestPanelOn = -1;

/* MP_WATCH=<secs> -- the FORENSICS lever for "the game froze but the link is up".
 *
 * A stalled match and a hung frame hook look identical from outside (nothing
 * moves), and they are different bugs. This logs, once every <secs>, the frame
 * hook's own tick count next to the SIM frame (gMp.frame) and the flags that
 * explain a stall. Read the two counters together:
 *   - hookTick keeps rising, simFrame stuck  -> the sim is not stepping (a pause:
 *     check pauseflag / drawPause, or running/role if the session fell over)
 *   - the watch line STOPS altogether        -> the frame hook itself is wedged
 * Unlike MP_HEARTBEAT (which lives in the sim tick and therefore stops WITH it),
 * this runs from the frame hook, which survives a frozen sim -- so it is the one
 * that can still speak at the moment of interest. Logging only; inert unless
 * MP_WATCH is set. */
extern int pauseflag;			/* declared with the other engine externs below; */
extern int gDrawPauseMenus;		/* needed here, earlier in the file */

static unsigned long gMpFrameHookTicks;

/* WHERE the frame last got to. Set at the entry and exit of the two big entry
 * points (the frame hook in core/mp_frame.c, the sim tick in mp_session.c) and
 * printed by MP_WATCH. Read it as "the stage the PREVIOUS frame reached":
 *   sim        the sim tick was entered and never finished -> hung inside it
 *   sim-done   the sim tick finished
 *   frame      the frame hook was entered and never finished -> hung inside it
 *   frame-done the frame hook finished, so the hang is BETWEEN frames (engine side)
 * Non-static so mp_session.c's tick can set it; declared in mp.h. */
const char* gMpStage = "boot";

static void MpWatchTick(void)
{
	static unsigned long lastMs;
	const char* s = getenv("MP_WATCH");
	unsigned long now;
	int secs;

	if (s == NULL || gMpCtx == NULL)
		return;

	secs = atoi(s);

	if (secs <= 0)
		secs = 1;

	now = MpNowMs();

	if (lastMs != 0 && (now - lastMs) < (unsigned long)(secs * 1000))
		return;

	lastMs = now;

	gMpCtx->jer_log(gMpCtx,
		"[mp] watch: hookTick %lu simFrame %lu stage %s running %d role %d pauseflag %d drawPause %d\n",
		gMpFrameHookTicks, gMp.frame, gMpStage, gMp.running, (int)gMp.role, pauseflag, gDrawPauseMenus);
}

/* SESSION RULES, re-asserted every frame.
 *
 * NOT once at launch, and that is the whole point of this function: the LEVEL
 * sets these for itself when it comes up (level setup does
 * `CopsAllowed = (maxCopCars != 0)`), so a single write during the launch is
 * overwritten by the level's own opinion a frame later. Re-asserting each frame
 * is what makes the host's choice stick, and it is the same shape the engine's
 * own cheats already use -- main.c re-zeroes the player's damage every frame
 * while gInvincibleCar is set.
 *
 * gCopDifficultyLevel is re-asserted for the same reason: the frontend's Cop
 * Difficulty screen and mission scripts both write it.
 *
 * Silent until the rules are known (gMp.ruleSet) and only while a match is live,
 * so no pre-session or single-player frame gets a rule forced onto it that
 * nobody asked for. */
static void MpSessionRulesTick(void)
{
	/* "The menu says no cops" and "the sim is running with no cops" are
	 * different claims, and only the second one is the feature. Say it once per
	 * change of rules, with the ENGINE's own variables read back after the
	 * write, so the log proves which of the two we have. */
	static int reportedCops = -1, reportedDiff = -1, reportedInv = -1, reportedImmune = -1;

	if (!gMp.ruleSet || !gMp.running)
	{
		gInvulnerableWorld = 0;
		return;
	}

	CopsAllowed = gMp.ruleCops;
	gCopDifficultyLevel = gMp.ruleDiff;

	/* CopsAllowed alone is not enough: the arena missions (M498..M505) ship
	 * cops.max == 0, so LoadMission set maxCopCars = 0 and ControlNumberOfCops
	 * clamps the wanted count to it — CopsAllowed = 1 with maxCopCars = 0 still
	 * pings nothing. The police model already sits in resident slot 3
	 * (residentModels[3] == 0, the siren), so only the count gate needs
	 * overriding: re-assert the pursuit budget the way mission.c:699-725 would
	 * for a mission that carries cops. gDontPingInCops = 0 because nothing places
	 * cops in an arena — they must ping in on their own. gMinimumCops = 1 for the same
	 * reason mission.c now floors an mp map at one: RegisterFelony returns early unless a
	 * cop can SEE the player, so with no cop on the ground there is no way for the chase
	 * that raises the felony to begin - the cops would never appear at all. */
	if (gMp.ruleCops)
	{
		maxCopCars = 4;
		gMinimumCops = 1;
		gDontPingInCops = 0;
	}
	else
	{
		maxCopCars = 0;
		gMinimumCops = 0;
		gDontPingInCops = 1;
	}

	/* The two invincibility flags are a FLOOR, not an override. A rule that turns
	 * one ON grants it to everyone and cannot be switched off locally; a rule that
	 * leaves it OFF leaves each player's own pause-menu switch in charge. Forcing
	 * them back OFF would quietly delete a feature that is sitting right there in
	 * that menu -- and the host who wants it off has simply not granted it. */
	if (gMp.ruleImmune)
		gPlayerImmune = 1;

	if (gMp.ruleInvuln)
		gInvincibleCar = 1;

	/* The cheat flag above covers the local driver. The host rule is every
	 * car, on every machine — the same bit the welcome/start packet carried. */
	gInvulnerableWorld = gMp.ruleInvuln ? 1 : 0;

	if (gMp.ruleCops != reportedCops || gMp.ruleDiff != reportedDiff ||
	    gMp.ruleInvuln != reportedInv || gMp.ruleImmune != reportedImmune)
	{
		reportedCops = gMp.ruleCops;
		reportedDiff = gMp.ruleDiff;
		reportedInv = gMp.ruleInvuln;
		reportedImmune = gMp.ruleImmune;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] session rules IN FORCE: rules cops=%d diff=%d immune=%d invuln=%d -> CopsAllowed=%d maxCopCars=%d gDontPingInCops=%d gCopDifficultyLevel=%d gPlayerImmune=%d gInvincibleCar=%d\n",
				gMp.ruleCops, gMp.ruleDiff, gMp.ruleImmune, gMp.ruleInvuln,
				CopsAllowed, maxCopCars, gDontPingInCops, gCopDifficultyLevel, gPlayerImmune, gInvincibleCar);
	}
}

int MpOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gMpFrameHookTicks++;	/* one add a frame; the MP_WATCH log reads it */
	MpWatchTick();		/* prints the stage the LAST frame reached */
	gMpStage = "frame";

	/* The engine's main pause menu reads this to relabel "Restart" as "Respawn".
	 * Sync it every frame so it is always right by the time the pause opens (and
	 * cleared the frame after the session ends). */
	gJerPauseRestartAsRespawn = gMp.running ? 1 : 0;

	/* NO BACKSPACE / MAP FAST-FORWARD IN A SESSION. The engine offers it two ways:
	 * BACKSPACE (a debug/cutscene convenience, redriver2_psxpc.cpp) and the
	 * fullscreen map, which sets FastForward every frame it is open (overlay.c).
	 * A fast-forwarded sim runs at a different rate from everyone else's, which is
	 * the definition of desync.
	 *
	 * A LOCK, not a per-frame clear: clearing the flag every frame while the map
	 * re-set it every frame meant the two fought and the sim toggled fast/normal
	 * for as long as the map was up. With FastForwardLocked the engine never offers
	 * it in the first place, so nothing oscillates. */
	{
		extern int FastForward;
		extern int FastForwardLocked;

		static int lockAnnounced;

		/* The WHOLE session, not just a live match: between level load and the
		 * moment gMp.running goes up (spawn hand-off, countdown) a BACKSPACE or a
		 * map open would otherwise still step 7x before lockstep starts. */
		FastForwardLocked = (gMp.running || MpIsActive()) ? 1 : 0;

		if (FastForwardLocked != 0 && !lockAnnounced)
		{
			lockAnnounced = 1;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] fast-forward locked off for this session (BACKSPACE and the map screen both "
					"run the sim fast, which would desync it)\n");
		}

		if (FastForwardLocked && FastForward != 0)
			FastForward = 0;	/* a press from before the session started */
	}

	/* A Change car that asked for a car this machine does not hold yet and was told to
	 * wait (a spare slot busy for a frame or two): retry it here, once a frame, in the
	 * sim, which is where a level's resident slots mean anything. */
	MpCarChangeWaitTick();

	MpNetPoll(0);

	/* The advertisement is driven from here, not from the frontend's own tick: a
	 * match runs with no frontend on screen, and that is exactly when a listing has
	 * to keep saying the right number of players. */
	MpMasterAdvertiseCounts(gMp.playerCount, gMp.running);
	MpMasterAdvertisePoll();

	/* The UPnP attempt reports itself once, on the frame after its worker
	 * finishes -- never from the worker (it must not touch the console). */
	MpUpnpPoll();

	MpUiTick();

	/* The host's session rules, every frame -- the level sets these for itself
	 * when it loads, so a one-shot write at launch would not survive it. */
	MpSessionRulesTick();

	/* The test levers ride on the lockstep tick, which exists only inside a session
	 * (MpLockstepFrame returns at once when not running). Run the ones that have a
	 * single-player meaning out here as well, so the on-foot and Change car paths
	 * have a padless way to be exercised. No-ops unless their lever is set AND the
	 * player is actually driving. */
	if (!gMp.running && !gInFrontend)
	{
		MpTestPauseCarTick();
		MpTestOnFootTick();
	}

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
	MpChatGrabKeyboard((gMp.chatOpen || MpUiManualEditing()) ? 1 : 0);
	jer_input_block(gMp.chatOpen);	/* typing never drives the car, horn, camera or pause */
	MpRejoinTick();		/* a server city change: rejoin, then restore our car */
	MpJoinLeaveTick();	/* a pending leave sound */

	/* Test lever: MP_TEST_CITYCONFIRM=<secs>[,<gameType>[,<role>]] fires the
	 * take-a-ride CITY CONFIRM the way the frontend does (CutSceneCitySelectScreen
	 * on CROSS), that many seconds after the frontend comes up. A padless run
	 * cannot press X on that screen -- the frontend reads Pads[0].mapnew -- so
	 * without this the Single Player / Multiplayer prompt path is unreachable
	 * headlessly. The prompt OPENING is asserted separately: MpUiTick logs "the
	 * Single Player / Multiplayer menu did not open" on failure, so a run that
	 * REQUIRES the line below and FORBIDS that one is the regression guard for
	 * this whole flow. Inert unless set.
	 *
	 * The two optional fields exist because the gameType ALONE is a lie: measured
	 * on the real LAN host flow, this hook arrives with gameType == GAME_MISSION
	 * (0) and a session role already set, and that combination is exactly what the
	 * gate has to accept. Synthesising only GAME_TAKEADRIVE is what let the LAN
	 * path regress unnoticed, so the guard needs to be able to present the real
	 * shape. gameType 0..15 (dr2types.h), role 0=NONE 1=HOST 2=CLIENT
	 * (mp_proto.h); defaults are the old behaviour. */
	{
		static unsigned long confirmAtMs = 0;
		static int confirmDone = 0;
		const char* s = getenv("MP_TEST_CITYCONFIRM");

		if (s != NULL && !confirmDone && gInFrontend && gMpCtx != NULL)
		{
			if (confirmAtMs == 0)
				confirmAtMs = MpNowMs() + (unsigned long)(atoi(s) * 1000);

			if (MpNowMs() >= confirmAtMs)
			{
				JER_ARGS_FRONTEND fe;
				int gameType = GAME_TAKEADRIVE;
				int role = MP_ROLE_NONE;
				int roleSaved = (int)gMp.role;
				const char* comma = strchr(s, ',');

				confirmDone = 1;

				if (comma != NULL)
				{
					const char* comma2;

					gameType = atoi(comma + 1);

					comma2 = strchr(comma + 1, ',');
					if (comma2 != NULL)
						role = atoi(comma2 + 1);
				}

				fe.gameLevel = MpGetGameLevel();
				fe.gameType = (GAMETYPE)gameType;
				fe.numPlayers = 1;
				fe.defer = 0;

				/* Present the role the way the real flow does, then put it back: the
				 * lever exists to show the gate a session, not to leave a half-open
				 * one behind for the rest of the run. */
				gMp.role = role;
				jer_fire(JER_EVENT_FRONTEND, &fe);
				gMp.role = roleSaved;

				gMpCtx->jer_log(gMpCtx,
					"[mp] test: MP_TEST_CITYCONFIRM -> city confirm fired (gameType=%d role=%d defer=%d)\n",
					gameType, role, fe.defer);
			}
		}
	}

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

	/* Test lever: MP_TEST_OPTIONS=<secs> fires the pause menu's four Player options
	 * once, that many seconds after the match goes live, by calling the SAME
	 * handlers the menu rows call -- so the toggles and the two one-shot repairs
	 * are checkable with no pad and no menu. Inert unless set. */
	{
		static unsigned long optAtMs = 0;
		const char* s = getenv("MP_TEST_OPTIONS");

		if (s != NULL && gMpCtx != NULL)
		{
			if (optAtMs == 0 && gMp.running)
				optAtMs = MpNowMs() + (unsigned long)(atoi(s) * 1000);

			if (optAtMs != 0 && optAtMs != (unsigned long)-1 && MpNowMs() >= optAtMs)
			{
				optAtMs = (unsigned long)-1;	/* once */
				gMpCtx->jer_log(gMpCtx,
					"[mp] test: MP_TEST_OPTIONS -> firing the four Player options\n");
				MpOptToggleInvincible(NULL, 0);
				MpOptToggleImmune(NULL, 0);
				MpOptRepair(NULL, 0);
				MpOptClearFelony(NULL, 0);
			}
		}
	}

	/* Test lever: MP_TEST_PED_MODEL=<id> selects a character through the same
	 * MpSetPedModel the Player Options row calls, once a level is live (a session
	 * or single player). This is what lets a padless run exercise the whole sync
	 * path (the choice log, JPCL/the roster/JPPD, and the receiving stand-in) and
	 * the single-player apply. Inert unless set. */
	{
		static int pedModelDone = 0;
		static unsigned long pedModelWaitMs = 0;
		const char* s = getenv("MP_TEST_PED_MODEL");

		if (s != NULL && !pedModelDone && gMpCtx != NULL &&
		    (gMp.running || (!gInFrontend && gMp.gameStarted)))
		{
			int model = atoi(s);
			int ok;

			/* Single player has not warmed the archive for a Tanner default, so a
			 * custom id cannot be selectable until its entry table exists. A
			 * session may also reach the frame hook a frame or two before its
			 * level-start warm finishes. */
			if (model >= MP_PED_MODEL_CUSTOM_FIRST && !MpPedModelSelectable(model))
				MpPedArchiveEnsure();

			/* Wait for the entry to become selectable instead of burning the
			 * one-shot on accepted 0. */
			if (model >= MP_PED_MODEL_JERICHO && !MpPedModelSelectable(model))
			{
				if (pedModelWaitMs == 0)
					pedModelWaitMs = MpNowMs();

				if (MpNowMs() - pedModelWaitMs < 5000)
					goto ped_model_done;
			}

			pedModelDone = 1;
			ok = MpSetPedModel(model);
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: MP_TEST_PED_MODEL -> %d (accepted %d)\n", model, ok);
			(void)ok;
		}
	ped_model_done:;
	}

	/* Test lever: MP_TEST_RESPAWN=<secs> fires the pause menu's Respawn --- the
	 * nearest-road respawn, MpRespawnOnRoad --- that many seconds in, so the
	 * repair + clear-felony side of Respawn is checkable with no pad and no menu.
	 * Inert unless set. */
	{
		static unsigned long rsAtMs = 0;
		const char* s = getenv("MP_TEST_RESPAWN");

		if (s != NULL && gMpCtx != NULL)
		{
			if (rsAtMs == 0 && gMp.running)
				rsAtMs = MpNowMs() + (unsigned long)(atoi(s) * 1000);

			if (rsAtMs != 0 && rsAtMs != (unsigned long)-1 && MpNowMs() >= rsAtMs)
			{
				MP_PLAYER* me = MpLocalPlayer();
				int hasCar = (me != NULL && me->carId >= 0 && me->carId < MAX_CARS);
				int injured = hasCar ? car_data[me->carId].totalDamage : -1;
				int before = hasCar ? car_data[me->carId].felonyRating : -1;
				int ok;

				rsAtMs = (unsigned long)-1;	/* once */
				ok = MpRespawnOnRoad();
				gMpCtx->jer_log(gMpCtx,
					"[mp] test: MP_TEST_RESPAWN -> returned %d; damage %d -> %d, felony %d -> %d\n",
					ok, injured, hasCar ? car_data[me->carId].totalDamage : -1,
					before, hasCar ? car_data[me->carId].felonyRating : -1);
			}
		}
	}

	/* Test lever: MP_TEST_FELONY=<value> sets the local car's felony rating, so a
	 * respawn's clear-felony is observable (felony otherwise needs collisions to
	 * rise, which a padless run does not cause). Inert unless set. */
	{
		static int felonyDone = 0;
		const char* s = getenv("MP_TEST_FELONY");

		if (s != NULL && !felonyDone && gMp.running && gMpCtx != NULL)
		{
			MP_PLAYER* me = MpLocalPlayer();

			if (me != NULL && me->carId >= 0 && me->carId < MAX_CARS)
			{
				felonyDone = 1;
				car_data[me->carId].felonyRating = atoi(s);
				gMpCtx->jer_log(gMpCtx,
					"[mp] test: MP_TEST_FELONY -> car felony = %d\n",
					car_data[me->carId].felonyRating);
			}
		}
	}

	/* Test lever: MP_TEST_RULES=<secs> adopts the configured session rules
	 * (mirroring MpRulesAdopt) that many seconds in, so MpSessionRulesTick's
	 * per-frame re-assertion --- and the cops count gate it opens --- is
	 * checkable without the host menu ever running. Inert unless set. */
	{
		static unsigned long rlAtMs = 0;
		const char* s = getenv("MP_TEST_RULES");

		if (s != NULL && gMpCtx != NULL)
		{
			if (rlAtMs == 0 && gMp.running)
				rlAtMs = MpNowMs() + (unsigned long)(atoi(s) * 1000);

			if (rlAtMs != 0 && rlAtMs != (unsigned long)-1 && MpNowMs() >= rlAtMs)
			{
				rlAtMs = (unsigned long)-1;	/* once */
				gMp.ruleCops = gMp.config.cops;
				gMp.ruleInvuln = gMp.config.invuln;
				gMp.ruleImmune = gMp.config.immune;
				gMp.ruleDiff = gMp.config.copDifficulty;
				gMp.ruleSet = 1;
				gMpCtx->jer_log(gMpCtx,
					"[mp] test: MP_TEST_RULES -> adopted cops=%d diff=%d\n",
					gMp.ruleCops, gMp.ruleDiff);
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

	/* A listen-host map switch (MpRelaunchTick): leave the level, then START
	 * again on the new map -- the session is NOT over, so none of the teardowns
	 * below may see this as the host leaving. */
	{
		int rl = MpRelaunchTick(gInFrontend);

		if (rl == MP_RELAUNCH_END_LEVEL && !gInFrontend)
			EndGame(GAMEMODE_QUIT);
		if (rl != MP_RELAUNCH_NONE)
			gHostInWorld = 0;
	}

	/* The host quit the match: it is back in the frontend while the session
	 * is still live, so tell the clients and tear everything down. */
	if (!MpRelaunchActive() && MpHostLeftMatch())
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] host left the match - notifying clients\n");

		MpHostByeAll();
		MpLeaveSession();
	}

	/* BACKING OUT OF THE MULTIPLAYER MENUS WITHOUT STARTING. Pressing Host Game
	 * (or joining) puts the module in HOST/CLIENT role and opens a listener or a
	 * connect, and backing out of the menus before a match starts used to leave
	 * that role behind -- the next single-player Take a Ride was then intercepted
	 * as a multiplayer launch, and the idle demo stayed suppressed. Cancel the
	 * lobby once the player is back on the MAIN MENU (screen 0) with no match.
	 * The -host / MP_AUTOSTART path is exempt: it has no menus and must keep
	 * hosting until a player arrives or the user quits. */
	if (gMp.role != MP_ROLE_NONE && !gMp.running && !gMp.autoSession &&
	    gInFrontend && pCurrScreen == PsxScreens)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] back on the main menu with no match - cancelling the lobby\n");

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

			if (!p->active || p->carId < 0 || p->carId >= MAX_CARS)
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

	gMpStage = "frame-done";
	return JER_RESULT_CONTINUE;
}

/* In-game the DRAW_OVERLAY event fires on every drawn frame -- including while
 * the game is PAUSED, where StepSim (and so JER_EVENT_FRAME) does not run.
 * Servicing the socket here is what keeps a paused session alive: we keep
 * sending our keepalive and keep draining the peer's. */
extern void SetTextColour(unsigned char Red, unsigned char Green, unsigned char Blue);
extern int  PrintStringHiresScaledSpaced(char* string, int x, int y, float scale, float extra);
extern int  gDrawPauseMenus;

/* A live match must keep simulating while the pause menu is up, but the engine
 * freezes the world (pauseflag blocks its StepSim). StepSim is an EXPORTED
 * engine symbol, so we drive the world ourselves from the overlay hook, which
 * fires on every drawn frame including while paused. */
extern void StepSim(void);
extern int  pauseflag;
extern void UnPauseSound(void);	/* the engine pauses music/sfx with the world */

int MpOnDrawOverlay(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	if (gMp.role == MP_ROLE_NONE)
		return JER_RESULT_CONTINUE;

	MpNetPoll(0);

	/* A LIVE MATCH DOES NOT FREEZE FOR THE PAUSE MENU. The engine freezes the world
	 * while its pause menu is open (pauseflag) and does not run JER_EVENT_FRAME --
	 * where our net tick lives -- while frozen, and it pauses the audio with it.
	 * So while a session is up and paused we step the world ourselves and undo the
	 * audio pause once on the way in. The menu is still the engine's.
	 *
	 * Do NOT call MpLockstepFrame again here: StepSim() fires JER_EVENT_PRE_SIM at
	 * its top, and our MpOnPreSim handler runs the lockstep. Calling it here as
	 * well advanced gMp.frame (and ran every lockstep tick) TWICE on the paused
	 * machine while every other machine advanced once -- the pausing player was
	 * one frame ahead per frame of their own pause. */
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
		}
		else
		{
			wasPaused = 0;
		}
	}

	/* The player list / MP options ride on the engine's PAUSE menu -- and only on
	 * the pause menu.
	 *
	 * The two obvious signals are both wrong on their own. `gDrawPauseMenus` means
	 * "a pause-MODE menu is up", which includes the game-over, mission-complete
	 * and pad-error screens (pause.c sets it in PauseMenu(), for every mode).
	 * `pauseflag` means "the world is frozen", which the director and cutscenes
	 * raise too. Together they put this panel on screen while the player was
	 * watching a cutscene.
	 *
	 * So: the engine fires JER_EVENT_PAUSE_MENU/JER_PAUSE_OPEN for the pause
	 * itself (main.c), and when we do not claim the press the engine opens
	 * PAUSEMODE_PAUSE. MpOnPauseMenu records that as gMpPauseOpened, and here we
	 * clear it as soon as the menus stop being drawn. The panel is then shown
	 * exactly while the pause the player opened is on screen. */
	if (!gDrawPauseMenus)
		gMpPauseOpened = 0;

	gMpShowPlayers = (gMp.running && gMpPauseOpened && gDrawPauseMenus) ? 1 : 0;

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

	/* MP_PANEL: show the panel itself, so its layout and colours can be captured
	 * from a -shot run with no pad and no engine pause. Unlike MP_PAUSE above,
	 * this sets OUR OWN flag (gMpShowPlayers) rather than reaching into the
	 * engine's pauseflag -- the point of the rule above is not to force another
	 * subsystem's state, not to avoid test levers. */
	if (MpLeverFlag("MP_PANEL", &gTestPanelOn) && gMp.running)
		gMpShowPlayers = 1;

	/* MP_OPENPAUSE=<secs>: open the pause for real, so the gating above can be
	 * checked with no pad (nothing else can open the engine pause headlessly).
	 * This does both halves of what the pad handler does: it fires the same
	 * JER_PAUSE_OPEN the engine fires -- which is what sets gMpPauseOpened -- and
	 * then calls the engine's own EnablePause, which is the SUPPORTED way to open
	 * the pause because it initialises the menu state. (The old MP_PAUSE lever set
	 * gDrawPauseMenus directly and read a menu that was never set up: a wild
	 * pointer. Calling the engine's entry point is not that.)
	 *
	 * It re-arms while the pause is closed, i.e. it HOLDS START. Without that a
	 * -shot lands on whatever frame it lands on, and a frame the pause happened to
	 * be closed on proves nothing either way. */
	if (gMp.running)
	{
		static unsigned long openAtMs = 0;
		const char* s = getenv("MP_OPENPAUSE");

		if (s != NULL)
		{
			if (openAtMs == 0)
				openAtMs = MpNowMs() + (unsigned long)(atoi(s) * 1000);

			if (openAtMs != (unsigned long)-1 && MpNowMs() >= openAtMs &&
				(!pauseflag || !gDrawPauseMenus))
			{
				JER_ARGS_PAUSE_MENU a;

				a.action = JER_PAUSE_OPEN;
				a.result = NULL;
				a.value = 0;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] test: MP_OPENPAUSE -> START\n");

				if (jer_fire(JER_EVENT_PAUSE_MENU, &a) != JER_RESULT_STOP)
					EnablePause(PAUSEMODE_PAUSE);
			}
		}
	}

	if (gMpShowPlayers)
		MpDrawPlayerList();

	/* The bottom-left HOST/CLIENT tag and its two live connection lines USED to be
	 * drawn here. REMOVED: with the status console drawing the same corner, the
	 * block was a second, larger debug readout stacked under it. The same facts
	 * (the role, the peer, the handshake stage, the last event) are still in the
	 * session log -- see MpConnEvent's `[mp] conn: ...` and `[mp] conn status: ...`
	 * lines, which is what MpConnLineText exists for now. */

	return JER_RESULT_CONTINUE;
}

/* One lockstep tick from the engine's PRE_SIM, plus the single-player
 * character apply that has no pose send to hang on. */
int MpOnPreSim(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	/* A local car whose imported slot is not drawable gets the same grey host-car-1
	 * stand-in in single-player as the session failsafe gives it in MP. No-op while a
	 * session is running (the failsafe owns that) and in the frontend. */
	MpCarReadinessTick();

	/* Single player has no pose send to hang the character apply on, so do it
	 * here, on the sim frame, as soon as the player is actually on foot. */
	if (!gMp.running)
		MpPreferredCharacterTick();

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
