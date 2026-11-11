/*
 * core/mp_menu.c -- the MP pause menu and its page tree.
 *
 * The Multiplayer page under "Modules" in the engine pause menu, plus every
 * submenu it owns: Vehicle Options (the change-car picker), Player Color,
 * Player Options (Preferred Character), Kick player, Warp to Player, the
 * local player switches, and the MP_DEEP_MENU depth proof. The page is
 * registered by MpMenuRegister(), called from core/mp_entry.c.
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
#include "system.h"		/* CITY_COUNT: the city registry MPCC_MAX_CITIES tracks */
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
#define MPCC_MAX_CITIES	CITY_COUNT	/* JERICHO: the engine's city ROW CAPACITY (stock nine +
					 * 16 runtime bank slots), NOT the number of cities a
					 * session has: carhacks answers JER_CARQ_CITIES with the
					 * cities this install actually has (chkCityCount), and the
					 * picker TRUNCATES to this. It was 6 when only Driver 2's
					 * four cities could be offered, and the D1 cities past the
					 * first two were silently dropped. Holding it at the
					 * capacity means a runtime bank can never be the one that
					 * gets truncated away. */
#define MPCC_MAX_CARS	MP_CAR_LIST_MAX	/* NOT a literal: this is the `max` handed to
					 * MpCarListForCity, whose contract IS
					 * MP_CAR_LIST_MAX (mp.h). They must never
					 * drift -- the note above is a drift bug. */
#define MPCC_SLOTS	10		/* frontend slots per city (carNumLookup[city][0..9]) */

static int mpCcCities[MPCC_MAX_CITIES];
static int mpCcCityCount;
static int mpCcCityIdx;
static int mpCcModels[MPCC_MAX_CARS];
static int mpCcSlots[MPCC_MAX_CARS];	/* the slot each model came from, parallel to mpCcModels */
static int mpCcModelCount;
static int mpCcModelIdx;
static int mpCcLoggedCity = -1;	/* the city this picker last described */
static int mpCcLoggedCount = -1;	/* ...and how many slots it offered then */
static int mpCcLoggedCities = -1;	/* ...and how many CITIES its city row could cycle */

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

	/* SAY WHICH CITIES THE ROW OFFERS, not just the one showing. "the picker only shows
	 * the host city" is a report this line answers directly: it names the list the row will
	 * cycle, so a run can tell a one-city list (nobody answered JER_CARQ_CITIES, or the car
	 * mods' roster hack is off) from the full one. Logged when it changes, like the roster
	 * line below, so a list that shrinks is as visible as one that grows. */
	if (mpCcLoggedCities != mpCcCityCount)
	{
		char names[192];
		int i;

		mpCcLoggedCities = mpCcCityCount;
		names[0] = 0;

		for (i = 0; i < mpCcCityCount; i++)
		{
			size_t used = strlen(names);

			if (used < sizeof(names) - 16)
				snprintf(names + used, sizeof(names) - used, "%s%s",
					(i > 0) ? " " : "", MpCarCityName(mpCcCities[i]));
		}

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] car select: %d city/cities on the row - %s\n",
				mpCcCityCount, (names[0] != 0) ? names : "(none)");
	}

	/* Ask the car mods, not the engine's table: in a session CarAvailability is still its
	 * initialiser (four cars a city), because the FRONTEND car screen is what writes it and
	 * -host/-join never opens that screen. MpCarListForCity asks the mods first and only
	 * falls back to that table when nobody answers. */
	mpCcModelCount = MpCarListForCity(city, mpCcSlots, mpCcModels, MPCC_MAX_CARS);

	/* EVERY CAR THE CITY HAS, loaded here or not.
	 *
	 * This used to be filtered down to what the level could build right now unless a
	 * session was running, on the reasoning that an unloaded pick could only be
	 * refused. It can't: choosing one IS the request that loads it -- MpChangeCar
	 * asks carhacks to import it, in single player exactly as in a session. So the
	 * filter never prevented a refusal, it removed the feature: out of a session every
	 * city listed nothing at all, which is what "no cars available" was. */

	/* What the picker had to leave out, logged when it changes - "I cannot pick that car" is
	 * the report this whole path exists to answer, and a silent skip is what made it need
	 * answering twice. It is computed from the roster that was actually built, so the line
	 * cannot claim something the menu does not show, whichever source answered. */
	if (mpCcLoggedCity != city || mpCcLoggedCount != mpCcModelCount)
	{
		char hidden[96];
		int h = 0;
		int seen[MPCC_SLOTS];

		mpCcLoggedCity = city;
		mpCcLoggedCount = mpCcModelCount;

		for (slot = 0; slot < MPCC_SLOTS; slot++)
			seen[slot] = 0;

		for (slot = 0; slot < mpCcModelCount; slot++)
		{
			if (mpCcSlots[slot] >= 0 && mpCcSlots[slot] < MPCC_SLOTS)
				seen[mpCcSlots[slot]] = 1;
		}

		strcpy(hidden, "; hidden:");

		for (slot = 0; slot < MPCC_SLOTS; slot++)
		{
			size_t used;

			if (seen[slot])
				continue;

			used = strlen(hidden);

			if (used < sizeof(hidden) - 16)
				snprintf(hidden + used, sizeof(hidden) - used, " [%d]", slot);

			h++;
		}

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] car select: %s offers %d of %d slot(s)%s%s\n",
				MpCarCityName(city), mpCcModelCount, MPCC_SLOTS, (h > 0) ? hidden : "",
				gMp.running ? "" : " (single player)");
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

/* PALETTE: the six colour variants every car's CLUT rows offer (civ_clut[..][..][0..5];
 * a variant a city never filled falls back to the base colours in CarClutLookup).
 * Applied to OUR car at once -- the owner's carstate carries ap.palette, so every
 * peer repaints it on the next packet -- and kept in defaultPlayerPalette, which the
 * next spawn (and the frontend car select) uses, and in mp.ini [mp] palette. */
#define MP_PALETTES	6

extern u_char defaultPlayerPalette;	/* main.c: the palette the player's car spawns in */

/* The car we are driving right now, or -1 (the menu is reachable before one exists). */
static int MpCcOwnCar(void)
{
	MP_PLAYER* me = MpLocalPlayer();

	if (me != NULL && me->carId >= 0 && me->carId < MAX_CARS)
		return me->carId;
	if (!gMp.running && player[0].playerCarId >= 0 && player[0].playerCarId < MAX_CARS)
		return player[0].playerCarId;
	return -1;
}

/* How many palettes the car SELECTED in the picker has (cars.h): the row describes
 * the car "Respawn as this car" would give you, so browsing City/Car updates it at
 * once. A Driver 1 car or a special has one, so it reads 1/1 there. */
static int MpCcPaletteCount(void)
{
	if (mpCcModelCount <= 0 || mpCcModelIdx < 0 || mpCcModelIdx >= mpCcModelCount)
	{
		int c = MpCcOwnCar();

		return (c >= 0) ? CarSlotPaletteCount(car_data[c].ap.model) : MP_PALETTES;
	}

	return CarModelPaletteCount(MpCcCity(), mpCcModels[mpCcModelIdx]);
}

/* Is the picker's selection the car we are driving? Then a palette change shows live. */
static int MpCcPickIsOwnCar(int c)
{
	if (c < 0 || mpCcModelCount <= 0 || mpCcModelIdx < 0 || mpCcModelIdx >= mpCcModelCount)
		return 0;

	return MpResidentSlotForCar(MpCcCity(), mpCcModels[mpCcModelIdx]) == car_data[c].ap.model;
}

/* The palette the row shows: the player's choice (defaultPlayerPalette) clamped to
 * what the selected car has -- the one "Respawn as this car" will paint it in. */
static int MpCcCurPalette(int n)
{
	int cur = (int)defaultPlayerPalette;

	return (cur >= 0 && cur < n) ? cur : 0;
}

static void MpCcLabelPalette(void* ud, char* out, int max)
{
	int n = MpCcPaletteCount();

	(void)ud;
	snprintf(out, max, "Palette: %d/%d", MpCcCurPalette(n) + 1, n);
}

static int MpCcAdjustPalette(void* ud, int dir)
{
	MP_PLAYER* me = MpLocalPlayer();
	int c = MpCcOwnCar(), n = MpCcPaletteCount();
	int pal = (MpCcCurPalette(n) + dir + n) % n;

	(void)ud;

	if (n <= 1)
		return JER_PAUSE_QUIT_NONE;	/* nothing to cycle on this car: keep the choice */

	defaultPlayerPalette = (u_char)pal;
	gMp.config.palette = pal;
	MpConfigSave();

	if (me != NULL)
		me->palette = pal;

	/* Live only on the car we drive, and only when it is the one selected; a palette
	 * for another car waits for "Respawn as this car". */
	if (MpCcPickIsOwnCar(c))
		car_data[c].ap.palette = (u_char)CarSlotClampPalette(car_data[c].ap.model, pal);

	return JER_PAUSE_QUIT_NONE;
}

/* Point the picker at the car we drive, so Vehicle Options opens on it (and its
 * palette) instead of the first city/car. Called once per pause open, never while
 * browsing. Not in a car (on foot, before a spawn) or a car the roster does not
 * list: leave the previous selection alone. */
static void MpCcSnapToOwnCar(void)
{
	int c = MpCcOwnCar(), slot, city, model, i;

	if (c < 0 || player[0].playerType != PLAYER_TYPE_CAR)
		return;

	slot = car_data[c].ap.model;

	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	city = GetCarModelSourceCity(slot);
	if (city < 0)
		city = GameLevel;	/* the level's own car */
	model = residentCarModels[slot];

	for (i = 0; i < mpCcCityCount; i++)
	{
		if (mpCcCities[i] != city)
			continue;

		{
			int oldCity = mpCcCityIdx, oldModel = mpCcModelIdx, m;

			mpCcCityIdx = i;
			MpCcRebuild();		/* that city's roster */

			for (m = 0; m < mpCcModelCount; m++)
			{
				if (mpCcModels[m] == model)
				{
					mpCcModelIdx = m;
					/* the row shows the palette the car wears */
					defaultPlayerPalette = car_data[c].ap.palette;
					return;
				}
			}

			mpCcCityIdx = oldCity;	/* not offered: keep the previous pick */
			MpCcRebuild();
			mpCcModelIdx = oldModel;
			if (mpCcModelIdx < 0 || mpCcModelIdx >= mpCcModelCount)
				mpCcModelIdx = 0;
			return;
		}
	}
}

static const JER_PAUSE_MENU_ITEM mpChangeCarItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */
	{ NULL, MpCcLabelCity, MpCcAdjustCity, NULL, NULL, 1 },
	{ NULL, MpCcLabelCar, MpCcAdjustCar, NULL, NULL, 1 },
	{ NULL, MpCcLabelPalette, MpCcAdjustPalette, NULL, NULL, 1 },
	{ "Respawn as this car", NULL, MpCcApply, NULL, NULL, 0 },
	/* Repair belongs with the car, not under Player options: it is a car action and
	 * "Respawn as this car" next to it is the one that also MOVES you. */
	{ "Repair car", NULL, MpOptRepair, NULL, NULL, 0 },
	/* ...and so does clearing the wanted level: the pursuit follows the CAR, and
	 * this is the row a player looks for while a cop is on their bumper. It used to
	 * sit on the Multiplayer page, a submenu away from the car it is about. */
	{ "Clear felony", NULL, MpOptClearFelony, NULL, NULL, 0 },
};

/* A JER_PAUSE_MENU's item_count, taken FROM the array.
 *
 * The count is a separate field and the engine builds the submenu EAGERLY, so a
 * count larger than the array walks off its end -- the documented way opening the
 * Multiplayer page access-violates. Deriving it makes that unrepresentable rather
 * than something a later edit has to remember. */
#define MP_MENU_ITEMS(a)	(int)(sizeof(a) / sizeof((a)[0]))

static const JER_PAUSE_MENU mpChangeCarMenu =
{ "Vehicle Options", mpChangeCarItems, MP_MENU_ITEMS(mpChangeCarItems), "mp.car" };

static const JER_PAUSE_MENU_ITEM mpColorItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */
	{ NULL, MpColorLabelOn, MpColorToggleOn, NULL, NULL, 0 },
	{ NULL, MpColorLabelR, MpColorAdjustR, NULL, NULL, 1 },
	{ NULL, MpColorLabelG, MpColorAdjustG, NULL, NULL, 1 },
	{ NULL, MpColorLabelB, MpColorAdjustB, NULL, NULL, 1 },
};

static const JER_PAUSE_MENU mpColorMenu =
{ "Player Color", mpColorItems, MP_MENU_ITEMS(mpColorItems), "mp.colour" };

/* Preferred Character: the on-foot model picker, directly below the colour row
 * because the two are the player's identity. It is a CYCLER for the same reason
 * the car roster is one -- a module page cannot nest a second level -- and it
 * SKIPS characters this level cannot draw (Jericho's JERI_* models exist only in
 * Havana), so left/right never selects a broken skeleton. */
static void MpCharLabel(void* ud, char* out, int max)
{
	int selected = MpPedModelSelected();

	(void)ud;

	snprintf(out, max, "Preferred Character: %s", MpPedModelName(selected));
}

static int MpCharAdjust(void* ud, int dir)
{
	int model = MpPedModelSelected();
	int i, count;

	(void)ud;

	if (dir == 0)
		return JER_PAUSE_QUIT_NONE;

	/* A solo game does not warm the archive at level start when the stored
	 * character is Tanner, so this row is where the custom entries first become
	 * reachable. Importing here makes the count below include them. Sessions are
	 * already warm; the call is a fast no-op there. */
	MpPedArchiveEnsure();

	count = MpPedModelCount();

	/* Walk in the asked direction until a character this level can draw. With
	 * only Tanner available the row simply stays put. */
	for (i = 0; i < count; i++)
	{
		model = (model + dir + count) % count;

		if (MpPedModelSelectable(model))
		{
			MpSetPedModel(model);
			break;
		}
	}

	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM mpPlayerItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */
	{ "Player Color", NULL, NULL, NULL, &mpColorMenu, 0 },
	{ NULL, MpCharLabel, MpCharAdjust, NULL, NULL, 1 },
};

static const JER_PAUSE_MENU mpPlayerMenu =
{ "Player Options", mpPlayerItems, MP_MENU_ITEMS(mpPlayerItems), "mp.player" };

int MpOnPauseMenu(void* userdata, void* args)
{
	JER_ARGS_PAUSE_MENU* pm = (JER_ARGS_PAUSE_MENU*)args;

	(void)userdata;

	if (pm == NULL)
		return JER_RESULT_CONTINUE;

	/* Dying inside a session is a RESPAWN, not a game over. This is the engine asking
	 * whether to arm it, so the answer is the refusal itself: STOP leaves the pause
	 * unarmed and makes the engine drop the death fade, and by the time we answer the
	 * player is already back on the map. Refusing is the only half a module CAN do -
	 * the fade and the engine's WantPause are statics - which is why the hook exists. */
	if (pm->action == JER_PAUSE_GAMEOVER)
		return MpRespawnAfterDeath() ? JER_RESULT_STOP : JER_RESULT_CONTINUE;

	if (pm->action != JER_PAUSE_OPEN)
		return JER_RESULT_CONTINUE;

	/* While the chat prompt is up, START is the player typing RETURN -- claim it,
	 * so the engine pause does not open over the prompt. */
	if (gMp.chatOpen)
		return JER_RESULT_STOP;

	if (!gMp.running)
	{
		/* SINGLE PLAYER: there is no session and no world of ours to keep running,
		 * but the Change car page is the SAME page (it is registered
		 * unconditionally) and its roster needs the same refresh -- the city list is
		 * carhacks' answer, which may have changed since the menu was last opened. */
		MpCcRebuild();
		MpCcSnapToOwnCar();

		return JER_RESULT_CONTINUE;
	}

	/* Let the ENGINE open its pause menu -- do not claim START. The world keeps
	 * running because MpOnDrawOverlay steps it while paused, and our player list
	 * (and MP options) ride on top of the menu the player expects to see.
	 *
	 * Record that it is OUR pause that is opening: the player list is gated on this
	 * plus gDrawPauseMenus, so the other pause-MODE screens (game over, mission
	 * complete, pad error) and cutscenes cannot put it on screen. */
	gMpPauseOpened = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] pause menu opened; the world keeps running\n");

	/* Refresh the Change car picker now, not while it is being drawn: the roster
	 * reads the city list (which a carhacks set may have changed since the last
	 * time the menu was opened) and the city's frontend car list. */
	MpCcRebuild();
	MpCcSnapToOwnCar();

	return JER_RESULT_CONTINUE;
}

extern void UnPauseSound(void);	/* the engine pauses music/sfx with the world */

/* The pause menu's ANSWER, in a live match.
 *
 * JERICHO hands a module the action before the engine runs it (JER_EVENT_GAME_QUIT,
 * fired in main.c), and that hook is what makes a multiplayer Restart possible at
 * all: the stock one calls EndGame(GAMEMODE_RESTART) and rebuilds the level, which
 * in a match takes every OTHER player's session down with it -- one player's
 * "restart" is not theirs to do to everyone.
 *
 * So mp CLAIMS restart and turns it into a RESPAWN instead: the engine's main
 * pause menu relabels that item "Respawn" (gJerPauseRestartAsRespawn), and here the
 * claimed restart becomes "back onto the nearest road, repaired" rather than the
 * soft reset -- one player's restart must not rebuild the level under everyone.
 * Claiming means the engine runs none of its endings, so we also unpause -- the
 * pause menu has already closed itself by the time this fires.
 *
 * Quit and the rest of the codes are left alone: leaving the match is a real thing
 * a player should be able to do from the menu. */
int MpOnGameQuit(void* userdata, void* args)
{
	JER_ARGS_GAME_QUIT* q = (JER_ARGS_GAME_QUIT*)args;

	(void)userdata;

	if (q == NULL || !gMp.running)
		return JER_RESULT_CONTINUE;

	/* The JER_PAUSE_QUIT_* values mirror the engine's menu quit codes
	 * (jer_pause_menu.h), and this event carries the engine's. */
	if (q->code != JER_PAUSE_QUIT_RESTART)
		return JER_RESULT_CONTINUE;

	/* ALWAYS claimed in a session: falling through to the stock Restart rebuilt the
	 * level on this machine alone, wiping its cars and its player out of the match.
	 * A respawn that could not place us says so (MpRespawnOnRoad notifies) and leaves
	 * us where we are. */
	MpRespawnOnRoad();

	pauseflag = 0;
	UnPauseSound();

	return JER_RESULT_STOP;
}

/* ------------------------------------------------------------------ */
/* Kick player: a target cycler + a confirm row, the same shape as the Change
 * car page. HOST ONLY -- on a client both rows say so and do nothing, because
 * only the host owns the connection table (and MpKickPlayer is a no-op there
 * anyway; the menu says it rather than looking broken). The confirm row IS the
 * confirmation screen: a kick takes a deliberate second press on a row that
 * names who is about to go. */
static int mpKickTarget = -1;	/* registry row of the selected target, or -1 */

/* The next kickable row from `from` in `dir`: a connected player that is not us
 * and not the host. Walks the registry, so a player who leaves drops out of the
 * cycle by himself. */
static int MpKickNext(int from, int dir)
{
	int i;

	for (i = 1; i <= MP_MAX_PLAYERS; i++)
	{
		int idx = (from + dir * i + MP_MAX_PLAYERS * 2) % MP_MAX_PLAYERS;

		if (gMp.players[idx].active && gMp.players[idx].connected &&
			!gMp.players[idx].isHost && !gMp.players[idx].isLocal)
			return idx;
	}

	return -1;
}

/* The effective target, repairing a stale/never-set one. */
static int MpKickCurrent(void)
{
	if (!MpIsHost())
		return -1;

	if (mpKickTarget < 0 || mpKickTarget >= MP_MAX_PLAYERS ||
		!gMp.players[mpKickTarget].active || !gMp.players[mpKickTarget].connected)
		mpKickTarget = MpKickNext(MP_MAX_PLAYERS - 1, 1);

	return mpKickTarget;
}

static const char* MpKickName(int idx)
{
	return (idx >= 0 && gMp.players[idx].name[0] != '\0') ? gMp.players[idx].name : "?";
}

static void MpKickLabelTarget(void* ud, char* out, int max)
{
	int t = MpKickCurrent();

	(void)ud;

	if (!MpIsHost())
		snprintf(out, max, "Kick: (host only)");
	else if (t < 0)
		snprintf(out, max, "Kick: (no other players)");
	else
		snprintf(out, max, "Kick: %s", MpKickName(t));
}

static int MpKickAdjust(void* ud, int dir)
{
	int t = MpKickCurrent();

	(void)ud;

	if (t >= 0)
		mpKickTarget = MpKickNext(t, dir);

	return JER_PAUSE_QUIT_NONE;
}

static void MpKickLabelConfirm(void* ud, char* out, int max)
{
	int t = MpKickCurrent();

	(void)ud;

	if (!MpIsHost())
		snprintf(out, max, "Confirm kick (host only)");
	else if (t < 0)
		snprintf(out, max, "Confirm kick (nobody selected)");
	else
		snprintf(out, max, "Confirm kick %s", MpKickName(t));
}

static int MpKickConfirm(void* ud, int dir)
{
	int t = MpKickCurrent();

	(void)ud;
	(void)dir;

	if (t >= 0)
		MpKickPlayer(gMp.players[t].id);

	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM mpKickItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */
	{ NULL, MpKickLabelTarget, MpKickAdjust, NULL, NULL, 1 },
	{ NULL, MpKickLabelConfirm, MpKickConfirm, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU mpKickMenu =
{ "Kick player", mpKickItems, MP_MENU_ITEMS(mpKickItems), "mp.kick" };

/* ------------------------------------------------------------------ */
/* Warp to Player: put this machine's car -- or our own pedestrian when we are on
 * foot -- down near another player's, out of the path they are committed to. The
 * PLACEMENT is mp_session.c's -- it needs InitCarPhysics, MapHeight and the spool
 * call, which all live with the respawn -- so all this file owns is the picker. */
static int gWarpTarget;		/* which OTHER player the row is pointing at */

static int MpWarpCount(void)
{
	int i, n = 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];

		if (p->active && p->id != gMp.localPlayerId)
			n++;
	}

	return n;
}

static MP_PLAYER* MpWarpPlayerAt(int index)
{
	int i, n = 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];

		if (!p->active || p->id == gMp.localPlayerId)
			continue;

		if (n == index)
			return p;

		n++;
	}

	return NULL;
}

/* The list is rebuilt every time it is READ rather than kept as state: a player who
 * leaves while the menu is open changes it, and an index into a stale list is how
 * a menu sends you to somebody who is no longer in the match. */
static void MpWarpLabelTarget(void* ud, char* out, int max)
{
	int n = MpWarpCount();
	MP_PLAYER* p;

	(void)ud;

	if (n == 0)
	{
		snprintf(out, max, "Target: (nobody else is here)");
		return;
	}

	if (gWarpTarget < 0 || gWarpTarget >= n)
		gWarpTarget = 0;

	p = MpWarpPlayerAt(gWarpTarget);

	snprintf(out, max, "Target: %s", (p != NULL) ? p->name : "?");
}

static int MpWarpAdjustTarget(void* ud, int dir)
{
	int n = MpWarpCount();

	(void)ud;

	if (n > 0)
		gWarpTarget = ((gWarpTarget + dir) % n + n) % n;

	return JER_PAUSE_QUIT_NONE;
}

static int MpWarpGo(void* ud, int dir)
{
	int n = MpWarpCount();
	MP_PLAYER* p;

	(void)ud;
	(void)dir;

	if (n == 0)
	{
		MpNotify("Warp: nobody else is in the match");
		return JER_PAUSE_QUIT_NONE;
	}

	if (gWarpTarget < 0 || gWarpTarget >= n)
		gWarpTarget = 0;

	p = MpWarpPlayerAt(gWarpTarget);

	if (p != NULL)
		MpWarpToPlayer(p->id);

	return JER_PAUSE_QUIT_NONE;
}

static const JER_PAUSE_MENU_ITEM mpWarpItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */
	{ NULL, MpWarpLabelTarget, MpWarpAdjustTarget, NULL, NULL, 1 },
	{ "Warp to this player", NULL, MpWarpGo, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU mpWarpMenu =
{ "Warp to Player", mpWarpItems, MP_MENU_ITEMS(mpWarpItems), "mp.warp" };

/* ------------------------------------------------------------------ */
/* The local player's own switches: the same engine flags the host's SESSION RULES
 * drive (gInvincibleCar / gPlayerImmune), for whoever is sitting at THIS machine,
 * plus two one-shot engine repairs. Deliberately NOT synced -- invincibility
 * belongs to the person at this keyboard, and the car/health the one-shots touch
 * is this machine's own.
 *
 * Note the two overlap on purpose: a host that turns a rule ON for the session
 * has it in force for everyone (MpSessionRulesTick forces it), while a host that
 * leaves it OFF is leaving it to each player's own switch here. That is why they
 * are on the top-level page and not one click deeper. */
/* The Respawn row: back onto the nearest road, repaired and upright. The work is
 * mp_session.c's -- it needs InitCarPhysics, MapHeight and the spool call, which
 * all live with the respawn and the warp -- so this file owns only the door. */
static int MpRespawn(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	MpRespawnOnRoad();

	return JER_PAUSE_QUIT_NONE;
}

static void MpOptLabelInvincible(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Invincibility: %s", gInvincibleCar ? "ON" : "off");
}

int MpOptToggleInvincible(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gInvincibleCar ^= 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] player option: invincible car %s\n",
			gInvincibleCar ? "ON" : "off");

	return JER_PAUSE_QUIT_NONE;
}

static void MpOptLabelImmune(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Immunity: %s", gPlayerImmune ? "ON" : "off");
}

int MpOptToggleImmune(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gPlayerImmune ^= 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] player option: player immunity %s\n",
			gPlayerImmune ? "ON" : "off");

	return JER_PAUSE_QUIT_NONE;
}

/* Same field the JMP Options row writes (AdjSuperTurbo in mp_ui.c). Local
 * only: MpConfigSave persists super_turbo, and nothing here packs it into
 * MP_RULES / WELCOME. Cross toggles, like Invincibility and Immunity on this
 * page; left/right flips it too, like Strict Version. */
static void MpOptLabelSuperTurbo(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Super Turbo: %s", gMp.config.superTurbo ? "On" : "Off");
}

static int MpOptToggleSuperTurbo(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	/* 0/1 only, so Cross (dir 0) and left/right (±1) are the same flip.
	 * JMP Options' AdjSuperTurbo writes this same field and saves it. */
	gMp.config.superTurbo = gMp.config.superTurbo ? 0 : 1;
	MpConfigSave();

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] player option: super turbo %s\n",
			gMp.config.superTurbo ? "On" : "Off");

	return JER_PAUSE_QUIT_NONE;
}

/* Same field the JMP Options row writes (AdjJoinLeaveSounds in mp_ui.c). */
static void MpOptLabelJoinLeaveSounds(void* ud, char* out, int max)
{
	(void)ud;
	snprintf(out, max, "Join/leave sounds: < %s >", gMp.config.joinLeaveSounds ? "On" : "Off");
}

static int MpOptToggleJoinLeaveSounds(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	gMp.config.joinLeaveSounds = gMp.config.joinLeaveSounds ? 0 : 1;
	MpConfigSave();

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] player option: join/leave sounds %s\n",
			gMp.config.joinLeaveSounds ? "On" : "Off");

	return JER_PAUSE_QUIT_NONE;
}

int MpOptRepair(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	MpSelfRepairCar();

	return JER_PAUSE_QUIT_NONE;
}

int MpOptClearFelony(void* ud, int dir)
{
	(void)ud;
	(void)dir;

	MpSelfClearFelony();

	return JER_PAUSE_QUIT_NONE;
}

/* Reload every imported car: carhacks releases each imported slot's assets
 * (pins, pool pages, palette rows, geometry and the city buffers that were their
 * last user), re-applies the set and rebuilds every one of them. Stock/level cars
 * are untouched. This is a full unload/reload, not another hot load over the top,
 * so a stale asset is actually given back and read again. */
int chkImportReloadAll(void);	/* carhacks/carimport.h */

static int MpMenuReloadCars(void* ud, int dir)
{
	int n;

	(void)ud; (void)dir;

	n = chkImportReloadAll();

	printInfo("[mp] Reload all cars: %d imported slot(s) rebuilt\n", n);
	return 0;
}

static const JER_PAUSE_MENU_ITEM mpPauseItems[] =
{
	/* label, get_label, on_activate, userdata, submenu, adjust */

	/* Respawn first, then the two things that shape a player: their car and
	 * their character. Player Color and Preferred Character live one level down
	 * in Player Options, together with the colour they depend on. Then the
	 * movement row, the local switches, and the admin/debug rows last.
	 * Super Turbo is the saved switch (mp.ini); the two above it are the live
	 * cheat flags and are not written out. */
	{ "Respawn", NULL, MpRespawn, NULL, NULL, 0 },
	{ "Vehicle Options", NULL, NULL, NULL, &mpChangeCarMenu, 0 },
	{ "Player Options", NULL, NULL, NULL, &mpPlayerMenu, 0 },
	{ "Warp To Player", NULL, NULL, NULL, &mpWarpMenu, 0 },
	{ NULL, MpOptLabelInvincible, MpOptToggleInvincible, NULL, NULL, 0 },
	{ NULL, MpOptLabelImmune, MpOptToggleImmune, NULL, NULL, 0 },
	{ NULL, MpOptLabelSuperTurbo, MpOptToggleSuperTurbo, NULL, NULL, 1 },
	{ NULL, MpOptLabelJoinLeaveSounds, MpOptToggleJoinLeaveSounds, NULL, NULL, 1 },
	{ "Kick player", NULL, NULL, NULL, &mpKickMenu, 0 },
	{ "Reload all cars", NULL, MpMenuReloadCars, NULL, NULL, 0 },
	{ "Write diagnostics now", NULL, MpMenuWriteDiag, NULL, NULL, 0 },
};

static const JER_PAUSE_MENU mpPauseMenu =
{ "Multiplayer", mpPauseItems, MP_MENU_ITEMS(mpPauseItems), "mp.page" };

/* DEPTH PROOF, for the test harness: MP_DEEP_MENU=1 registers a page whose tree
 * nests one level PAST the bridge's cap, so the refusal path can be SEEN in a log
 * instead of reasoned about. Nothing is registered without the variable, so a
 * normal run carries these tables unused.
 *
 * A seven-level chain against a cap of five: levels 0..5 build and level 6 is the
 * one that is refused, which is exactly the boundary worth pinning -- an off-by-one
 * there would let a menu the navigation stack cannot hold be built. */
#define MP_DEEP_LEVELS 7

static const JER_PAUSE_MENU_ITEM kDeepItems6[] =
{
	{ "the last level", NULL, NULL, NULL, NULL, 0 },
};
static const JER_PAUSE_MENU kDeepMenu6 = { "Deep 6", kDeepItems6, MP_MENU_ITEMS(kDeepItems6) };

static const JER_PAUSE_MENU_ITEM kDeepItems5[] =
{
	{ "deeper", NULL, NULL, NULL, &kDeepMenu6, 0 },
};
static const JER_PAUSE_MENU kDeepMenu5 = { "Deep 5", kDeepItems5, MP_MENU_ITEMS(kDeepItems5) };

static const JER_PAUSE_MENU_ITEM kDeepItems4[] =
{
	{ "deeper", NULL, NULL, NULL, &kDeepMenu5, 0 },
};
static const JER_PAUSE_MENU kDeepMenu4 = { "Deep 4", kDeepItems4, MP_MENU_ITEMS(kDeepItems4) };

static const JER_PAUSE_MENU_ITEM kDeepItems3[] =
{
	{ "deeper", NULL, NULL, NULL, &kDeepMenu4, 0 },
};
static const JER_PAUSE_MENU kDeepMenu3 = { "Deep 3", kDeepItems3, MP_MENU_ITEMS(kDeepItems3) };

static const JER_PAUSE_MENU_ITEM kDeepItems2[] =
{
	{ "deeper", NULL, NULL, NULL, &kDeepMenu3, 0 },
};
static const JER_PAUSE_MENU kDeepMenu2 = { "Deep 2", kDeepItems2, MP_MENU_ITEMS(kDeepItems2) };

static const JER_PAUSE_MENU_ITEM kDeepItems1[] =
{
	{ "deeper", NULL, NULL, NULL, &kDeepMenu2, 0 },
};
static const JER_PAUSE_MENU kDeepMenu1 = { "Deep 1", kDeepItems1, MP_MENU_ITEMS(kDeepItems1) };

static const JER_PAUSE_MENU_ITEM kDeepItems0[] =
{
	{ "MP_DEEP_MENU: nest past the cap", NULL, NULL, NULL, &kDeepMenu1, 0 },
};
static const JER_PAUSE_MENU kDeepMenu0 = { "Deep 0", kDeepItems0, MP_MENU_ITEMS(kDeepItems0) };

/* Our own page in the pause screen ("Modules" -> "Multiplayer"). NOTE: the
 * declared item_count must equal the array length -- the engine builds the
 * submenu eagerly, so a count that is too large reads past the end. (The
 * MP_MENU_ITEMS macro derives it from the array, so the two cannot drift.)
 * The depth proof rides along only when asked for. */
void MpMenuRegister(void)
{
	jer_pause_menu_register(&mpPauseMenu);

	if (getenv("MP_DEEP_MENU") != NULL)
		jer_pause_menu_register(&kDeepMenu0);
}
