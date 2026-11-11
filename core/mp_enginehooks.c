/*
 * core/mp_enginehooks.c -- the car/ped/game hooks.
 *
 * The JER_EVENT_* handlers that connect mp to the engine's cars, pedestrians
 * and level lifecycle: the grey stand-in draw colour, the local Super Turbo
 * engine force, a player's character colour, the per-player input application
 * (car and on-foot), and the level-launch / game-start / net-recv hooks.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_frontend.h"
#include "jer_pause_menu.h"
#include "jer_colour.h"		/* a player's suit colour is a canonical JER_COLOUR */
#include "jer_ped_palette.h"	/* a player's own Tanner in their own colour */
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
#include "mp_bot.h"	/* the test bot; testing only, inert unless MP_BOT is set */

extern void MpNoteLocalPad(int pad);	/* mp_session.c: the pad driving our own car */

/* A car whose real model is still loading is drawn as the host city's car 1,
 * solid grey: the stand-in body is obvious, and the chase camera never follows a
 * slot with no mesh. Only cars marked by the reload failsafe are touched. */
int MpOnCarDrawColor(void* userdata, void* args)
{
	JER_ARGS_CAR_DRAW_COLOR* a = (JER_ARGS_CAR_DRAW_COLOR*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;

	(void)userdata;

	if (cp != NULL && MpCarIsStandIn(cp->id))
	{
		/* The placeholder body already reads as a stand-in; only the host-car-1
		 * fallback is painted grey. */
		if (MpStandInIsPlaceholder())
			return JER_RESULT_CONTINUE;

		a->flatBlack = 0;
		a->tintR = a->tintG = a->tintB = 128;	/* solid grey */
	}

	return JER_RESULT_CONTINUE;
}

/* CAR_DATA.thrust is a short. ProcessCarPad (handling.c) copies this hook's
 * thrust back with `(short)jerArgs.thrust`. A stock accel thrust is about
 * 4915 (powerRatio 4096 × 4915, FIXEDH), so ×10 does not fit: 49150 wraps to
 * a negative short and the car reverses. Clamp to the short range. That is
 * the whole of ×10 the field can hold — about 6.7× on that stock car, and
 * exactly ×10 when the unscaled thrust is within ±3276. */
#define MP_SUPER_TURBO_MUL	10
#define MP_THRUST_SHORT_MAX	32767
#define MP_THRUST_SHORT_MIN	(-32768)

/* The car this machine is driving. In a match NumPlayers is forced to 1, so
 * this is player[0]. Split-screen outside a session is 2. Do not walk the
 * rest of player[]: those entries are how a match seats other people's cars. */
static int MpCarIsLocalSeat(const CAR_DATA* cp)
{
	int i, n;

	if (cp == NULL)
		return 0;

	n = (int)NumPlayers;
	if (n < 1)
		n = 1;
	if (n > 2)
		n = 2;

	for (i = 0; i < n; i++)
	{
		int slot = (int)player[i].playerCarId;

		if (slot >= 0 && slot == (int)cp->id)
			return 1;
	}

	return 0;
}

/* JER_EVENT_CAR_ENGINE: end of ProcessCarPad, after engine force is computed
 * and before it is stored. Super Turbo is a local toggle — JMP Options and the
 * pause Multiplayer page both write gMp.config.superTurbo — not the host's
 * Invulnerability rule, which is packed into the session and applied on every
 * machine. Only the local seat is scaled, so a remote car's pad path — and
 * therefore its net sync — is left alone. Peers still see the faster car,
 * because they adopt this machine's owner-authoritative state. */
int MpOnCarEngine(void* userdata, void* args)
{
	JER_ARGS_CAR_ENGINE* a = (JER_ARGS_CAR_ENGINE*)args;
	long long scaled;

	(void)userdata;

	if (a == NULL || !gMp.config.superTurbo || !MpCarIsLocalSeat((const CAR_DATA*)a->car))
		return JER_RESULT_CONTINUE;

	scaled = (long long)a->thrust * MP_SUPER_TURBO_MUL;
	if (scaled > MP_THRUST_SHORT_MAX)
		scaled = MP_THRUST_SHORT_MAX;
	else if (scaled < MP_THRUST_SHORT_MIN)
		scaled = MP_THRUST_SHORT_MIN;
	a->thrust = (int)scaled;

	return JER_RESULT_CONTINUE;
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
int MpOnPedDraw(void* userdata, void* args)
{
	JER_ARGS_PED_DRAW* a = (JER_ARGS_PED_DRAW*)args;
	int i, on = 0, r = 0, g = 0, b = 0, who = -1;

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
		who = gMp.localPlayerId;
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
				who = p->id;
				break;
			}
		}
	}

	/* GAME-MODE DECISION POINT: a team colour would replace the suit colour here.
	 * Take a Ride has no teams, so on/r/g/b come back exactly as they went in. */
	on = MpModePlayerColour(gMp.gamemode, who, on, r, g, b, &r, &g, &b);

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

/* Apply each player car's input. Our own car may be driven by the test bot
 * (mp_bot.c); a remote car gets the pad its owner replicated. */
int MpOnNetInput(void* userdata, void* args)
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
			if (pauseflag != 0)
			{
				/* The pause menu owns the pad: swallow it so navigating the menu
				 * (D-pad / buttons) never reaches the car. The engine keeps
				 * running StepSim while mp pauses (see MpOnPauseMenu, below) and
				 * re-reads the pad with no pause guard, so zero it and mark it
				 * handled rather than letting the stock pad leak through. */
				in->pad = 0;
				in->handled = 1;
			}
			else if (gMp.chatOpen)
			{
				/* ...and the CHAT PROMPT owns the keyboard: the pad is built from the
				 * raw keyboard state (PsyX_pad.cpp), so every letter typed into a
				 * line is also a pad button -- typing "wasd" steered and
				 * accelerated the car under the player. Swallow our own pad for as
				 * long as the prompt is up, exactly as the pause menu does. */
				in->pad = 0;
				in->handled = 1;
			}
			else if (MpRoundHoldsCar())
			{
				/* ...and the ROUND holds everyone still through the 3-2-1 and the
				 * result screen (the timer modes; never Take a Ride, never the
				 * WAITING lobby). Not a zeroed pad, which lets a moving car roll:
				 * the engine's own stop (handling.c's "can't drive" lock) --
				 * the handbrake, or the brake while the wheels still turn fast.
				 * Bot included; GO (the referee's LIVE) gives the pad back. */
				in->pad = (car_data[carId].hd.wheel_speed > 36864) ? CAR_PAD_BRAKE : CAR_PAD_HANDBRAKE;
				in->handled = 1;
			}
			else if (MpBotEnabled())
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
int MpOnPedInput(void* userdata, void* args)
{
	JER_ARGS_PED_INPUT* in = (JER_ARGS_PED_INPUT*)args;

	(void)userdata;

	if (in == NULL || !gMp.running)
		return JER_RESULT_CONTINUE;

	/* The pause menu owns the pad while it is up: hold the ped still too, so
	 * menu navigation never walks the player. The chat prompt owns the KEYBOARD the
	 * same way -- the ped pad comes from the same raw keyboard state -- so typing a
	 * line must not walk us either, and the round hold (the 3-2-1 and the result
	 * screen, MpRoundHoldsCar) keeps us standing.
	 * (No `handled` field on this hook, so zeroing
	 * the bits is the swallow.) */
	if ((pauseflag != 0 || gMp.chatOpen || MpRoundHoldsCar()) && (PLAYER*)in->player == &player[0])
	{
		in->pad = 0;
		return JER_RESULT_CONTINUE;
	}

	if (!MpBotEnabled())
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
int MpOnGameStart(void* userdata, void* args)
{
	int i;

	(void)userdata;
	(void)args;

	/* A level start (fresh launch, restart or next mission) rebuilds the engine's
	 * ped banks, so our "already imported/installed" bookkeeping is stale even
	 * when GameLevel is unchanged. Forget it before anything decides to skip. */
	MpPedArchiveReset();

	/* A level is up: our car (and every peer's copy) is real from this moment.
	 * MpSendOwnCarState refuses to send anything before this, because the
	 * frontend/loading world's car_data is zeroed or a demo car at the origin.
	 * Set for single player too, so the module has one "level is really up"
	 * signal there as well (the test levers read it). */
	gMp.gameStarted = 1;

	if (!MpIsActive())
	{
		/* Single player: no session warm runs, so load the archive here when the
		 * stored choice is a custom entry. A Tanner run loads no ped pages, and
		 * Jericho alone uses Havana's native models or the on-foot import. */
		if (gMp.config.pedModel >= MP_PED_MODEL_CUSTOM_FIRST)
			MpPedArchiveEnsure();

		return JER_RESULT_CONTINUE;
	}

	if (gMpCtx == NULL)
		return JER_RESULT_CONTINUE;

	MpTestJerichoImport();

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];
		CAR_DATA* cp;

		if (!p->active || p->carId < 0 || p->carId >= MAX_CARS)
			continue;

		cp = &car_data[p->carId];
		gMpCtx->jer_log(gMpCtx, "[mp] car: player %d slot %d controlType=%d apmodel=%d carmodel=%d want=%d loaded=%d pos=%d,%d,%d\n",
			p->id, p->carId, cp->controlType, cp->ap.model,
			(cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS) ? residentCarModels[cp->ap.model] : -1,
			(p->carId < MP_MAX_PLAYERS && PlayerStartInfo[p->carId] != NULL) ? PlayerStartInfo[p->carId]->model : -9,
			JerCarSlotUsable(cp->ap.model) ? 1 : 0,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2]);
	}

	/* The host used to broadcast a "meeting point" ('JPSW') here and every
	 * machine lined up on it. RETIRED: both machines already place both cars at
	 * the engine's own deterministic spawn, and a client gathers itself beside
	 * the host in MpHandleCarState (see ARCHITECTURE section 4). */

	return JER_RESULT_CONTINUE;
}

/* Diagnostics: log inbound addon-bridge payloads when MP_DEBUG is set. */
int MpOnNetRecv(void* userdata, void* args)
{
	JER_ARGS_NET_RECV* r = (JER_ARGS_NET_RECV*)args;

	(void)userdata;

	if (MpDebugOn() && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] bridge recv '%s' from peer %d (%d bytes)\n",
			(r->channel != NULL) ? r->channel : "?", r->peer, r->len);

	return JER_RESULT_CONTINUE;
}

/* The level-launch hook: apply the host's time of day and weather. */
int MpOnLevelLaunch(void* userdata, void* args)
{
	JER_ARGS_LEVEL_LAUNCH* l = (JER_ARGS_LEVEL_LAUNCH*)args;

	(void)userdata;

	if (MpIsActive())
	{
		/* Only override when the SESSION actually has a value. The engine seeds
		 * these args from the frontend's own picks (-1 = "mission default"), so
		 * writing -1 blindly would erase the host's choice on the Time of Day
		 * screen and drop the launch to the mission default. */
		if (gMp.timeOfDay >= 0)
			l->timeOfDay = gMp.timeOfDay;
		if (gMp.weather >= 0)
			l->weather = gMp.weather;
	}

	return JER_RESULT_CONTINUE;
}
