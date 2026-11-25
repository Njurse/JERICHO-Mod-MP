/*
 * mp_cars.c -- split out of mp_session.c
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"

/* ------------------------------------------------------------------ */
/* ROLE CARS (0.23.1): one car per role, picked on join                 */
/*                                                                      */
/* A mode with role slots (mp_mode.h MP_MODE_ROLE_SLOT; Cops n Robbers: */
/* PATROL CAR from the police cars, GETAWAY CAR from the rest) asks the */
/* join car select for one pick per slot (jer_carquery.h JER_CARQ_ROLES; */
/* carhacks' menu walks them, each step's roster filtered to its pool), */
/* and is told each pick (JER_CARQ_ROLEPICK). Only the LAST step        */
/* launches, so a player is not "car ready" for waitForCars           */
/* (MpPlayerCarReady / carConfirmed) until every role has a car.        */
/* The picks are this player's own and live in memory for THIS        */
/* CONNECTION only (0.23.2): every round and a server city change (the  */
/* MP_KICK_FLAG_REJOIN leave + auto-rejoin) keep them; leaving, a lost  */
/* connection, a kick, the host ending the match, or a rejoin that      */
/* gives up forgets them (MpRolePicksForget), so the next join asks for */
/* every role again. Nothing is written to mp's config. At every       */
/* round's COUNTDOWN MpRoleCarApply puts the player in the car of the   */
/* role they have this round, through the pause picker's own change-car */
/* door. A Change car pick made in the pause menu becomes that role's   */
/* pick.                                                                */
/* ------------------------------------------------------------------ */
typedef struct MP_ROLE_PICK
{
	int set;
	int city;	/* the roster city the car was picked from */
	int model;
	int palette;
} MP_ROLE_PICK;

#define MP_CHANGE_RETRY_EVERY	5	/* retry every N frames, not every frame */
#define MP_CHANGE_WAIT_FRAMES	90	/* ~3.5 s at 25 fps */
#define MP_CRUSH_HALF_MAX	20
#define MP_CRUSH_HALF_MIN	2
#define MP_ENTER_ADOPT_DIST	1536	/* world units: the owner's car can have moved a little */

/* THE RELOAD FAILSAFE.
 *
 * A car reload (a change, "Reload all cars", the verifier's repair) releases the
 * resident slot the local car is drawn from and rebuilds it. For the frames in
 * between, ap.model names a slot with no mesh: the chase camera has no car to sit
 * behind and spins, and the player falls through the map while the world keeps
 * stepping. The proper car comes back on its own; what is missing is a body to
 * stand in for it.
 *
 * So: if the car we are MEANT to drive is not loadable right now, put the host
 * city's car 1 (the first civilian its own level ships) under the player and keep
 * the session told the real choice. When the wanted slot becomes usable, put it
 * back. This is a stopgap, not a car: it is local-only, it logs both transitions,
 * and it never changes what the player's carstate says they drive. */
static int sMpWantSlot = -1;

/* Lean retry for a car that stays missing: while the stand-in is up, ask for the wanted
 * car again every MP_RELOAD_RETRY_FRAMES (~4 s at 25 fps), at most MP_RELOAD_RETRY_MAX
 * times per outage, so a car that failed to load once gets another go. */
#define MP_RELOAD_RETRY_FRAMES	100
#define MP_RELOAD_RETRY_MAX	5
static int sMpReloadWait;
static int sMpReloadTries;

static int sMpStandInCar[MAX_CARS];	/* per car object: drawn as a grey stand-in */

static int sMpReadySlot[MAX_CARS];	/* readiness stand-in: wanted slot + 1, 0 = none */

static int sMpReadyHost[MAX_CARS];	/* the host slot the grey body sits on */

static u_char sMpReadyPalette[MAX_CARS];	/* palette to put back on the real car */

static int sMpCamReseat;

static int sMpStandInModel = -1;	/* the placeholder's resident slot, -1 = host car 1 */

/* The last (city, model) we reported NOT holding for a remote player, so "this
 * machine does not have their car" is one line per CHANGE, not per snapshot --
 * a carstate arrives every frame. `Set` is 0 until we have logged one. */
static int sMpCarKeepCity[MP_MAX_PLAYERS];

static int sMpCarKeepModel[MP_MAX_PLAYERS];

static unsigned char sMpCarKeepSet[MP_MAX_PLAYERS];

/* Make this player's car in OUR world the vehicle their owner just got into.
 *
 * IN PLACE, on the slot we already drive for them -- ONLY the cosmetic model and
 * the colour change, so nothing about this world's car slots is disturbed.
 *
 * Do NOT move the player onto the car the owner NAMED. Slot numbers do not mean
 * the same car on two machines (traffic is not replicated), so doing that warps
 * the player into whatever unrelated car happens to sit at that index here --
 * seen as "the host teleported into the client's old car and the client ended up
 * warped in as a traffic car". The hijacked car is also owned by the LOCAL traffic
 * system, which then tries to recycle it and crashes (PingInCivCar / StepSim).
 * Matching the vehicle is the job here; matching the ENTITY needs replicated
 * traffic, which the session does not have.
 *
 * The owner names the car as (city, model) -- the pair the wire carries -- and
 * THIS machine resolves it to its own resident slot (MpResidentSlotForCar). A
 * slot number is never taken off the wire. Returns 1 if the car was changed, 0
 * when this machine cannot hold it (not resident, or its mesh is not loaded). */
/* The reusable core of a car change: re-model the car in `carSlot` as (city, model).
 *
 * `who` is the roster row doing the changing, or NULL in SINGLE PLAYER, where there
 * is no roster at all -- the pause menu's Change car row is registered
 * unconditionally, so the same row has to work in a solo game. Session-free on
 * purpose: everything session-shaped (the identity bookkeeping, the keep-gate, the
 * per-player log wording) is gated on `who`; everything that decides whether the
 * car CAN be changed is not. Returns 1 when the car was changed. */
/* The model each car's dentable verts were last built from. A slot reused IN PLACE
 * (carhacks' retiring-slot handover) keeps the same slot NUMBER while its MODEL changes,
 * so `was == slot` alone must not skip the rebuild: that would draw the new model's
 * polygons over the old model's vertices (the garbled-car bug). -1 = never rebuilt. */
static int gMpAdoptModel[MAX_CARS];

static int gMpAdoptModelInit;

static MP_ROLE_PICK gMpRolePick[MP_MODE_ROLE_SLOTS_MAX];

static int gMpRolePickMode = -1;	/* the mode whose picks gMpRolePick holds */

static struct
{
	int active;
	int city;
	int model;
	int framesLeft;
} gMpCarChangeWait;

void MpSetCarCrushScale(int carId, int scale);
int MpCarSlotTakenByOther(const MP_PLAYER* leaver, int slot);
const char* MpCarCityName(int city);
int MpResidentSlotForCar(int city, int model);
static int MpHostCarOneSlot(void);
int MpCarQueryStandIn(void);
static int MpCarStandIn(int carSlot);
static void MpCarClearStandIn(int carSlot);
static int MpAdoptCar(int carSlot, int city, int model, MP_PLAYER* who);
int MpAdoptRemoteCar(MP_PLAYER* p, int city, int model);
int MpCarQueryLoad(int city, int model);
int MpCarQuerySlots(int city, int* slots, int* models, int max);
static int MpCarListForCityAll(int city, int* slots, int* models, int max);
static void MpRolePicksFor(int mode);
static int MpRolePickStore(int mode, int slot, int city, int model, int palette, const char* from);
void MpCarQueryChosen(int city, int model, int changed);
const char* MpChangeCarRefusal(int city, int model);
static int MpChangeCarRun(int city, int model, int isRetry);

/* Called from MpSessionReset: this module's state is per session, so
 * the next match starts from the same values the module loaded with. */
void MpCarsReset(void)
{
	/* no session state of its own */
}

void MpFailsafeReset(void)
{
	sMpReloadWait = 0;
	sMpReloadTries = 0;
	sMpWantSlot = -1;
	sMpWantCity = -2;
	sMpWantModel = -1;
	sMpStandIn = 0;
	sMpCamReseat = 0;
	memset(sMpStandInCar, 0, sizeof(sMpStandInCar));
	memset(sMpReadySlot, 0, sizeof(sMpReadySlot));
	memset(sMpReadyHost, 0, sizeof(sMpReadyHost));
	memset(sMpReadyPalette, 0, sizeof(sMpReadyPalette));
}

void MpFailsafeNoteCar(int slot)
{
	if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS)
		return;

	sMpWantSlot = slot;
	sMpWantModel = residentCarModels[slot];
	sMpWantCity = GetCarModelSourceCity(slot);
	sMpStandIn = 0;
}

uint8_t MpCrushHalfFromScale(int scale)
{
	int half = (scale + 1024) / 2048;	/* 4096-fixed -> 0.5 steps */

	if (half < MP_CRUSH_HALF_MIN)
		half = MP_CRUSH_HALF_MIN;
	else if (half > MP_CRUSH_HALF_MAX)
		half = MP_CRUSH_HALF_MAX;

	return (uint8_t)half;
}

int MpCrushScaleFromHalf(uint8_t half)
{
	int h = (int)half;

	if (h < MP_CRUSH_HALF_MIN)
		h = MP_CRUSH_HALF_MIN;
	else if (h > MP_CRUSH_HALF_MAX)
		h = MP_CRUSH_HALF_MAX;

	return h * 2048;	/* back to 4096 fixed point */
}

/* Tell crumple what an owner says a car's multiplier is (4096 = 1.0x). */
void MpSetCarCrushScale(int carId, int scale)
{
	JER_CRUMPLE_SCALE_ARGS a;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	memset(&a, 0, sizeof(a));
	a.carId = carId;
	a.scale = scale;

	jer_fire(JER_CRUMPLE_SET_SCALE, &a);
}

/* A remote player got OUT. The car we were driving for them is left exactly as
 * it is, standing where they left it.
 *
 * DELIBERATELY NOT handed to the traffic AI. That car is one the MOD created
 * (InitPlayer), so its civil-AI state was never set up; flipping its controlType
 * to CIV_AI hands it to the engine's traffic AI, which then steers a car with no
 * AI data -- an access violation inside CivSteerAngle (found from a dump:
 * JERICHO_dev.exe rva 0xC961). Leaving it a player car and simply never sending
 * it anything again does what was asked for ANYWAY: it stays put (the engine's
 * input fallback coasts it to a stop) instead of an AI driving it away. */
void MpReleaseRemoteCar(MP_PLAYER* p)
{
	CAR_DATA* cp;

	if (p == NULL || p->carId < 0 || p->carId >= MAX_CARS)
		return;

	/* The car stops being driven by its owner here (on foot, or gone): its
	 * body-crush multiplier goes back to the default, so a body that is later
	 * reused cannot inherit the previous driver's setting. */
	MpSetCarCrushScale(p->carId, 4096);

	/* The body stops being ours: its sample timeline must go with it. */
	MpPlayClear(p->id);

	cp = &car_data[p->carId];

	if (cp->controlType == CONTROL_TYPE_PLAYER)
	{
		/* Leave OUR copy as the stopped, empty civ car the owner's own engine
		 * made it (the local get-out path does the same). Left as
		 * CONTROL_TYPE_PLAYER it could never be entered by anyone else here
		 * (TannerCanEnterCar takes CIV_AI only), and it kept a player slot
		 * the late-join seat scan skips. */
		cp->controlType = CONTROL_TYPE_CIV_AI;
		cp->ai.c.thrustState = CIV_AI_THRUST_STOP;
		cp->ai.c.ctrlState = CIV_AI_CTRL_EMPTY;
		cp->wheel_angle = 0;
		cp->ai.padid = &gMpQuietPad;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d got out; car slot %d left standing where it was (enterable)\n",
				p->id, p->carId);
	}
}

/* Some OTHER player (or us) is now in this slot: it belongs to them. */
int MpCarSlotTakenByOther(const MP_PLAYER* leaver, int slot)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		const MP_PLAYER* o = &gMp.players[i];

		if (o == leaver || !o->active)
			continue;

		/* DRIVING IT, OR PARKED IT AND STILL STANDING NEXT TO IT.
		 *
		 * carId alone missed the commandeer-then-get-out case: the new owner's
		 * row has carId < 0 but leftCarSlot1 names this body, and the body is
		 * CIV_AI + the quiet pad, so every other test calls it free. The original
		 * owner's spawn then took it for an abandoned duplicate and deleted the
		 * car its new owner could still walk back into. A left-car claim is
		 * ownership here for the same reason MpCarSlotOwnedByPlayer treats it as
		 * ownership when choosing a slot. */
		if (o->carId == slot || o->leftCarSlot1 == slot + 1)
			return 1;
	}

	return 0;
}

/* A player is leaving (LEAVE, timeout, or dropped from the roster): take their
 * stand-in Tanner away, and the car they abandoned on foot, unless another
 * player has commandeered it since -- then it is that player's car and stays.
 * Their CURRENT car is handled by MpRemovePlayer (same ownership check). */
void MpCleanupLeaverWorld(MP_PLAYER* p)
{
	int slot;

	if (p == NULL || p->isLocal)
		return;

	if (p->ped != NULL)
	{
		jer_npc_despawn((JerNpc*)p->ped);
		p->ped = NULL;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] ped: player %d left; stand-in removed\n", p->id);
	}

	slot = p->leftCarSlot1 - 1;

	if (slot >= 0 && slot < MAX_CARS && slot != p->carId &&
		car_data[slot].controlType != CONTROL_TYPE_NONE &&
		car_data[slot].ap.model == p->leftCarModel &&
		!MpCarSlotTakenByOther(p, slot))
	{
		MpReleaseOurCarSlot(slot);
		car_data[slot].controlType = CONTROL_TYPE_NONE;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] removed player %d's abandoned car (slot %d)\n", p->id, slot);
	}

	p->leftCarSlot1 = 0;
}

/* A city's name, for logs and menu labels. `city` is a 0..3 index or -1 for the
 * session's own city (the value the wire uses for "the level's own car data"). */
const char* MpCarCityName(int city)
{
	/* JERICHO: ask the ENGINE, do not keep a local copy.
	 *
	 * This was a hardcoded names[4] = CHICAGO/HAVANA/VEGAS/RIO, which is why the
	 * pause menu city picker could not name a Driver 1 car-data city even once
	 * carhacks started offering them: the COUNT already came from carhacks through
	 * JER_CARQ_CITIES, and the NAMES did not, so the picker cycled to a real city
	 * and displayed "the session city" instead.
	 *
	 * LevelNames is the engine's own registry, so it grows when the engine does -
	 * the same reason carselect.c now reads CITY_COUNT rather than a literal 4.
	 * A city the engine has no row for is still reported as the session city,
	 * rather than as a name that does not exist. */

	if (city < 0 || city >= CITY_COUNT || LevelNames[city] == NULL)
		return "the session city";

	return LevelNames[city];
}

/* The city name for a ROW of somebody else's server: the engine's own name, or
 * "-" for a city the engine has no row for.
 *
 * Deliberately NOT MpCarCityName: that answers "the session city" for a missing
 * row, which is right for a local picker (where the player's own city is the
 * alternative) and meaningless in a list of servers that are not ours. */
const char* MpCityLabel(int city)
{
	if (city < 0 || city >= CITY_COUNT || LevelNames[city] == NULL)
		return "-";

	return LevelNames[city];
}

/* The local resident SLOT that holds (city, model) on THIS machine, or -1 when
 * no resident slot does. `city` is -1 for the SESSION's own city (a slot whose
 * car data is the level's own) or a 0..3 city index for a cross-city import.
 *
 * A model NUMBER alone is not enough: every city ships CARMODEL_0..12 and the
 * same number is a DIFFERENT vehicle in each, so a number is matched together
 * with the city its data came from (GetCarModelSourceCity). This is what turns
 * a peer's (city, model) into a slot HERE, where the slot numbers may differ. */
int MpResidentSlotForCar(int city, int model)
{
	int levelCity = -1;
	int slot, i;

	if (model < 0)
		return -1;

	/* WHICH CITY INDEX IS THIS LEVEL'S OWN. Needed because the two ends spell "the
	 * level's own city" differently: a model the level ships has
	 * GetCarModelSourceCity(slot) < 0 here, while the wire carries the owner's city by
	 * INDEX. So the host driving its own level's car arrives as (RIO, 8) and the slot to
	 * match has src < 0 - and without this the client resolved nothing, kept its own car
	 * and logged "player 0 drives RIO model 8, but this machine draws ... (theirs is not
	 * loaded here)" for exactly the cars the level DOES hold. */
	for (i = 0; i < 4; i++)
	{
		if (LevelNames[i] != NULL && GameLevel >= 0 && LevelNames[GameLevel] != NULL &&
			strcmp(LevelNames[i], LevelNames[GameLevel]) == 0)
			levelCity = i;
	}

	for (slot = 0; slot < MAX_CAR_RESIDENT_MODELS; slot++)
	{
		int src;

		if (residentCarModels[slot] != model)
			continue;

		src = GetCarModelSourceCity(slot);

		if (city < 0)
		{
			if (src < 0)
				return slot;
		}
		else if (src == city || (src < 0 && city == levelCity))
		{
			return slot;		/* the same car, spelled two ways */
		}
	}

	return -1;
}

/* The host city's own car 1 (or, failing that, its first civilian), in a resident
 * slot that is really loaded. This is the body the reload failsafe puts under the
 * player while the chosen car is being rebuilt. */
static int MpHostCarOneSlot(void)
{
	int i;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		if (residentCarModels[i] == 1 && GetCarModelSourceCity(i) < 0 && JerCarSlotUsable(i))
			return i;

	for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
		if (residentCarModels[i] >= 0 && GetCarModelSourceCity(i) < 0 && JerCarSlotUsable(i))
			return i;

	return -1;
}

/* Ask the car mods for a resident slot holding the stand-in body (the placeholder
 * car). -1 = none (no bank, or no spare slot), and mp falls back to host car 1. */
int MpCarQueryStandIn(void)
{
	JER_CARQ_STANDIN_ARGS a;

	memset(&a, 0, sizeof(a));
	a.slot = -1;
	jer_fire(JER_CARQ_STANDIN, &a);
	return a.slot;
}

int MpStandInIsPlaceholder(void)
{
	return sMpStandInModel >= 0;
}

/* Put a stand-in body under a car object that has no mesh: the placeholder when
 * the car mods can place it, else the host city's car 1. The grey draw-colour
 * hook only paints the car-1 fallback - the placeholder is already distinct.
 * `fresh` says the flag was not already set, i.e. this is the transition to log. */
static int MpCarStandIn(int carSlot)
{
	extern u_char defaultPlayerPalette;
	CAR_DATA* cp;
	int hostSlot;
	int fresh;
	int standIn;

	if (carSlot < 0 || carSlot >= MAX_CARS)
		return 0;

	standIn = MpCarQueryStandIn();
	hostSlot = (standIn >= 0) ? standIn : MpHostCarOneSlot();
	sMpStandInModel = standIn;

	if (hostSlot < 0)
		return 0;

	cp = &car_data[carSlot];

	fresh = !sMpStandInCar[carSlot];
	sMpStandInCar[carSlot] = 1;

	if (cp->ap.model != hostSlot || !JerCarSlotUsable(cp->ap.model))
	{
		cp->ap.model = hostSlot;
		cp->ap.carCos = &car_cosmetics[hostSlot];
		cp->lowDetail = -1;
		cp->ap.palette = (u_char)CarSlotClampPalette(hostSlot, (int)defaultPlayerPalette);
		CreateDentableCar(cp);
	}

	/* The chase camera must follow a body that exists. If this is OUR car, re-seat
	 * it on the stand-in; a camera left on the released slot is the spin. */
	if (carSlot == player[0].playerCarId)
	{
		player[0].cameraCarId = cp->id;
		player[0].cameraAngle = cp->hd.direction + 1536;
	}

	return fresh;
}

static void MpCarClearStandIn(int carSlot)
{
	if (carSlot >= 0 && carSlot < MAX_CARS)
		sMpStandInCar[carSlot] = 0;
}

int MpCarIsStandIn(int carSlot)
{
	return (carSlot >= 0 && carSlot < MAX_CARS) ? sMpStandInCar[carSlot] : 0;
}

/* JERICHO: the readiness stand-in, for SINGLE-PLAYER (and for any frame in which the
 * session's own reload failsafe is not driving).
 *
 * The engine's slot decision (JerCarSlotState) now refuses an imported slot until every set
 * its model names has a live pin and a mapped palette row. That is deliberately stricter
 * than "a mesh exists", and a car already on the road can briefly be in that state while a
 * switch/reload lands. In a session, MpCarFailsafeTick already covers this and reports the
 * real choice over the wire; OUTSIDE a session nothing did, so the draw gate simply skipped
 * the car and the player saw nothing. Use the SAME proven stand-in here: the host city's
 * car 1, repointed and tinted solid grey by the draw-colour hook, until the real slot is
 * drawable - then repoint back and rebuild its dump.
 *
 * The engine draw path itself never mutates cp->ap.model: only this module does, exactly as
 * the session failsafe already does, so carhacks/mp identity keeps reading the real choice
 * from the remembered slot rather than from the grey body. */
void MpCarReadinessTick(void)
{
	extern u_char defaultPlayerPalette;
	CAR_DATA* cp;
	int carId, stored, want, host, city;

	if (gMp.running || gInFrontend)
		return;			/* the session failsafe owns MP; no car in the frontend */

	if (player[0].playerType != PLAYER_TYPE_CAR)
		return;			/* on foot: do not steal the parked car or the camera */

	carId = player[0].playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	cp = &car_data[carId];
	stored = sMpReadySlot[carId];

	if (stored > 0)
	{
		want = stored - 1;
		host = sMpReadyHost[carId];

		if (cp->ap.model != host)
		{
			/* The car object was repointed since we put the stand-in on: this is no
			 * longer our body, so forget the memory rather than fight it. */
			sMpReadySlot[carId] = 0;
			MpCarClearStandIn(carId);
			return;
		}

		if (!JerCarSlotDrawable(want))
			return;		/* still not drawable: keep the grey body */

		sMpReadySlot[carId] = 0;
		MpCarClearStandIn(carId);

		cp->ap.model = want;
		cp->ap.carCos = &car_cosmetics[want];
		cp->lowDetail = -1;
		cp->ap.palette = (u_char)CarSlotClampPalette(want, (int)sMpReadyPalette[carId]);
		CreateDentableCar(cp);

		printInfo("[mp] car readiness: slot %d (%s model %d) is drawable - leaving the host city's car 1 stand-in\n",
			want, MpCarCityName(GetCarModelSourceCity(want)), residentCarModels[want]);

		return;
	}

	want = cp->ap.model;

	if (want < 0 || want >= MAX_CAR_RESIDENT_MODELS)
		return;

	city = GetCarModelSourceCity(want);

	if (city < 0)
		return;			/* the level's own car: no import sets to wait for */

	if (JerCarSlotDrawable(want))
		return;

	sMpReadyPalette[carId] = (u_char)cp->ap.palette;

	if (MpCarStandIn(carId))
	{
		sMpReadySlot[carId] = want + 1;
		sMpReadyHost[carId] = cp->ap.model;	/* MpCarStandIn repointed it */

		printInfo("[mp] car readiness: slot %d (%s model %d) is not drawable yet - standing in with the host city's car 1, solid grey\n",
			want, MpCarCityName(city), residentCarModels[want]);
	}
}

void MpCarFailsafeTick(void)
{
	extern u_char defaultPlayerPalette;
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;
	int want, hostSlot;

	if (!gMp.running || me == NULL || !me->isLocal || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	cp = &car_data[me->carId];

	/* THE WOUND CHECK: never let the chase camera target a car that has no body. If
	 * the player's car or the camera's car is cleared/released, re-seat the camera on
	 * our car this frame - a camera left on a released slot is the spin. */
	if (player[0].playerType == PLAYER_TYPE_CAR &&
		(player[0].cameraCarId < 0 || player[0].cameraCarId >= MAX_CARS ||
		 car_data[player[0].cameraCarId].controlType == CONTROL_TYPE_NONE ||
		 !JerCarSlotUsable(car_data[player[0].cameraCarId].ap.model)))
	{
		player[0].cameraCarId = me->carId;
		player[0].cameraAngle = cp->hd.direction + 1536;

		if (!sMpCamReseat && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] camera: re-seated on our car (slot %d) - its target had no vehicle\n",
				me->carId);

		sMpCamReseat = 1;
	}
	else
		sMpCamReseat = 0;

	/* Learn the choice from a healthy car once (a spawn or a car we got into);
	 * a change sets it explicitly in MpAdoptCar, even when that load fails. */
	if (!sMpStandIn && sMpWantModel < 0 &&
		cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS &&
		JerCarSlotUsable(cp->ap.model))
	{
		sMpWantSlot = cp->ap.model;
		sMpWantModel = residentCarModels[sMpWantSlot];
		sMpWantCity = GetCarModelSourceCity(sMpWantSlot);
	}

	if (sMpWantModel < 0)
		return;

	want = MpResidentSlotForCar(sMpWantCity, sMpWantModel);

	if (want >= 0 && want < MAX_CAR_RESIDENT_MODELS && JerCarSlotUsable(want))
	{
		sMpReloadWait = 0;
		sMpReloadTries = 0;

		if (sMpStandIn)
		{
			sMpStandIn = 0;
			sMpWantSlot = want;
			MpCarClearStandIn(me->carId);

			cp->ap.model = want;
			cp->ap.carCos = &car_cosmetics[want];
			cp->lowDetail = -1;
			cp->ap.palette = (u_char)CarSlotClampPalette(want, (int)defaultPlayerPalette);
			CreateDentableCar(cp);

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] car reload: %s model %d is loaded - leaving the host city's car 1 stand-in\n",
					MpCarCityName(sMpWantCity), sMpWantModel);
		}
		else
			sMpWantSlot = want;

		return;
	}

	hostSlot = MpHostCarOneSlot();

	if (hostSlot < 0)
		return;

	if (!sMpStandIn)
	{
		sMpStandIn = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] car reload: %s model %d is not loaded yet (slot %d) - standing in with the host city's car 1, solid grey (resident slot %d) until it does\n",
				MpCarCityName(sMpWantCity), sMpWantModel, want, hostSlot);
	}

	/* still not loaded: after a short delay, ask for it again (bounded) */
	if (++sMpReloadWait >= MP_RELOAD_RETRY_FRAMES && sMpReloadTries < MP_RELOAD_RETRY_MAX)
	{
		sMpReloadWait = 0;
		sMpReloadTries++;
		MpCarQueryLoad(sMpWantCity, sMpWantModel);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] car reload: retrying %s model %d (attempt %d of %d)\n",
				MpCarCityName(sMpWantCity), sMpWantModel, sMpReloadTries, MP_RELOAD_RETRY_MAX);
	}

	MpCarStandIn(me->carId);
}

static int MpAdoptCar(int carSlot, int city, int model, MP_PLAYER* who)
{
	CAR_DATA* cp;
	int slot;

	if (carSlot < 0 || carSlot >= MAX_CARS)
		return 0;

	if (who != NULL && who->isLocal)
	{
		/* Remember the choice even when this load cannot happen yet: the reload
		 * failsafe needs to know which car to put back afterwards. */
		sMpWantCity = city;
		sMpWantModel = model;
		sMpWantSlot = MpResidentSlotForCar(city, model);
	}

	if (!gMpAdoptModelInit)
	{
		int i;

		for (i = 0; i < MAX_CARS; i++)
			gMpAdoptModel[i] = -1;

		gMpAdoptModelInit = 1;
	}

	if (who != NULL && !who->isLocal && MpRemoteTargetsLocalCar(carSlot, "car adopt/rebuild", who->id))
		return 0;

	cp = &car_data[carSlot];

	/* The resident slot on THIS machine that holds the vehicle the owner named.
	 * This is the whole reason the wire carries a (city, model) and not a slot:
	 * slot numbers do not mean the same car on two machines. */
	slot = MpResidentSlotForCar(city, model);

	/* Only to a slot the renderer actually HAS. Pointing ap.model at a mesh we
	 * never loaded is a crash, not a cosmetic glitch, so an unavailable slot
	 * keeps the old one and says so. */
	if (JerCarSlotUsable(slot))
	{
		int was = cp->ap.model;

		MpCarClearStandIn(carSlot);

		if (was == slot && gMpAdoptModel[carSlot] == model)
		{
			/* Already that car, and its verts match. OUR car may still want a new
			 * palette ("Respawn as this car" after only the palette changed): repaint
			 * it; the carstate's ap.palette carries it to the peers. */
			extern u_char defaultPlayerPalette;
			int want;

			if (who != NULL && !who->isLocal)
				return 0;

			want = CarSlotClampPalette(slot, (int)defaultPlayerPalette);

			if (want == (int)cp->ap.palette)
				return 0;

			cp->ap.palette = (u_char)want;

			if (who != NULL)
				who->palette = want;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] same car on slot %d: repainted in palette %d\n", slot, want);

			return 1;
		}

		cp->ap.model = slot;

		/* only a palette the NEW model has: ours is the player's choice
		 * (defaultPlayerPalette), a peer's the one they last named; out of range
		 * -> the model's own colours. The carstate then sends what is drawn. */
		{
			extern u_char defaultPlayerPalette;
			int want = (who == NULL || who->isLocal) ? (int)defaultPlayerPalette
								 : who->palette;

			cp->ap.palette = (u_char)CarSlotClampPalette(slot, want);
		}

		/* REBUILD THE MESH. DrawCar draws the slot's poly/UV list against
		 * gTempCarVertDump[carId], and CreateDentableCar is the ONLY thing that fills
		 * that dump. Writing ap.model and stopping there draws one car's polygons
		 * over another car's vertices - that is what a garbled remote car IS.
		 *
		 * This mirrors how the engine makes a car: InitCar sets ap.carCos from the
		 * slot and finishes with CreateDentableCar (civ_ai.c:108-112, :164).
		 * ap.carCos is not cosmetic only - the collision box and the chase camera
		 * read it (bcollide.c, camera.c), so leaving it stale would follow a car with
		 * the wrong box. lowDetail = -1 matches a freshly made car.
		 *
		 * CreateDentableCar also zeroes this car's LOCAL damage (denting.c), which is
		 * expected: the session does not replicate damage. DentCar is NOT called - it
		 * applies the spawn-time "used car" look, which a model swap should not add. */
		cp->ap.carCos = &car_cosmetics[slot];
		cp->lowDetail = -1;

		CreateDentableCar(cp);
		gMpAdoptModel[carSlot] = model;

		/* The owner is the authority for the wheel appearance too. A rebuild resets
		 * the body, so put their lost-hubcap bits back on the fresh car. */
		if (who != NULL && !who->isLocal)
		{
			CarApplyHubcapState(carSlot, who->hubcaps & 0x0f);

			if (gMpCtx != NULL && who->hubcaps)
				gMpCtx->jer_log(gMpCtx,
					"[mp] hubcaps: player %d -> %d applied to the rebuilt car (slot %d)\n",
					who->id, who->hubcaps & 0x0f, carSlot);
		}

		/* OUR car: rebind its engine and SIREN sounds to the new model, exactly as
		 * getting in does (players.c ChangePedPlayerToCar -> StartPlayerCarSounds).
		 * The siren is a looping channel (player*3+2) that is only STARTED there, and
		 * the per-frame control just unmutes it while the horn is held -- so changing
		 * from a car with no siren into a police car left that channel never started:
		 * light bar on, siren silent until you got out and back in. Going the other
		 * way, stop the old siren loop so it cannot hang on. */
		if (carSlot == player[0].playerCarId && player[0].playerType == PLAYER_TYPE_CAR)
		{
			if (CarHasSiren(slot) == 0)
				StopChannel(2);

			StartPlayerCarSounds(0, slot, (VECTOR*)cp->hd.where.t);
		}

		/* Say it: the car was REBUILT, not merely re-pointed. A hot-loaded car also
		 * had its cosmetics replaced the moment they landed (JerHotLoadCarCosmetics),
		 * and this is the call that makes the car on the road use them. */
		if (gMpCtx != NULL)
		{
			if (who != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] rebuilt player %d's car on slot %d (model %d from %s): cosmetics, collision box and mesh are that car's own now"
					" [local=%d localId=%d slot=%d]\n",
					who->id, slot, model, MpCarCityName(city),
					who->isLocal, gMp.localPlayerId, carSlot);
			else
				gMpCtx->jer_log(gMpCtx,
					"[mp] rebuilt the local car on slot %d (model %d from %s): cosmetics, collision box and mesh are that car's own now (single player)\n",
					slot, model, MpCarCityName(city));
		}

		/* The roster row's identity is a model NUMBER now, with its city -- not the
		 * slot we happened to resolve it to. In single player there is no row to
		 * record it on: the car on the road IS the record. */
		if (who != NULL)
		{
			who->car = model;
			who->carIsSlot = 0;
			who->carCity = (city < 0) ? -1 : city;

			if (who->id >= 0 && who->id < MP_MAX_PLAYERS)
				sMpCarKeepSet[who->id] = 0;	/* it holds the car now; re-arm the gate */

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] player %d changed car: slot %d -> %d (%s model %d), mesh rebuilt"
					" [local=%d localId=%d row=%d]\n",
					who->id, was, slot, MpCarCityName(city), model,
					who->isLocal, gMp.localPlayerId, (int)(who - gMp.players));
		}
		else if (gMpCtx != NULL)
		{
			gMpCtx->jer_log(gMpCtx,
				"[mp] changed car (single player): slot %d -> %d (%s model %d), mesh rebuilt\n",
				was, slot, MpCarCityName(city), model);
		}

		return 1;
	}

	/* Cannot hold it: put the host city's car 1 under it, painted solid grey, until
	 * the owner's model is resident. The old body was the previous model, which on a
	 * release/reload is exactly the slot that has just lost its mesh - the spinning
	 * camera. Say so ONCE per change, not every frame -- a carstate arrives every
	 * frame, so the repetition is the noise this avoids. */
	if (MpCarStandIn(carSlot) && gMpCtx != NULL && (who == NULL || !who->isLocal))
		gMpCtx->jer_log(gMpCtx,
			"[mp] car load: %s model %d is not loaded yet - using the host city's car 1, solid grey, on slot %d until it does\n",
			MpCarCityName(city), model, carSlot);

	if (who != NULL && gMpCtx != NULL && who->id >= 0 && who->id < MP_MAX_PLAYERS &&
		(!sMpCarKeepSet[who->id] || sMpCarKeepCity[who->id] != city || sMpCarKeepModel[who->id] != model))
	{
		sMpCarKeepCity[who->id] = city;
		sMpCarKeepModel[who->id] = model;
		sMpCarKeepSet[who->id] = 1;

		if (slot >= 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d drives %s model %d (slot %d here) but that mesh is not loaded; the host city's car 1 stands in on slot %d\n",
				who->id, MpCarCityName(city), model, slot, carSlot);
		else
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d drives %s model %d, which this machine does not hold (the hotload will load it); the host city's car 1 stands in on slot %d\n",
				who->id, MpCarCityName(city), model, carSlot);
	}
	else if (who == NULL && gMpCtx != NULL)
	{
		/* Single player: a menu action, not a per-frame carstate, so there is no
		 * repetition to suppress -- say it every time. */
		if (slot >= 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): %s model %d resolves to slot %d here but that mesh is not loaded; the host city's car 1 stands in on slot %d\n",
				MpCarCityName(city), model, slot, carSlot);
		else
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): %s model %d is not held by this machine; the host city's car 1 stands in on slot %d\n",
				MpCarCityName(city), model, carSlot);
	}

	return 0;
}

/* A roster row's car, arriving from its owner (or from the host's roster): the
 * session-shaped entry point, which is just the core aimed at that row's car. */
int MpAdoptRemoteCar(MP_PLAYER* p, int city, int model)
{
	if (p == NULL)
		return 0;

	return MpAdoptCar(p->carId, city, model, p);
}

/* Which cities this session can offer. Fills `out` with 0..3 city indices and
 * returns how many were written; 0 means nobody answered, and the caller should
 * offer the session's own city alone. */
int MpCarQueryCities(int* out, int max)
{
	JER_CARQ_CITIES_ARGS a;

	if (out == NULL || max <= 0)
		return 0;

	memset(&a, 0, sizeof(a));
	a.cities = out;
	a.max = max;

	jer_fire(JER_CARQ_CITIES, &a);

	if (a.count < 0)
		return 0;

	return (a.count > max) ? max : a.count;
}

/* Ask whoever can to make (city, model) available on THIS machine and in the
 * session (carhacks' import/hotload). The holder decides -- a refusal is reported
 * to the player rather than driven into a model this machine cannot render. */
int MpCarQueryLoad(int city, int model)
{
	JER_CARQ_LOAD_ARGS a;

	memset(&a, 0, sizeof(a));
	a.city = city;
	a.model = model;

	jer_fire(JER_CARQ_LOAD, &a);

	return a.ok ? 1 : 0;
}

/* Which slots may `city` be offered in, and the model in each (jer_carquery.h). */
int MpCarQuerySlots(int city, int* slots, int* models, int max)
{
	JER_CARQ_SLOTS_ARGS a;

	if (slots == NULL || models == NULL || max <= 0)
		return 0;

	memset(&a, 0, sizeof(a));
	a.city = city;
	a.slots = slots;
	a.models = models;
	a.max = max;

	jer_fire(JER_CARQ_SLOTS, &a);

	/* count stays 0 when nobody answered -- "no better answer", not an error: the caller
	 * then keeps the list it already had. */
	if (a.count < 0)
		return 0;

	return (a.count > max) ? max : a.count;
}

/* The car list a city may be offered: what the car mods say, or failing that the engine's
 * own frontend table - see mp.h for why that table is the wrong question IN A SESSION.
 * The game mode then narrows it (MpCarListForCity, below). */
static int MpCarListForCityAll(int city, int* slots, int* models, int max)
{
	extern int CarAvailability[CITY_COUNT][10];    /* grown with the registry */
	extern char carNumLookup[CITY_COUNT][10];
	int n, slot;

	if (slots == NULL || models == NULL || max <= 0)
		return 0;

	if (city < 0 || city >= CITY_COUNT)
		city = 0;

	n = MpCarQuerySlots(city, slots, models, max);

	if (n > 0)
		return n;

	/* Nobody knew better: the frontend table, exactly as this picker read it before there
	 * was anything to ask. */
	n = 0;

	for (slot = 0; slot < 10 && n < max; slot++)
	{
		if (CarAvailability[city][slot] == 0)
			continue;

		slots[n] = slot;
		models[n] = (int)(unsigned char)carNumLookup[city][slot];
		n++;
	}

	return n;
}

/* GAME-MODE DECISION POINT: the ONE list the pause picker and the test rigs read, so a
 * mode's car pool (C&R: police / not police) is applied once, here. Take a Ride's pool
 * is "everything", and the filter returns the list untouched. */
int MpCarListForCity(int city, int* slots, int* models, int max)
{
	int n = MpCarListForCityAll(city, slots, models, max);

	if (n <= 0)
		return n;

	n = MpModeFilterCarPool(gMp.gamemode, city, slots, models, n);

	/* Cops n Robbers: the role's cars (police for a cop, the rest for the getaway).
	 * Which car is a police car is the engine's answer for THIS city's model
	 * (JerCarIsPolice: model 0 in a Driver 2 city, the bake's cop flags in a car
	 * mod's bank), so an imported police car counts. */
	{
		MP_PLAYER* me = MpLocalPlayer();

		if (gMp.running && me != NULL && MpMarkHaveState() && MpMarkMode() == gMp.gamemode)
		{
			int police[MP_CAR_LIST_MAX];
			int i, m = (n < MP_CAR_LIST_MAX) ? n : MP_CAR_LIST_MAX;

			for (i = 0; i < m; i++)
				police[i] = JerCarIsPolice(city, models[i]);

			n = MpModeFilterCarPoolRole(gMp.gamemode, MpMarkMarked() == me->id, slots, models, police, m);
		}
	}

	return n;
}

/* The picks for `mode`: another mode's picks name other roles, so a change of
 * mode starts from none. In memory only. */
static void MpRolePicksFor(int mode)
{
	if (gMpRolePickMode == mode)
		return;

	gMpRolePickMode = mode;
	memset(gMpRolePick, 0, sizeof(gMpRolePick));
}

/* The connection is over: forget every pick, so the next join asks again. Kept
 * while a server city change's rejoin is pending (MpRejoinActive): that leave
 * and join are one connection to the player. */
void MpRolePicksForget(const char* why)
{
	int i, had = 0;

	if (MpRejoinActive())
		return;

	for (i = 0; i < MP_MODE_ROLE_SLOTS_MAX; i++)
		had |= gMpRolePick[i].set;

	memset(gMpRolePick, 0, sizeof(gMpRolePick));
	gMpRolePickMode = -1;

	if (had && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] role car: picks forgotten (%s) - the next join asks again\n", why);
}

/* Keep (city, model) as `slot`'s car when the slot's pool allows it. */
static int MpRolePickStore(int mode, int slot, int city, int model, int palette, const char* from)
{
	int police, pool;
	MP_ROLE_PICK* r;

	if (slot < 0 || slot >= MpModeRoleSlotCount(mode) || model < 0 || city < 0 || city >= CITY_COUNT)
		return 0;

	MpRolePicksFor(mode);
	pool = MpModeRoleSlotPool(mode, slot);
	police = JerCarIsPolice(city, model);

	if (!MpModePoolKeeps(pool, police))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] role car: %s %s model %d refused for %s (%s car, wants %s) - %s\n",
				from, MpCarCityName(city), model, MpModeRoleSlotTitle(mode, slot),
				police ? "a police" : "not a police",
				pool == MP_MODE_POOL_POLICE ? "police" : "non-police", "the round picks one");
		return 0;
	}

	r = &gMpRolePick[slot];
	r->set = 1;
	r->city = city;
	r->model = model;
	r->palette = (palette >= 0 && palette <= 5) ? palette : 0;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] role car: %s = %s model %d palette %d (%s, %s car)\n",
			MpModeRoleSlotTitle(mode, slot), MpCarCityName(city), model, r->palette, from,
			police ? "police" : "non-police");

	return 1;
}

/* JER_CARQ_ROLES: the join car select asks how many cars to ask for. */
int MpOnCarqRoles(void* userdata, void* args)
{
	JER_CARQ_ROLES_ARGS* a = (JER_CARQ_ROLES_ARGS*)args;
	MP_PLAYER* me;
	int i, n, mode = gMp.gamemode, launch;

	(void)userdata;

	if (a == NULL || !MpIsActive())
		return JER_RESULT_CONTINUE;

	n = MpModeRoleSlotCount(mode);

	if (n <= 0)
		return JER_RESULT_CONTINUE;	/* one ordinary pick */

	if (n > JER_CARQ_ROLES_MAX)
		n = JER_CARQ_ROLES_MAX;

	for (i = 0; i < n; i++)
	{
		snprintf(a->roles[i].title, sizeof(a->roles[i].title), "%s", MpModeRoleSlotTitle(mode, i));
		a->roles[i].pool = MpModeRoleSlotPool(mode, i);
	}

	/* the level loads with the car of the role this player has now: the marked
	 * one's when they already are (a rejoin mid-match), everyone else's otherwise
	 * (the getaway is only named at the countdown, which swaps it in) */
	me = MpLocalPlayer();
	launch = MpModeRoleSlotFor(mode, me != NULL && MpMarkHaveState() && MpMarkMode() == mode &&
		MpMarkMarked() == me->id);

	a->count = n;
	a->launch = (launch >= 0 && launch < n) ? launch : 0;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] role car: the car select asks for %d car(s) (%s), launching with %s\n",
			n, MpModeDisplayName(mode), a->roles[a->launch].title);

	return JER_RESULT_CONTINUE;
}

/* JER_CARQ_ROLEPICK: one role's pick, from the join car select. */
int MpOnCarqRolePick(void* userdata, void* args)
{
	JER_CARQ_ROLEPICK_ARGS* a = (JER_CARQ_ROLEPICK_ARGS*)args;

	(void)userdata;

	if (a == NULL || !MpIsActive())
		return JER_RESULT_CONTINUE;

	a->ok = MpRolePickStore(gMp.gamemode, a->role, a->city, a->model, a->palette, "car select");
	return JER_RESULT_CONTINUE;
}

/* Tell whoever keeps the session's car identity (carhacks) how a switch ended: the local
 * player now drives (city, model) - city -1 for the level's own car - or (changed = 0) the
 * switch to it did not happen. A notice, not a question: nobody answering is fine. This is
 * what lets the identity move only after a SUCCESSFUL switch, and the car the player left
 * be given back (carhacks/net.c, chkNetReleaseSlotIfUnused). */
void MpCarQueryChosen(int city, int model, int changed)
{
	JER_CARQ_CHOSEN_ARGS a;

	if (model < 0)
		return;

	memset(&a, 0, sizeof(a));
	a.city = city;
	a.model = model;
	a.changed = changed ? 1 : 0;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] car chosen: %s model %d (%s)\n",
			MpCarCityName(city), model, changed ? "driving it now" : "the switch did not happen");

	jer_fire(JER_CARQ_CHOSEN, &a);
}

/* Change OUR OWN car, mid-match, from the pause menu.
 *
 * Replace the vehicle we are driving with another one -- the "Change car" row in
 * the Multiplayer pause page (mp.c), which is also the path a carhacks live pick
 * drives. On foot the player is put into their parked car first: "change car" from
 * the pavement has to produce a car, not a new pedestrian.
 *
 * THE SLOT IS STILL OUR OWN and only the cosmetic model changes, through the SAME
 * function a peer's car goes through (MpAdoptRemoteCar) -- so nothing about this
 * world's car slots, and so nothing about anyone else's car, is disturbed, and a
 * change we make and a change a peer makes read identically in the log.
 *
 * The choice then travels the ordinary way: our carstate carries the new
 * (city, model) from this frame on, so every peer re-models its copy of us, and a
 * client also tells the host so the host's roster names the car it is really
 * driving. Whether a machine HOLDS the car is not this function's business -- a
 * car the level does not carry is what the carhacks import is for. Returns 1 if
 * the car on the road changed (0 when we were already driving it, or there was
 * nothing to change). */
/* Why a pick could not be applied, in the player's own words.
 *
 * One place, so the on-screen notice, the log and the picker's filtering all
 * describe the SAME rule: this machine does not hold the car / it holds a slot with
 * no mesh / the car is already the one being driven. (MpAdoptCar returns 0 for the
 * last case too, which is not a refusal at all.) */
const char* MpChangeCarRefusal(int city, int model)
{
	int slot = MpResidentSlotForCar(city, model);

	if (slot < 0)
		return "this machine does not hold that car";

	if (JerCarSlotUsable(slot))
		return "you are already driving it";

	return JerCarSlotRefusal(slot);
}

int MpCarChangeWaiting(void)
{
	return gMpCarChangeWait.active;
}

int MpChangeCar(int city, int model)
{
	/* GAME-MODE DECISION POINT: a mode may lock the car while a round runs. Take a
	 * Ride never does, so this is a no-op there. Checked here, at the one public door
	 * (the pause Apply row and every test lever come through it), and not in the
	 * retry, which only finishes a change this already allowed. */
	{
		const char* why = MpModeCarChangeRefusal(gMp.gamemode, gMp.running);

		if (why != NULL)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] change car refused: %s model %d - %s\n",
					MpCarCityName(city), model, why);

			return 0;
		}
	}

	/* A mode with role cars: the pick is the car of the role this player has now
	 * (MpCarListForCity already offered only that role's pool). Kept before the
	 * change runs, so a change that waits for an import still counts. */
	if (gMp.running && MpModeRoleSlotCount(gMp.gamemode) > 0 && city >= 0)
	{
		MP_PLAYER* me = MpLocalPlayer();
		extern u_char defaultPlayerPalette;
		int slot = MpModeRoleSlotFor(gMp.gamemode, me != NULL && MpMarkHaveState() &&
			MpMarkMode() == gMp.gamemode && MpMarkMarked() == me->id);

		MpRolePickStore(gMp.gamemode, slot, city, model, (int)defaultPlayerPalette, "pause Change car");
	}

	return MpChangeCarRun(city, model, 0);
}

/* Forget a wait that is no longer relevant (level reset, leaving the session). */
void MpCarChangeWaitForget(void)
{
	gMpCarChangeWait.active = 0;
}

/* One retry a frame, from the frame hook (the world is up there, which is what makes a
 * level's resident slots mean anything). The wait owns framesLeft, so MpChangeCarRun
 * only ever CLEARS the wait -- it never re-arms one on a retry. */
void MpCarChangeWaitTick(void)
{
	if (!gMpCarChangeWait.active)
		return;

	if (--gMpCarChangeWait.framesLeft <= 0)
	{
		int city = gMpCarChangeWait.city;
		int model = gMpCarChangeWait.model;

		gMpCarChangeWait.active = 0;

		/* The wait ran out. The refusal is a LOG line now: the on-screen notice that
		 * used to say it here went with the rest of mp's bottom-left debug block, and
		 * the REASON is the part worth keeping. */
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] change car refused: %s model %d - %s\n",
				MpCarCityName(city), model, MpChangeCarRefusal(city, model));

		return;
	}

	/* One retry a FRAME is too eager: the pool frees in frames, not microseconds, and
	 * each attempt costs a log line and a walk of the set. Measured: an unthrottled
	 * retry turned a run's handful of "no room" lines into 500+ of them. */
	if ((gMpCarChangeWait.framesLeft % MP_CHANGE_RETRY_EVERY) != 0)
		return;

	MpChangeCarRun(gMpCarChangeWait.city, gMpCarChangeWait.model, 1);
}

static int MpChangeCarRun(int city, int model, int isRetry)
{
	MP_PLAYER* me = MpLocalPlayer();
	int changed;

	if (model < 0)
		return 0;

	/* SINGLE PLAYER. The pause menu's Change car row is registered unconditionally,
	 * so in a solo game its Apply row arrives HERE, with gMp.running false and no
	 * roster in existence. A change is then a purely local re-model of the car the
	 * local player is driving: the same picker and the same "does this machine hold
	 * it" rule as a session, with nobody to publish to. A refused pick (a car this
	 * level has no mesh for) leaves the car alone and says so, exactly as it does in
	 * a session. */
	if (!gMp.running)
	{
		int slot = MainPlayer.playerCarId;

		if (slot < 0 || slot >= MAX_CARS || MainPlayer.playerType != PLAYER_TYPE_CAR)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] change car (single player): not driving (playerCarId %d, type %d) - nothing to change\n",
					slot, (int)MainPlayer.playerType);

			return 0;
		}

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] change car (single player): we asked for %s model %d (slot %d)\n",
				MpCarCityName(city), model, slot);

		/* NOT A CAR THIS MACHINE HOLDS YET? Ask for it, exactly as the session path
		 * does below. The picker offers a city's whole roster, so choosing one IS the
		 * request that imports it -- without this a cross-city or Driver 1 pick could
		 * be selected and then always refused, and the roster would be a lie.
		 *
		 * The ANSWER is used now: "not yet" means wait (see the block above the
		 * function), never adopt. */
		if (MpResidentSlotForCar(city, model) < 0 && !MpCarQueryLoad(city, model))
		{
			if (!isRetry)
			{
				gMpCarChangeWait.active = 1;
				gMpCarChangeWait.city = city;
				gMpCarChangeWait.model = model;
				gMpCarChangeWait.framesLeft = MP_CHANGE_WAIT_FRAMES;
			}

			return 0;
		}

		gMpCarChangeWait.active = 0;	/* the load landed (or was never needed) */

		changed = MpAdoptCar(slot, city, model, NULL);

		/* A successful change is already in the log (the "we asked for" line above,
		 * and MpAdoptCar's own). A refusal's REASON was the useful part of the notice
		 * that used to be on screen, so it goes to the log instead. */
		if (!changed && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] change car refused: %s model %d - %s\n",
				MpCarCityName(city), model, MpChangeCarRefusal(city, model));

		return changed;
	}

	if (me == NULL)
		return 0;

	if (me->carId < 0)
	{
		int seat = -1;
		int s;

		/* PREFER THE CAR WE ACTUALLY GOT OUT OF. It is the body this machine last
		 * parked, and the one the peers still hold for us (their leftCarSlot1), so
		 * getting back into it makes the carstate carry MP_CARSTATE_SAME_BODY and
		 * they re-seat us on it. Seating ourselves in some OTHER parked car would
		 * read as a genuinely different vehicle and they would build a second one,
		 * leaving this one standing. Same usability tests as the scan below. */
		if (me->leftCarSlot1 > 0)
		{
			int left = me->leftCarSlot1 - 1;

			if (left >= 0 && left < MAX_CARS && gMpOurCars[left] &&
				MpGetPlayerByCar(left) == NULL &&
				car_data[left].controlType == CONTROL_TYPE_CIV_AI &&
				car_data[left].ap.carCos != NULL &&
				JerCarSlotUsable(car_data[left].ap.model))
				seat = left;
		}

		/* Otherwise: get back into any car OF OURS -- one the module made, left
		 * parked and enterable (see MpFollowLocalCar). Never a live traffic car:
		 * taking one over and vacating it hands the traffic AI a car it cannot
		 * steer, which is the crash that note exists to prevent. */
		for (s = 0; seat < 0 && s < MAX_CARS; s++)
		{
			if (!gMpOurCars[s] || MpGetPlayerByCar(s) != NULL)
				continue;

			/* The destination has to be a car we can actually put the player in:
			 * a live CIV_AI body, with its own cosmetics and a resident model whose
			 * mesh is built. A slot the engine cleared (PingOutCar) is not a car any
			 * more, and entering it would move the local identity onto a CAR_DATA
			 * whose ap.carCos is NULL - the SetUpCivCollFlags 0xD8 crash. */
			if (car_data[s].controlType == CONTROL_TYPE_CIV_AI &&
				car_data[s].ap.carCos != NULL &&
				JerCarSlotUsable(car_data[s].ap.model))
			{
				seat = s;
				break;
			}
		}

		if (seat < 0)
		{
			if (gMpCtx != NULL)
			{
				int left = me->leftCarSlot1 - 1;

				/* Say WHY, not just that it failed: this is the one place a player
				 * finds out their parked car is not usable any more, and the four
				 * reasons are indistinguishable from the outside. */
				if (left >= 0 && left < MAX_CARS)
					gMpCtx->jer_log(gMpCtx,
						"[mp] change car: on foot and no car of ours to get back into "
						"(left slot %d: ours %d, taken %d, type %d, cosmetics %s, "
						"model %d, usable %d)\n",
						left, (int)gMpOurCars[left], MpGetPlayerByCar(left) != NULL,
						(int)car_data[left].controlType,
						(car_data[left].ap.carCos != NULL) ? "present" : "NULL",
						(int)car_data[left].ap.model,
						JerCarSlotUsable(car_data[left].ap.model));
				else
					gMpCtx->jer_log(gMpCtx,
						"[mp] change car: on foot and no car of ours to get back into\n");
			}

			return 0;
		}

		/* The pedestrian is a real object, not just the player's pointer to it: the
		 * engine's own get-in path (PedGetInCar -> ChangePedPlayerToCar + DestroyPedestrian
		 * + the Tanner counter) removes it, and calling only ChangePedPlayerToCar leaves
		 * the Tanner standing exactly where we got out - "you put me back in the car but
		 * did not remove my Tanner". */
		{
			LPPEDESTRIAN ped = player[0].pPed;

			ChangePedPlayerToCar(0, &car_data[seat]);
			RemovePlayerPedestrian(ped);

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] change car: got back into slot %d and put our pedestrian away (%s)\n",
					seat, (ped != NULL) ? "removed" : "none");
		}

		/* Adopt the move NOW, so the re-model below lands on the car we just got
		 * into instead of waiting a frame for the poll in MpSendOwnCarState. */
		MpFollowLocalCar();
	}

	if (me->carId < 0 || me->carId >= MAX_CARS)
		return 0;

	/* NOT logged on a retry: a retry is not a new ask, and the carstress rig counts
	 * changes from this exact line (measured: an unthrottled retry inflated its
	 * "changes" figure 7x and made the coverage number meaningless). */
	if (gMpCtx != NULL && !isRetry)
		gMpCtx->jer_log(gMpCtx, "[mp] change car: we asked for %s model %d (slot %d)\n",
			MpCarCityName(city), model, me->carId);

	/* Not a car this machine holds? Ask whoever can to make it available (carhacks'
	 * import + hotload), which is also what tells the other machines to fold it in.
	 * Only THEN try the swap: pointing the car at a mesh that is not loaded is a
	 * crash, not a cosmetic glitch (see MpAdoptRemoteCar).
	 *
	 * So the answer is used: "not yet" (a spare slot busy for a frame or two) is a
	 * wait, not a refusal, and it must NOT fall through to the swap. */
	if (MpResidentSlotForCar(city, model) < 0 && !MpCarQueryLoad(city, model))
	{
		if (!isRetry)
		{
			gMpCarChangeWait.active = 1;
			gMpCarChangeWait.city = city;
			gMpCarChangeWait.model = model;
			gMpCarChangeWait.framesLeft = MP_CHANGE_WAIT_FRAMES;
		}

		return 0;
	}

	gMpCarChangeWait.active = 0;	/* the load landed (or was never needed) */

	changed = MpAdoptRemoteCar(me, city, model);

	/* Report the OUTCOME (JER_CARQ_CHOSEN). "Driving it" is read off the car on the road, so
	 * "already that car" counts as driving it, and the city is the one the slot's data came
	 * from (-1 = the level's own car) - the identity the other machines must draw. */
	{
		int want = MpResidentSlotForCar(city, model);
		int onIt = (want >= 0 && me->carId >= 0 && me->carId < MAX_CARS &&
			car_data[me->carId].ap.model == want);

		MpCarQueryChosen(onIt ? GetCarModelSourceCity(want) : city, model, onIt);
	}

	/* Publish the choice. A client's pick goes to the HOST alone, which
	 * republishes the roster naming it; the host does that itself. Our carstate
	 * carries the model either way. */
	if (MpIsHost())
		MpHostSendRoster();
	else
		MpSendPickedCar(model, city);

	/* The change itself is in the log (MpAdoptRemoteCar's "rebuilt player N's car"
	 * line and MpCarQueryChosen's "car chosen"). Only a REFUSAL carried text worth
	 * keeping, so its reason goes to the log rather than the screen. */
	if (!changed && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] change car refused: %s model %d - %s\n",
			MpCarCityName(city), model, MpChangeCarRefusal(city, model));

	/* THE INVISIBLE-CAR INSTRUMENT.
	 *
	 * A player reported cars "loading invisible" after cycling a few, and no run has
	 * reproduced it: the lever changes 12 cars across all four cities with every switch
	 * succeeding, the previous slot released each time (7/8 alternating) and the pool flat.
	 * So rather than guess at a fix, this states the ONE thing that makes a car invisible -
	 * ap.model pointing at a resident model whose mesh is not built - in the log of the
	 * change itself. DrawCar returns without drawing when gCarCleanModelPtr[] is NULL, so
	 * that NULL is exactly the invisible car, and a report can name the state rather than
	 * the symptom. Logged once per change, not per frame. */
	if (gMpCtx != NULL && me != NULL && me->carId >= 0 && me->carId < MAX_CARS)
	{
		CAR_DATA* cp = &car_data[me->carId];
		int slot = cp->ap.model;

		if (slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS)
		{
			int built = JerCarSlotUsable(slot);

			gMpCtx->jer_log(gMpCtx,
				"[mp] car status: driving model %d (resident %d, source %s); mesh %s\n",
				(int)residentCarModels[slot], slot, MpCarCityName(GetCarModelSourceCity(slot)),
				built ? "loaded" : "MISSING - this car is not drawn");
		}
		else
		{
			gMpCtx->jer_log(gMpCtx,
				"[mp] car status: driving resident %d, which is out of range - this car is not drawn\n",
				slot);
		}
	}

	return changed;
}

/* A mode with role cars, at every COUNTDOWN: the car of the role this player
 * has this round (Cops n Robbers: the patrol car for a cop, the getaway car for
 * the getaway). The player's own pick for that role first (ROLE CARS, above);
 * without one, the role's pool decides: a cop goes into the level's police car
 * (Driver 2's model 0), the getaway into the first of the level's own cars that
 * is not a police car. Through the pause picker's own door (MpChangeCarRun),
 * so an import waits and retries and the change travels as any other. Nothing
 * to do when the car already fits. */
void MpRoleCarApply(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	int mode = gMp.gamemode, roleSlot, pool, slot, police;
	extern u_char defaultPlayerPalette;

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS || player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
		return;

	roleSlot = MpModeRoleSlotFor(mode, MpMarkMarked() == me->id);

	if (roleSlot < 0)
		return;

	MpRolePicksFor(mode);
	pool = MpModeRoleSlotPool(mode, roleSlot);
	slot = car_data[me->carId].ap.model;

	if (gMpRolePick[roleSlot].set)
	{
		MP_ROLE_PICK* r = &gMpRolePick[roleSlot];
		int want = MpResidentSlotForCar(r->city, r->model);

		if (want >= 0 && want == slot)
			return;		/* already in it */

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] role car: player %d -> %s (%s model %d)\n",
				me->id, MpModeRoleSlotTitle(mode, roleSlot), MpCarCityName(r->city), r->model);

		defaultPlayerPalette = (u_char)r->palette;	/* the paint picked with it (MpAdoptRemoteCar reads it) */
		MpChangeCarRun(r->city, r->model, 0);
		return;
	}

	police = JerCarSlotIsPolice(slot);

	if (MpModePoolKeeps(pool, police))
		return;

	if (pool == MP_MODE_POOL_POLICE)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] role car: player %d has no %s pick - into the level's police car\n",
				me->id, MpModeRoleSlotTitle(mode, roleSlot));
		MpChangeCarRun(-1, 0, 0);	/* Driver 2's police body, model 0 */
		return;
	}

	{
		int s2;

		for (s2 = 0; s2 < MAX_CAR_RESIDENT_MODELS; s2++)
		{
			if (residentCarModels[s2] < 0 || JerCarSlotIsPolice(s2) ||
				GetCarModelSourceCity(s2) >= 0 || !JerCarSlotUsable(s2))
				continue;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] role car: player %d has no %s pick - out of the police car (model %d)\n",
					me->id, MpModeRoleSlotTitle(mode, roleSlot), (int)residentCarModels[s2]);
			MpChangeCarRun(-1, residentCarModels[s2], 0);
			return;
		}
	}
}

/* A player is on foot and this machine has no body for the car they left: build
 * the parked car from the NO_CAR row. A machine that was present for the get-out
 * already has it; this exists for a joiner that arrived afterwards, which
 * otherwise sees only the player's pedstate stand-in.
 *
 * The body is built through the same InitPlayer path as every other remote car,
 * then parked exactly as the owner's own get-out parks it (CIV_AI + STOP/EMPTY +
 * the quiet pad), so TannerCanEnterCar accepts it and the traffic AI ignores it. */
void MpMaterializeLeftCar(MP_PLAYER* pl, const MP_CARSTATE_ENTRY* e)
{
	static unsigned long lastLogMs;
	int slot, k, resident, rot, city;

	if (pl == NULL || e == NULL || !gMp.running)
		return;

	/* Already have it? A claim that still names a parked body is enough. Do not
	 * compare the model: a re-model (the owner's Change car) is still the same
	 * body, and building a second one would be the duplicate this avoids. */
	if (pl->leftCarSlot1 > 0)
	{
		int have = pl->leftCarSlot1 - 1;

		/* Someone is DRIVING it -- the local player may have just commandeered
		 * it, and MpFollowLocalCar may not have adopted that yet. Drop the stale
		 * claim and do not build a second body. */
		if (have >= 0 && have < MAX_CARS && car_data[have].controlType == CONTROL_TYPE_PLAYER)
		{
			pl->leftCarSlot1 = 0;
			pl->leftCarModel = -1;
			return;
		}

		/* Someone else drives or claims it now: the claim moved with the body.
		 * Drop our stale claim and do NOT build a second car -- this is the
		 * commandeer case, and it is the difference between one body and two. */
		if (MpCarSlotTakenByOther(pl, have))
		{
			pl->leftCarSlot1 = 0;
			pl->leftCarModel = -1;
			return;
		}

		if (have >= 0 && have < MAX_CARS &&
			car_data[have].controlType == CONTROL_TYPE_CIV_AI &&
			car_data[have].ai.padid == &gMpQuietPad &&
			car_data[have].ap.carCos != NULL)
		{
			/* long long: a far-off pose must not overflow the subtraction; either axis
			 * at or past the snap distance is far, rejected before squaring */
			long long dx = (long long)car_data[have].hd.where.t[0] - e->x;
			long long dz = (long long)car_data[have].hd.where.t[2] - e->z;

			if (dx > -MP_INTERP_SNAP_DIST && dx < MP_INTERP_SNAP_DIST &&
				dz > -MP_INTERP_SNAP_DIST && dz < MP_INTERP_SNAP_DIST &&
				dx * dx + dz * dz < (long long)MP_INTERP_SNAP_DIST * MP_INTERP_SNAP_DIST)
			{
				pl->leftCarModel = car_data[have].ap.model;
				return;
			}

			/* ...but not one nowhere near where the owner says it is parked: that is a
			 * car this machine built for them (a net spawn at the level start), not the
			 * car they left. It is ours and nobody else's (tested above): remove it and
			 * build the real body at the owner's pose below. */
			MpReleaseOurCarSlot(have);
			car_data[have].controlType = CONTROL_TYPE_NONE;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] left car: player %d's claim on slot %d is %lld,%lld from the parked pose - removed, rebuilding\n",
					pl->id, have, dx, dz);

			pl->leftCarSlot1 = 0;
			pl->leftCarModel = -1;
		}

		if (pl->leftCarSlot1 > 0 && gMpCtx != NULL && (MpNowMs() - lastLogMs) > 2000)
		{
			lastLogMs = MpNowMs();
			gMpCtx->jer_log(gMpCtx,
				"[mp] left car: player %d's claim on slot %d is not a parked body "
				"(type %d pad %d cos %s) - rebuilding\n",
				pl->id, have,
				(have >= 0 && have < MAX_CARS) ? (int)car_data[have].controlType : -1,
				(have >= 0 && have < MAX_CARS && car_data[have].ai.padid == &gMpQuietPad) ? 1 : 0,
				(have >= 0 && have < MAX_CARS && car_data[have].ap.carCos != NULL) ? "present" : "NULL");
		}

		pl->leftCarSlot1 = 0;
		pl->leftCarModel = -1;
	}

	if (e->leftModel == MP_CARSTATE_NO_CAR)
		return;

	city = MpCityFromWire(e->leftModelCity);
	resident = MpResidentSlotForCar(city, (int)e->leftModel);

	if (resident < 0 || !JerCarSlotUsable(resident))
	{
		if (gMpCtx != NULL && (MpNowMs() - lastLogMs) > 2000)
		{
			lastLogMs = MpNowMs();
			gMpCtx->jer_log(gMpCtx,
				"[mp] left car: player %d left %s model %d, which this machine does not hold - not built\n",
				pl->id, MpCarCityName(city), (int)e->leftModel);
		}
		return;
	}

	/* The same slot rule as a join: a free PLAYER row, never slot 0 or a car
	 * anyone drives or claims. The second pass allows a quiet-pad body only when
	 * nothing else is free; the ownership test still rejects another row's car. */
	slot = -1;
	for (k = 1; k < MAX_CARS && k < MAX_PLAYERS; k++)
	{
		if (car_data[k].controlType == CONTROL_TYPE_CIV_AI && car_data[k].ai.padid == &gMpQuietPad)
			continue;
		if (MpCarSlotOwnedByPlayer(k, -1))
			continue;
		slot = k;
		break;
	}

	for (k = 1; slot < 0 && k < MAX_CARS && k < MAX_PLAYERS; k++)
	{
		if (car_data[k].controlType == CONTROL_TYPE_CIV_AI && car_data[k].ai.padid == &gMpQuietPad &&
			!MpCarSlotOwnedByPlayer(k, -1))
			slot = k;
	}

	if (slot < 1 || slot >= MAX_CARS || slot >= MAX_PLAYERS)
	{
		if (gMpCtx != NULL && (MpNowMs() - lastLogMs) > 2000)
		{
			lastLogMs = MpNowMs();
			gMpCtx->jer_log(gMpCtx,
				"[mp] left car: no free car slot for player %d's model %d\n",
				pl->id, (int)e->leftModel);
		}
		return;
	}

	rot = (int)(e->heading & 0xfff);
	{
		LONGVECTOR4 start;
		char padid = (char)-slot;
		MATRIX m;

		start[0] = e->x;
		start[1] = e->y;
		start[2] = e->z;
		start[3] = 0;

		InitPlayer(&player[slot], &car_data[slot], CONTROL_TYPE_PLAYER, rot, &start,
			(int)e->leftModel, (int)e->palette, &padid);

		car_data[slot].ap.needsDenting = 1;
		car_data[slot].hd.where.t[0] = e->x;
		car_data[slot].hd.where.t[1] = e->y;
		car_data[slot].hd.where.t[2] = e->z;
		car_data[slot].hd.direction = rot;

		_RotMatrixY(&m, (short)rot);
		memcpy(car_data[slot].hd.where.m, m.m, sizeof(car_data[slot].hd.where.m));
	}

	car_data[slot].controlType = CONTROL_TYPE_CIV_AI;
	car_data[slot].ai.c.thrustState = CIV_AI_THRUST_STOP;
	car_data[slot].ai.c.ctrlState = CIV_AI_CTRL_EMPTY;
	car_data[slot].wheel_angle = 0;
	car_data[slot].ai.padid = &gMpQuietPad;
	gMpOurCars[slot] = 1;

	pl->leftCarSlot1 = slot + 1;
	pl->leftCarModel = car_data[slot].ap.model;
	pl->carId = -1;
	pl->onFoot = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] left car: materialized player %d's parked car (slot %d, model %d) at %d,%d\n",
			pl->id, slot, (int)e->leftModel, (int)e->x, (int)e->z);
}

int MpFindEnteredParkedBody(const MP_PLAYER* pl, const MP_CARSTATE_ENTRY* e)
{
	MP_PLAYER* me = MpLocalPlayer();
	int wantCity = MpCityFromWire(e->modelCity);
	int resident = MpResidentSlotForCar(wantCity, (int)e->model);
	int k, best = -1;
	double bestD2 = (double)MP_ENTER_ADOPT_DIST * MP_ENTER_ADOPT_DIST;

	if (resident < 0)
		return -1;

	for (k = 1; k < MAX_CARS; k++)
	{
		CAR_DATA* cp = &car_data[k];
		double dx, dz, d2;

		if (k == (int)player[0].playerCarId || (me != NULL && k == me->carId))
			continue;
		if (gMpPeerTraffic[k])
			continue;
		if (cp->controlType != CONTROL_TYPE_CIV_AI || cp->ai.padid != &gMpQuietPad)
			continue;
		if (cp->ap.carCos == NULL || cp->ap.model != resident || !JerCarSlotUsable(cp->ap.model))
			continue;
		if (MpGetPlayerByCar(k) != NULL)
			continue;

		dx = (double)e->x - (double)cp->hd.where.t[0];
		dz = (double)e->z - (double)cp->hd.where.t[2];
		d2 = dx * dx + dz * dz;

		if (d2 < bestD2)
		{
			bestD2 = d2;
			best = k;
		}
	}

	(void)pl;
	return best;
}
