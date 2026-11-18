/*
 * mp_spawn.c -- split out of mp_session.c
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"

#define MP_EXTRA_SLOTS	2		/* the level's spare resident slots: 5 and 6 */
#define MP_JOIN_CLEAR	2000

/* The car model each city keeps in its SPECIAL slot (slot 7). MEASURED, by booting
 * each level and reading the residents the engine prints:
 *
 *     chicago   1 2 3 0 4 -1 -1 10
 *     havana    1 2 3 0 4 -1 -1 11
 *     lasvegas  1 3 4 0 2 -1 -1 9
 *     rio       1 2 3 0 4 -1 -1 8
 *
 * So EVERY level seats five usable car models (slots 0..4 -- the set {0,1,2,3,4},
 * in a different order per city), leaves slots 5 and 6 EMPTY (mission.c defaults
 * them to -1 and says they exist for modules to fill), and keeps its own special in
 * slot 7. Five cars is fewer than MP_MAX_PLAYERS, which is exactly what made "every
 * player drives their own car" impossible past the fifth player: the assignment
 * wrapped and handed two players the same model. */
static const int MP_CITY_SPECIAL[4] = { 10, 11, 9, 8 };	/* chicago, havana, lasvegas, rio */

/* Which extra models mp puts in those slots, per session city -- MEASURED against
 * the level's own car-model table, not guessed (see MP_EXTRA_SLOTS below for how).
 *
 * They are read from the level's OWN file (modelSource = -1), which is the only
 * thing that works: the engine's importer holds ONE source city per level
 * (models.c:446 picks the first slot that names one), so asking for a different
 * city per slot silently looks the model up in the FIRST city's table -- a
 * havana-coded spare slot was read against chicago's file and reported "which has
 * no such model". Reading the level's own table by model index has no such trap.
 *
 * The numbers stay distinct from the five domestic ones and from the session
 * city's own special, so no two player ids can be handed the same car. */
static const int MP_CITY_EXTRA[4][MP_EXTRA_SLOTS] = {
	{ 9, 11 },	/* chicago  (its special is 10) */
	{ 9, 10 },	/* havana   (its special is 11) */
	{ 8, 10 },	/* lasvegas (its special is 9)  */
	{ 9, 10 },	/* rio      (its special is 8)  */
};

/* What the spare slots HOLD this level, as mp left them. The assignment below has
 * to name models that are really resident: naming one a level does not have is not
 * a cosmetic failure, because the car keeps the model it had and the machines then
 * disagree about who drives what. */
static int gMpExtraModel[MP_EXTRA_SLOTS];

static int gMpExtraSet;

static int MpExtraModel(int lvl, int which);
int MpCarSlotOwnedByPlayer(int slot, int ignoreId);
int MpJoinSpotClearOfPlayers(int x, int z, int ignoreId, int ignoreSlot);

#define MP_PLACE_SELF_TRIES	90
#define MP_RESPAWN_CLEAR	3000	/* no other player's car this close to the spot */
#define MP_RESPAWN_MAX		30000
#define MP_RESPAWN_MIN		12000
#define MP_RESPAWN_TRIES	12	/* per pass */
#define MP_ROAD_AXIS_DIRS	16	/* directions the axis probe tries */
#define MP_ROAD_AXIS_PROBE	500	/* how far along a road the heading test looks */
#define MP_ROAD_AXIS_STEPS	6	/* ...each out to MP_ROAD_AXIS_PROBE * this, both ways */
#define MP_ROAD_HALF_WIDTH	450	/* a car's half-width: the footprint must fit the road */
#define MP_ROAD_PROBES		12	/* points probed around each ring */
#define MP_ROAD_RINGS		12	/* 12 rings = 7200 units out */
#define MP_ROAD_RING_STEP	600	/* units between rings */
#define MP_WARP_CLEAR	300
#define MP_WARP_PED_FRESH_MS	5000

static int gMpPlaceSelfHoldY;	/* the y the car is pinned at while placement runs */

int gMpRespawnDeferred;		/* RESPAWN HOLD: a held death waits for the hold to end */
int gMpRespawnDelayArmed;	/* v24 MP_MODE_RESPAWN_DELAY: a death waits until... */
static unsigned gMpRespawnDelayAt;	/* ...this frame (MpRespawnHoldTick) */

static int gMpRespawnDelayShown;	/* the "Respawn in N" second last noticed */

static int gMpRespawnDelayNow;		/* the delayed respawn itself is running */

int gMpRespawnOut;		/* v24 MP_MODE_RESPAWN_NONE: out until the next COUNTDOWN */
int gMpSpectating;		/* v24: benched (WAITING / OUT), the camera on another car */
int gMpSpectateCar = -1;		/* the car slot the camera was last put on */
int gMp321Count;			/* v24 engine-style 3-2-1 (MpMarkDrawOverlay): the digit, 0 = none */
int gMp321Scale;			/* ...its PrintScaledString scale, 32 shrinking to 1 */
int gMpGoFrame = -1;		/* the frame the round went live straight out of a countdown */
static int gMpRespawnHeldLogged;	/* one log line per hold, not one per engine ask */

#define MP_WARP_NEAR	300
#define MP_WARP_FAR	2000

/* Where to put the car, in the target's own axes: `along` is measured along
 * their FORWARD axis and every value is <= 0, which is the definition of "not in
 * their trajectory". Beside first (least in the way), then behind, then further
 * out. */
static const int kWarpAlong[]  = { -MP_WARP_NEAR, -MP_WARP_NEAR, -900, -900, -1600, -MP_WARP_FAR, 0, 0 };

static const int kWarpAcross[] = { 600, -600, 900, -900, 0, 0, 900, -900 };

int MpPlaceOnGround(CAR_DATA* cp, int x, int z, int dir);
void MpClearDeathState(void);
static int MpRespawnCarSlot(MP_PLAYER* me);
static int MpRespawnFreshCar(MP_PLAYER* me, int fromSlot, PEDESTRIAN* ped);
static int MpRespawnOnFoot(MP_PLAYER* me);
static int MpRespawnHeldNow(void);
static int MpRespawnPolicyNow(int* outFrames);
static int MpRespawnRoundLive(void);
static int MpRespawnWaiting(void);
static void MpRespawnDelayTick(void);
static int MpHeldDeathAnswered(void);
int MpRespawnAfterDeath(void);
int MpWarpSpotOk(int x, int z);
int MpWarpStreamTo(int x, int z);
static int MpRoadLaneHeading(int x, int y, int z, int* outDir);
int MpRoadHeadingAt(int x, int y, int z, int* outDir);
static int MpRoadFootprintOk(int x, int y, int z, int dir);
int MpFindRoadSpotNear(int cx, int cy, int cz, int seed, int* outX, int* outZ, int* outDir, int* outRing);
static int MpFindRespawnSpot(int cx, int cz, int* outX, int* outZ, int* outDir);
static int MpRoadHeadingNear(int x, int z, int want);
static int MpPlaceSelfAt(CAR_DATA* cp, int x, int z, int dir, const char* how, int attempt);
static int MpStreamRegionAt(int x, int z);
int MpFindRoadSpotWide(int carId, int* outX, int* outZ, int* outDir);
static int MpPlaceSelfFallback(CAR_DATA* cp, int carId, int seed);
static int MpPlaceOwnPedAt(LPPEDESTRIAN ped, int x, int z, int dir);

/* Called from MpSessionReset: this module's state is per session, so
 * the next match starts from the same values the module loaded with. */
void MpSpawnReset(void)
{
	gMpPlaceSelfHoldY = 0;
	gMpRespawnHeldLogged = 0;
}

/* Place a car at (x,z) on the GROUND there, the way the engine's own InitCar does
 * (civ_ai.c: MapHeight at the target minus the suspension offset). A bare
 * InitCarPhysics with y=0 puts the car at world zero, which on a road that sits
 * at y=2048 is 2048 units BELOW it - the car then falls through the map. That is
 * the "respawn, then keep falling" loop: every placement repeated the drop.
 *
 * 0 is also MapHeight's answer for ground that is not resident, so the caller has
 * to have made the spot real first (MpWarpStreamTo) - the same rule the warp and
 * the join spawn already follow. Returns 0 when there is no ground to stand on. */
int MpPlaceOnGround(CAR_DATA* cp, int x, int z, int dir)
{
	LONGVECTOR4 start;
	VECTOR probe;
	int gy;

	if (cp == NULL || cp->ap.carCos == NULL)
		return 0;

	probe.vx = x;
	probe.vy = 0;
	probe.vz = z;
	probe.pad = 0;

	gy = MapHeight(&probe);

	if (gy <= -1000)
		return 0;		/* the void: nothing to stand on */

	start[0] = x;
	start[1] = gy - cp->ap.carCos->wheelDisp[0].vy;
	start[2] = z;
	start[3] = 0;

	InitCarPhysics(cp, &start, dir);
	return 1;
}

/* Not dying, not wanted: the state the engine's death route counts down through.
 * Left standing, the wrecked banner and the playersdead game over re-arm. */
void MpClearDeathState(void)
{
	tannerDeathTimer = 0;
	player[0].dying = 0;
	player[0].upsideDown = 0;
	pedestrianFelony = 0;
}

/* The car slot a respawn should place: our row's car, and when the row has not caught
 * up yet (just got in, a car swap in flight) the engine's own player car. -1 = none. */
static int MpRespawnCarSlot(MP_PLAYER* me)
{
	int slot = (me != NULL) ? me->carId : -1;

	if (slot >= 0 && slot < MAX_CARS && car_data[slot].controlType != CONTROL_TYPE_NONE)
		return slot;

	if (player[0].playerType == PLAYER_TYPE_CAR)
	{
		slot = player[0].playerCarId;

		if (slot >= 0 && slot < MAX_CARS && car_data[slot].controlType != CONTROL_TYPE_NONE &&
			car_data[slot].ap.carCos != NULL)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] respawn: row carId %d not usable - using engine playerCarId %d\n",
					me != NULL ? me->carId : -1, slot);
			return slot;
		}
	}

	return -1;
}

/* A FRESH CAR for an on-foot respawn whose parked car someone else has taken: the same
 * model and palette, built beside us on the nearest road through the same InitPlayer path
 * every other mp-built car uses, parked as ours (CIV_AI EMPTY, quiet pad, gMpOurCars) and
 * then entered through the engine's get-in. Nobody is kicked out of the old one. The slot
 * rule is the late-joiner's (1..MAX_PLAYERS-1, never a car a player owns), preferring a
 * free slot over recycling a stock traffic car. Returns 1 when we are in the new car. */
static int MpRespawnFreshCar(MP_PLAYER* me, int fromSlot, PEDESTRIAN* ped)
{
	int k, slot = -1, bx = 0, bz = 0, bdir = -1, ring = 0;
	int extModel, palette;

	if (me == NULL || ped == NULL || fromSlot < 0 || fromSlot >= MAX_CARS ||
		car_data[fromSlot].ap.model < 0 || car_data[fromSlot].ap.model >= MAX_CAR_RESIDENT_MODELS ||
		!JerCarSlotUsable(car_data[fromSlot].ap.model))
		return 0;

	extModel = residentCarModels[car_data[fromSlot].ap.model];
	palette = car_data[fromSlot].ap.palette;

	if (!MpFindRoadSpotNear(ped->position.vx, -ped->position.vy, ped->position.vz, -1, &bx, &bz, &bdir, &ring) ||
		!MpWarpStreamTo(bx, bz))
		return 0;

	for (k = 1; k < MAX_CARS && k < MAX_PLAYERS; k++)
	{
		if (car_data[k].controlType == CONTROL_TYPE_NONE && !reservedSlots[k] &&
			!gMpOurCars[k] && !MpCarSlotOwnedByPlayer(k, -1))
		{
			slot = k;
			break;
		}
	}

	/* No free slot: recycle a stock traffic car (never a cop, never one of ours). */
	for (k = 1; slot < 0 && k < MAX_CARS && k < MAX_PLAYERS; k++)
	{
		if (car_data[k].controlType == CONTROL_TYPE_CIV_AI &&
			!(car_data[k].controlFlags & CONTROL_FLAG_COP) &&
			!gMpOurCars[k] && !MpCarSlotOwnedByPlayer(k, -1) &&
			PingOutCar(&car_data[k]))
			slot = k;
	}

	if (slot < 1)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] respawn on foot: parked car taken and no free slot for a fresh one\n");
		return 0;
	}

	{
		LONGVECTOR4 start;
		char padid = (char)-slot;
		int dir = (bdir >= 0) ? bdir : MpRoadHeadingNear(bx, bz, 0);
		CAR_DATA* cp = &car_data[slot];
		LPPEDESTRIAN oldPed = player[0].pPed;

		start[0] = bx;
		start[1] = 0;
		start[2] = bz;
		start[3] = 0;

		InitPlayer(&player[slot], cp, CONTROL_TYPE_PLAYER, dir, &start, extModel, palette, &padid);

		if (cp->ap.carCos == NULL || !MpPlaceOnGround(cp, bx, bz, dir))
		{
			cp->controlType = CONTROL_TYPE_NONE;
			return 0;
		}

		/* Parked as ours first, so TannerCanEnterCar-style state and the ping-out guard
		 * hold from this frame, then the engine's own get-in. */
		cp->controlType = CONTROL_TYPE_CIV_AI;
		cp->ai.c.thrustState = CIV_AI_THRUST_STOP;
		cp->ai.c.ctrlState = CIV_AI_CTRL_EMPTY;
		cp->wheel_angle = 0;
		cp->ai.padid = &gMpQuietPad;
		gMpOurCars[slot] = 1;
		gJerCarNoPingOut[slot] = 1;

		ChangePedPlayerToCar(0, cp);
		RemovePlayerPedestrian(oldPed);

		player[0].cameraAngle = cp->hd.direction + 1536;
		player[0].cameraCarId = cp->id;

		/* A different body from the one we left: forget that claim (it is the other
		 * player's car now) and adopt this one now rather than next frame. */
		me->leftCarSlot1 = 0;
		me->leftCarModel = -1;
		MpFollowLocalCar();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] respawn on foot: parked car (slot %d) is taken - fresh model %d in slot %d at %d,%d\n",
				fromSlot, extModel, slot, bx, bz);
	}

	return 1;
}

/* ON-FOOT RESPAWN: back in the car we parked if it is still ours to take, a fresh car of
 * the same model if another player is in it, else onto the nearest road (streamed and grounded), else where we stand. Always clears the death
 * state; returns 1 when the player was moved, 0 when left in place. */
static int MpRespawnOnFoot(MP_PLAYER* me)
{
	PEDESTRIAN* ped = player[0].pPed;
	int slot = (me != NULL) ? me->leftCarSlot1 - 1 : -1;

	MpClearDeathState();

	if (slot >= 0 && slot < MAX_CARS &&
		car_data[slot].controlType == CONTROL_TYPE_CIV_AI &&
		car_data[slot].ap.carCos != NULL &&
		car_data[slot].ap.model == me->leftCarModel &&
		MpGetPlayerByCar(slot) == NULL && ped != NULL)
	{
		CAR_DATA* cp = &car_data[slot];

		/* An upside-down parked car is righted where it stands first. */
		if (cp->hd.where.m[1][1] < 100)
			MpPlaceOnGround(cp, cp->hd.where.t[0], cp->hd.where.t[2], cp->hd.direction & 0xfff);

		/* The engine's get-in, and the ped put away after it (see the enter-left lever:
		 * ChangePedPlayerToCar clears pPed, the stale ped must not stay in the pool). */
		ChangePedPlayerToCar(0, cp);
		RemovePlayerPedestrian(ped);

		cp->felonyRating = 0;
		player[0].cameraAngle = cp->hd.direction + 1536;
		player[0].cameraCarId = cp->id;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] respawn on foot: back in our parked car (slot %d)\n", slot);

		return 1;
	}

	/* The parked car is still there but ANOTHER PLAYER is in it: a fresh car of the same
	 * model, not the nearest road and never kicking them out. */
	if (slot >= 0 && slot < MAX_CARS &&
		car_data[slot].controlType != CONTROL_TYPE_NONE &&
		car_data[slot].ap.carCos != NULL &&
		MpGetPlayerByCar(slot) != NULL && MpGetPlayerByCar(slot) != me &&
		MpRespawnFreshCar(me, slot, ped))
		return 1;

	if (ped != NULL)
	{
		int bx = 0, bz = 0, bdir = -1, ring = 0;
		VECTOR at;

		if (MpFindRoadSpotNear(ped->position.vx, -ped->position.vy, ped->position.vz, -1, &bx, &bz, &bdir, &ring) &&
			MpWarpStreamTo(bx, bz))
		{
			at.vx = bx;
			at.vy = 0;
			at.vz = bz;
			at.pad = 0;

			if (MapHeight(&at) > -1000)
			{
				ped->position.vx = bx;
				ped->position.vz = bz;
				ped->position.vy = -62 - MapHeight(&at);	/* standing height, as pedest.c places him */

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] respawn on foot: no parked car - nearest road %d,%d (ring %d)\n",
						bx, bz, ring);

				return 1;
			}
		}
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] respawn on foot: no parked car and no road - left in place\n");

	return 0;
}

/* RESPAWN HOLD (0.23.3). A round mode holds the normal respawn while the round
 * is over or counting down (MP_MODE_DESC.respawnHold, MpModeRespawnHeld): the
 * bomb holder's wreck at 0:00 set the car's damage to the limit, the engine's
 * death route asked for a game over, and the death respawn put that player back
 * on a road in the middle of the result screen. Held, a death is still refused
 * but the car stays where it is; MpMarkRoundReset repairs it when the next
 * countdown begins, and a death still pending when the hold ends without one
 * (ENDED back to WAITING) is respawned then, by MpRespawnHoldTick. The phase is
 * the referee's (the host's, or this client's JPMK mirror), so every machine
 * decides for its own car with nothing new on the wire. */
static int MpRespawnHeldNow(void)
{
	if (!MpModeUsesTimer(gMp.gamemode) || !MpMarkHaveState() || MpMarkMode() != gMp.gamemode)
		return 0;

	return MpModeRespawnHeld(gMp.gamemode, MpMarkPhase());
}

/* v24: the respawn policy for this death (MP_MODE_RESPAWN_*), *outFrames = DELAY's
 * frames. TEST LEVER (MP_TEST_RESPAWN_POLICY=delay|none): every mode answers a death that
 * way (delay = 3 s), so the paths can be played before any mode ships them. Inert
 * unless set. */
static int MpRespawnPolicyNow(int* outFrames)
{
	static int test = -2;
	int policy;

	if (test == -2)
	{
		const char* s = getenv("MP_TEST_RESPAWN_POLICY");

		test = (s == NULL) ? -1 : (strcmp(s, "delay") == 0) ? MP_MODE_RESPAWN_DELAY :
			(strcmp(s, "none") == 0) ? MP_MODE_RESPAWN_NONE : -1;

		if (test >= 0 && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] TEST LEVER MP_TEST_RESPAWN_POLICY=%s - every mode's respawn policy is %d\n", s, test);
	}

	policy = (test >= 0) ? test : MpModeRespawnPolicy(gMp.gamemode);
	*outFrames = (policy != MP_MODE_RESPAWN_DELAY) ? 0 :
		(test >= 0) ? 90 : MpModeRespawnDelayFrames(gMp.gamemode);
	return policy;
}

/* v24: a round mode's live round, with us in it -- where RESPAWN_NONE means out. */
static int MpRespawnRoundLive(void)
{
	MP_PLAYER* me = MpLocalPlayer();

	return me != NULL && MpModeUsesTimer(gMp.gamemode) && MpMarkHaveState() &&
		MpMarkMode() == gMp.gamemode && MpMarkPhase() == MP_MARK_PHASE_LIVE && MpMarkInRound(me->id);
}

/* v24: out, or waiting for the delayed respawn -- the pause menu's respawn is refused. */
static int MpRespawnWaiting(void)
{
	return gMpRespawnOut || gMpRespawnDelayArmed;
}

/* v24 MP_MODE_RESPAWN_DELAY, every frame: "Respawn in N", then the placement. */
static void MpRespawnDelayTick(void)
{
	int left;

	if (!gMpRespawnDelayArmed)
		return;

	left = (int)(gMpRespawnDelayAt - gMp.frame);

	if (left > 0)
	{
		int sec = (left + 29) / 30;

		if (sec != gMpRespawnDelayShown)
		{
			char line[32];

			snprintf(line, sizeof(line), "Respawn in %d", sec);
			jer_notify(line, 3, 1);
			gMpRespawnDelayShown = sec;
		}
		return;
	}

	gMpRespawnDelayArmed = 0;
	gMpRespawnDelayNow = 1;
	MpRespawnAfterDeath();
	gMpRespawnDelayNow = 0;
}

/* A deferred death that is no death any more: our car is whole (under the
 * mission's wreck point) and has ground under it. */
static int MpHeldDeathAnswered(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	int slot = MpRespawnCarSlot(me);
	CAR_DATA* cp;
	VECTOR probe;

	if (slot < 0 || player[0].playerType != PLAYER_TYPE_CAR)
		return 0;

	cp = &car_data[slot];

	if (MaxPlayerDamage[0] > 0 && cp->totalDamage >= MaxPlayerDamage[0])
		return 0;

	probe.vx = cp->hd.where.t[0];
	probe.vy = 0;
	probe.vz = cp->hd.where.t[2];
	probe.pad = 0;

	return MapHeight(&probe) > -1000 && cp->hd.where.t[1] > -1000;
}

/* Every frame in a match (MpMarkPresentTick). */
void MpRespawnHoldTick(void)
{
	if (!gMp.running)
	{
		gMpRespawnDeferred = 0;
		gMpRespawnHeldLogged = 0;
		gMpRespawnDelayArmed = 0;
		gMpRespawnOut = 0;
		return;
	}

	/* v24: OUT lasts only while the round it happened in is live */
	if (gMpRespawnOut && !(MpModeUsesTimer(gMp.gamemode) && MpMarkHaveState() &&
		MpMarkMode() == gMp.gamemode && MpMarkPhase() == MP_MARK_PHASE_LIVE))
	{
		gMpRespawnOut = 0;
		gMpRespawnDeferred = 1;	/* answered below, or by the round reset */
	}

	if (MpRespawnHeldNow())
	{
		gMpRespawnDelayArmed = 0;	/* the round reset places the car */
		return;
	}

	MpRespawnDelayTick();

	gMpRespawnHeldLogged = 0;

	if (!gMpRespawnDeferred)
		return;

	gMpRespawnDeferred = 0;

	/* ALREADY ANSWERED: the wreck's game-over ask can land again after the
	 * countdown's reset (the engine's death delay was already armed), which
	 * re-deferred a death the round start had just repaired and placed. Respawning
	 * it at GO threw the car off its start spot onto a random road (0.24.0 log:
	 * "placed at 2210,-220695 heading 3072", then "respawn hold over (phase 1)" and
	 * a ring spot 11k away). A car that is whole and on the ground stays. */
	if (MpHeldDeathAnswered())
	{
		MpClearDeathState();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] respawn hold over (phase %d) - the round start already placed the car, no respawn\n",
				MpMarkPhase());
		return;
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] respawn hold over (phase %d) - the held death respawns now\n",
			MpMarkPhase());

	MpRespawnAfterDeath();
}

/* IN A SESSION, DYING IS A RESPAWN, NOT A GAME OVER.
 *
 * The engine asks (JER_PAUSE_GAMEOVER) just before it arms the game-over pause - the death
 * fade (gDieWithFade, set when the car drops below y = -1000) reaching its end, or a mission
 * asking for it. Answering "handled" IS the refusal: the engine leaves the pause unarmed and
 * drops the fade instead of blackening the screen, and the player is put back on the map
 * HERE, so by the time the engine asks the car is no longer dying.
 *
 * The placement is the engine's own (InitCarPhysics: the rigid-body quaternion and the
 * fixed-point fposition, NOT hd.where) for the reason the soft restart above learned the hard
 * way - writing hd.where alone leaves the body where it was, at the roll it had, and the next
 * physics step snaps the car back onto it. The camera is re-seated with it, because
 * cameraAngle is an absolute world yaw.
 *
 * The car goes back WHERE IT IS when that is still on the map, and to the level's own start
 * when it is not: "in place" cannot mean the void, and MapHeight answering with a sane height
 * is what says which of the two this is.
 *
 * Returns 1 when the game over was refused, 0 to let the engine have it - no session, or a
 * player with no car (on foot the engine's death test never fires, so a game over there is
 * not this one).
 */
int MpRespawnAfterDeath(void)
{
	MP_PLAYER* me;
	CAR_DATA* cp;
	VECTOR probe;
	int ground;
	int x, z, dir;
	int slot;

	if (!gMp.running)
		return 0;

	/* GAME-MODE DECISION POINT: the mode may HOLD the respawn (the round is over
	 * or counting down). Still no game over: the death state is cleared and the
	 * car is left where it is, wreck latch and all, for the round reset. */
	if (MpRespawnHeldNow())
	{
		MpClearDeathState();
		gMpRespawnDeferred = 1;

		if (!gMpRespawnHeldLogged && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] death in a session while mode %d holds respawn (phase %d) - game over refused, no respawn until the round resets\n",
				gMp.gamemode, MpMarkPhase());
		gMpRespawnHeldLogged = 1;

		return 1;
	}

	/* GAME-MODE DECISION POINT (v24): the mode chooses HOW a death is answered,
	 * never WHETHER -- whatever it says, the game over is refused. INSTANT is the
	 * placement below; DELAY leaves the wreck where it is for the mode's frames
	 * (MpRespawnHoldTick places it then); NONE, in a live round, takes this player
	 * out until the next COUNTDOWN: the car stays put, the host benches them
	 * (MpHostBenchOut, or MP_CARSTATE_OUT on our rows) and MpSpectateTick watches
	 * the round. */
	{
		int frames = 0;
		int policy = MpRespawnPolicyNow(&frames);

		if (gMpRespawnOut || (policy == MP_MODE_RESPAWN_NONE && MpRespawnRoundLive()))
		{
			MpClearDeathState();

			if (!gMpRespawnOut)
			{
				MP_PLAYER* outMe = MpLocalPlayer();

				gMpRespawnOut = 1;
				jer_notify("You're out - spectating until the next round", 4, 4);

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] death in mode %d with no respawn - out until the next round\n",
						gMp.gamemode);

				if (MpIsHost() && outMe != NULL)
					MpHostBenchOut(outMe->id, "own car");
			}

			return 1;
		}

		if (policy == MP_MODE_RESPAWN_DELAY && frames > 0 && !gMpRespawnDelayNow)
		{
			MpClearDeathState();

			if (!gMpRespawnDelayArmed)
			{
				gMpRespawnDelayArmed = 1;
				gMpRespawnDelayAt = gMp.frame + (unsigned)frames;
				gMpRespawnDelayShown = -1;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] death in mode %d - respawn in %d frames\n", gMp.gamemode, frames);
			}

			return 1;
		}
	}

	me = MpLocalPlayer();

	/* From here on, IN A SESSION THE ANSWER IS ALWAYS "REFUSED": a failed placement
	 * leaves the player where they are (not dying, not wanted) rather than handing the
	 * engine a game over that tears the session down on this machine. */
	if (me == NULL)
	{
		MpClearDeathState();
		return 1;
	}

	/* ON FOOT: busted by a cop (the "You've been caught" route), drowned or hit by
	 * a train. The engine's game over there tore the session down -- the menu
	 * opened over a world the camera no longer had a target in, and the player
	 * had to rejoin. Refuse it the same way: not dying, not wanted, and off the
	 * water if that is what was killing us. */
	if (player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
	{
		MpRespawnOnFoot(me);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] death on foot in a session - game over refused\n");

		return 1;
	}

	slot = MpRespawnCarSlot(me);

	if (slot < 0)
	{
		MpClearDeathState();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] death in a session with no car slot we can place - game over refused, left in place\n");

		return 1;
	}

	cp = &car_data[slot];

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] respawn reason: engine game over (car) - slot %d at %d,%d,%d damage %d/%d dying %d upsideDown %d felony %d; last rx %s conn %d frame %lu (now %lu)\n",
			slot, cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2],
			cp->totalDamage, MaxPlayerDamage[0], (int)player[0].dying, (int)player[0].upsideDown,
			(int)cp->felonyRating, gMpLastRxTag, gMpLastRxConn, gMpLastRxFrame, gMp.frame);

	/* Placed on EVERY ask, because "the engine wants a game over" IS the dying signal - the
	 * car's own y is not: the engine can already have clamped where.t[1] back to the ground
	 * while the fade and the mission's game-over delay are still counting. What keeps this
	 * from running per frame is the ENGINE: it clears Mission.gameover_delay when we refuse
	 * (see JerGameOverRefused), so it asks once per death, not once per frame. */
	x = cp->hd.where.t[0];
	z = cp->hd.where.t[2];
	dir = cp->hd.direction;

	probe.vx = x;
	probe.vy = 0;
	probe.vz = z;
	probe.pad = 0;

	/* A sane ground height means this x/z is on the map. Anything else is the void. */
	ground = MapHeight(&probe);

	if (ground <= -1000 && jer_map_ready() &&
		jer_map_region_has_data(jer_map_region_of(x, z)))
	{
		/* FELL THROUGH, ON THE MAP: the ring search below works from where the car
		 * went down, not from the level start. Going to the start sent a Cops n
		 * Robbers getaway 100k units off and handed it the escape (0.24.0 log:
		 * "respawn ... ring around 87058,-455655" then "got away" the same second). */
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] respawn: no ground under %d,%d - respawning near there\n", x, z);
	}
	else if (ground <= -1000)
	{
		/* Off the map: stream the level start in and go there, if there is one;
		 * otherwise the ring search below works from where we are. */
		if (PlayerStartInfo[0] != NULL)
		{
			x = PlayerStartInfo[0]->position.vx;
			z = PlayerStartInfo[0]->position.vz;
			dir = PlayerStartInfo[0]->rotation;
		}
	}

	/* Face ALONG THE ROAD: a wreck usually ends at an angle (or upside down
	 * across the lanes), and respawning it that way left the driver to sort it out. */
	{
		int fx, fz, fdir = -1;

		if (MpFindRespawnSpot(x, z, &fx, &fz, &fdir) ||
			MpFindRoadSpotNear(x, 0, z, -1, &fx, &fz, &fdir, NULL))
		{
			x = fx;
			z = fz;
			if (fdir >= 0)
				dir = fdir;
		}
	}
	dir = MpRoadHeadingNear(x, z, dir & 0xfff);

	/* THE PLACEMENT RESOLVES THE GROUND. y=0 is not "the engine will sort it out":
	 * a road at y=2048 left the car two thousand units below it, falling. If the
	 * chosen spot is somehow void, fall back to the level start rather than drop
	 * the player. */
	/* STREAM BEFORE PLACING, the way the pause Respawn does: MapHeight answers the void
	 * for ground that simply is not resident yet, which used to end the session. */
	if (!MpPlaceOnGround(cp, x, z, dir) &&
		!(MpWarpStreamTo(x, z) && MpPlaceOnGround(cp, x, z, dir)))
	{
		int ok = 0;

		if (PlayerStartInfo[0] != NULL)
		{
			x = PlayerStartInfo[0]->position.vx;
			z = PlayerStartInfo[0]->position.vz;
			dir = PlayerStartInfo[0]->rotation;

			ok = MpPlaceOnGround(cp, x, z, dir) ||
				(MpWarpStreamTo(x, z) && MpPlaceOnGround(cp, x, z, dir));
		}

		if (!ok)
		{
			/* At worst: refuse and leave the car where it is. */
			MpClearDeathState();
			cp->felonyRating = 0;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] death in a session: no ground to place slot %d on - game over refused, left in place\n", slot);

			return 1;
		}
	}
	cp->hd.speed = 0;
	cp->wheel_angle = 0;

	/* REPAIRED, and handed back whole: totalDamage alone leaves the car looking caved in,
	 * because the DRAWN vertices only come back from the clean model through
	 * CreateDentableCar (the same reason the restart above needs it). */
	cp->totalDamage = 0;
	memset(cp->ap.damage, 0, sizeof(cp->ap.damage));
	CreateDentableCar(cp);
	CarReapplyHubcapState(cp->id);
	cp->ap.needsDenting = 0;

	/* NOT DYING ANY MORE, and NOT WANTED. Repairing the CAR is not enough: the mission's
	 * death route counts the player down through tannerDeathTimer -> player[].dying, and
	 * BOTH the "your vehicle's wrecked" banner and the playersdead game over hang off that
	 * counter -- so with it left standing the game kept saying the car was wrecked and kept
	 * re-arming the game over we had just refused (which is why a refusal without this
	 * respawned a few times before it settled, instead of once). */
	tannerDeathTimer = 0;
	player[0].dying = 0;
	player[0].upsideDown = 0;

	/* NO FELONY either - a forced respawn is a fresh start, not a wanted level carried
	 * over. GetPlayerFelony picks the car's rating or the pedestrian's, so both go, or
	 * stepping out puts the cops straight back on us. */
	cp->felonyRating = 0;
	pedestrianFelony = 0;

	player[0].cameraAngle = cp->hd.direction + 1536;
	player[0].cameraCarId = cp->id;

	{
		JER_ARGS_RESET_CAR rc;

		rc.carId = slot;
		jer_fire(JER_EVENT_RESET_CAR, &rc);
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] death in a session: respawned at %d,%d (ground=%d) in slot %d - game over refused\n",
			cp->hd.where.t[0], cp->hd.where.t[2], ground, slot);

	return 1;
}

/* Can this world point be LANDED on, and if not, can it be made so?
 *
 * Three questions in this order, and the order is the whole point:
 *
 *   1. does the REGION hold data (jer_map_region_has_data)? 0 means a void
 *      forever - there is nothing to stream in - so never land there.
 *   2. is it RESIDENT? If not, `spool` asks jer_map_spool_to to make it so
 *      (synchronous on PC) and this answers "not yet".
 *   3. only now the ground and the scenery.
 *
 * Asking the GROUND first is how a car lands in the void: an unstreamed
 * destination has MapHeight answer 0, and 0 is also a perfectly good height, so
 * `MapHeight > -1000` says "fine" about ground that is not there. Worse, the
 * spool has to be for THE POINT BEING TESTED - a candidate a couple of thousand
 * units from the one that was streamed is very likely a different region. */
int MpWarpSpotOk(int x, int z)
{
	VECTOR p;
	int region;

	if (!jer_map_ready())
		return 0;

	region = jer_map_region_of(x, z);

	if (!jer_map_region_has_data(region))
		return 0;	/* a void forever: there is nothing to stream in */

	if (!jer_map_region_resident(region))
		return 0;

	p.vx = x;
	p.vy = 0;
	p.vz = z;
	p.pad = 0;

	if (!CellEmpty(&p, MP_WARP_CLEAR))
		return 0;

	return MapHeight(&p) > -1000;
}

/* Make (x,z) landable if it can be, and say whether it now is.
 *
 * The streamer follows MainPlayer.spoolXZ, so it is AIMED FIRST and the stream is
 * then asked for -- the sequence the hop path and antfarm both use, and the
 * reason a bare jer_map_spool_to can quietly leave the window somewhere else.
 * The helper is skipped when the region is already resident, because in that case
 * it is not free (it still runs the load-area checks). The VECTOR must outlive
 * the call, hence the static.
 *
 * Refusing is a real answer: a spot whose region has no data can never be
 * streamed, and landing there is the void. */
int MpWarpStreamTo(int x, int z)
{
	static VECTOR sWarpSpool;
	VECTOR* oldSpoolXZ;
	int region, ok;

	if (MpWarpSpotOk(x, z))
		return 1;

	if (!jer_map_ready())
		return 0;

	region = jer_map_region_of(x, z);

	if (!jer_map_region_has_data(region))
		return 0;

	/* ALREADY RESIDENT: the spot failed on its scenery or its height, not on the
	 * stream, and streaming cannot change that. jer_map_spool_to on a resident
	 * region is a synchronous CheckLoadAreaData + StartSpooling + UpdateSpool pass
	 * (jer_map.c), and that pass is what stalled the pair and 3-seat runs (the
	 * gather's measured note). Callers retry this per frame (the chase every 15
	 * tries, the respawn ring per try), which turned one stall into a spool storm:
	 * the 0.23.4 and 0.24.0 hangs end in hundreds of StartSpooling CD warnings. */
	if (jer_map_region_resident(region))
		return 0;

	/* HAND THE STREAMER BACK WHEN THE CALL ENDS. jer_map_spool_to reads
	 * MainPlayer.spoolXZ to decide where to stream, and the caller's pointer is the
	 * engine's own (the player car's hd.where.t, or the pedestrian's position).
	 * Leaving it on a module static -- which this used to do -- strands the
	 * streamer at the destination for the rest of the match: the car drives away
	 * and the world stops following it. Only the spool call itself needs the
	 * static, because ControlMap must read the destination while it runs. */
	oldSpoolXZ = MainPlayer.spoolXZ;

	sWarpSpool.vx = x;
	sWarpSpool.vy = 0;
	sWarpSpool.vz = z;
	sWarpSpool.pad = 0;

	MainPlayer.spoolXZ = &sWarpSpool;

	jer_map_spool_to(x, z);

	MainPlayer.spoolXZ = oldSpoolXZ;

	ok = MpWarpSpotOk(x, z);	/* synchronous on PC, so readable now */

	return ok;
}

/* The heading a car should face to DRIVE the road at (x,z): along it, the way
 * the traffic in that lane goes.
 *
 * The answer is the engine's own civ-car rule (civ_ai.c InitCivState, the
 * targetRoute[0].dir it gives a car it places on a road):
 *   straight: the lane under the point (GetLaneByPositionOnRoad, dr2roads.c),
 *             then straight->angle, or angle + 2048 when ROAD_LANE_DIR says that
 *             lane runs the other way;
 *   curve:    the TANGENT at the point -- ratan2 of the point about the curve's
 *             centre, +1024 or -1024 by the lane's direction.
 *
 * What this replaced (0.23.0): the
 * bare straight->angle (the lane's direction ignored, so half the spawns faced
 * the oncoming traffic), curve->start (a radial START angle, not the tangent
 * where the car is, so a car on a bend sat across it), and an axis probe that
 * took the FIRST of eight directions with road both ways -- on a wide road or a
 * junction every direction has road both ways, so that was simply 0 (north).
 *
 * Returns 2 for a lane heading, 1 for the road's AXIS only (a junction or a cell
 * RoadInCell cannot read: the direction along the axis is not known, and the
 * caller may flip it), 0 for nothing. */
static int MpRoadLaneHeading(int x, int y, int z, int* outDir)
{
	VECTOR p;
	DRIVER2_ROAD_INFO info;
	int surf, lane;

	p.vx = x;
	p.vy = y;
	p.vz = z;
	p.pad = 0;

	/* THE PLANE IS PICKED AT THIS HEIGHT, so ask the ground rather than trusting a
	 * caller that only knows x/z (the join spawn passes 0). */
	p.vy = MapHeight(&p);

	surf = RoadInCell(&p);

	if (surf < 0 || !IS_ROAD_SURFACE(surf))
		return 0;

	memset(&info, 0, sizeof(info));

	if (!GetSurfaceRoadInfo(&info, surf))
		return 0;

	if (IS_STRAIGHT_SURFACE(surf) && info.straight != NULL)
	{
		lane = GetLaneByPositionOnRoad(&info, &p);

		if (lane < 0)
			lane = 0;

		*outDir = ROAD_LANE_DIR(&info, lane) ? ((info.straight->angle + 2048) & 0xfff)
			: (info.straight->angle & 0xfff);
		return 1;
	}

	if (IS_CURVED_SURFACE(surf) && info.curve != NULL)
	{
		int radial = ratan2(x - info.curve->Midx, z - info.curve->Midz) & 0xfff;

		lane = GetLaneByPositionOnRoad(&info, &p);

		if (lane < 0)
			lane = 0;

		*outDir = (ROAD_LANE_DIR(&info, lane) == 0 ? radial + 1024 : radial - 1024) & 0xfff;
		return 1;
	}

	return 0;
}

int MpRoadHeadingAt(int x, int y, int z, int* outDir)
{
	int i, best = -1, bestRun = 0, bestSum = 0;

	if (MpRoadLaneHeading(x, y, z, outDir))
		return 2;

	/* No segment under the point (a junction, or a cell RoadInCell cannot read --
	 * measured on CHICAGO at spawn points JerRoadAt calls road). The road's AXIS is
	 * the direction it runs FURTHEST both ways: on a junction or a wide road every
	 * direction has road at 500 units, so the first such direction is no answer. */
	for (i = 0; i < MP_ROAD_AXIS_DIRS / 2; i++)
	{
		int ang = (i * 4096) / MP_ROAD_AXIS_DIRS;
		int fwd = 0, back = 0, k, run;

		for (k = 1; k <= MP_ROAD_AXIS_STEPS; k++)
		{
			int fx = (int)(((long)rsin(ang) * MP_ROAD_AXIS_PROBE * k) >> 12);
			int fz = (int)(((long)rcos(ang) * MP_ROAD_AXIS_PROBE * k) >> 12);

			if (fwd == k - 1 && JerRoadAt(x + fx, y, z + fz))
				fwd = k;
			if (back == k - 1 && JerRoadAt(x - fx, y, z - fz))
				back = k;
		}

		run = fwd < back ? fwd : back;

		if (run > bestRun || (run == bestRun && run > 0 && fwd + back > bestSum))
		{
			best = ang;
			bestRun = run;
			bestSum = fwd + back;
		}
	}

	if (best < 0)
		return 0;

	/* The axis is known; its direction is the lane's of the nearest SEGMENT along
	 * it (out of the junction, at the same side offset, so the same lane). */
	{
		int k, sgn;

		for (k = 2; k <= MP_ROAD_AXIS_STEPS; k += 2)
		{
			for (sgn = 1; sgn >= -1; sgn -= 2)
			{
				int fx = sgn * (int)(((long)rsin(best) * MP_ROAD_AXIS_PROBE * k) >> 12);
				int fz = sgn * (int)(((long)rcos(best) * MP_ROAD_AXIS_PROBE * k) >> 12);
				int lane;

				if (MpRoadLaneHeading(x + fx, y, z + fz, &lane))
				{
					*outDir = lane;
					return 2;
				}
			}
		}
	}

	*outDir = best;
	return 1;
}

/* The car must FIT on the road, not merely touch it: `JerRoadAt` at the centre
 * cell is true on the shoulder too, and a car planted there overhangs the
 * drivable surface ("spawned out of bounds"). Probe the half-width to each side
 * of the resolved heading; a spot whose body leaves the road is refused. `dir`
 * < 0 means no heading was resolved (a junction) - then the centre test is all
 * we have. */
static int MpRoadFootprintOk(int x, int y, int z, int dir)
{
	int lx, lz, rx, rz;

	if (dir < 0)
		return 1;

	lx = (int)(((long)rsin(dir + 1024) * MP_ROAD_HALF_WIDTH) >> 12);
	lz = (int)(((long)rcos(dir + 1024) * MP_ROAD_HALF_WIDTH) >> 12);
	rx = (int)(((long)rsin(dir - 1024) * MP_ROAD_HALF_WIDTH) >> 12);
	rz = (int)(((long)rcos(dir - 1024) * MP_ROAD_HALF_WIDTH) >> 12);

	return JerRoadAt(x + lx, y, z + lz) && JerRoadAt(x + rx, y, z + rz);
}

/* A landable road point near (cx, cz). `pickRandom` chooses among the road points
 * on the FIRST ring that has any; otherwise the first usable one wins (the warp's
 * and respawn's "nearest"). Returns 1 and fills the outputs on success. The caller
 * still has to stream the winner - see MpWarpStreamTo. */
int MpFindRoadSpotNear(int cx, int cy, int cz, int seed,
	int* outX, int* outZ, int* outDir, int* outRing)
{
	int ring, i;
	unsigned int rnd = 0;

	if (seed >= 0)
	{
		/* The seed is the JOINING PLAYER'S id, not the local one: seeded with the
		 * local id the host picked the same spot for every peer it placed (measured:
		 * players 1 and 2 both landed on 87358,-456175, i.e. one lay-by, which is the
		 * thing this exists to avoid). Per-player and deterministic, so every machine
		 * agrees on where a given player comes in and a replay lands the same way. */
		rnd = gMp.seed ^ (unsigned int)((seed + 1) * 2654435761u);
	}

	for (ring = 1; ring <= MP_ROAD_RINGS; ring++)
	{
		int r = ring * MP_ROAD_RING_STEP;
		int cand[MP_ROAD_PROBES];
		int n = 0;

		for (i = 0; i < MP_ROAD_PROBES; i++)
		{
			/* a full turn is 4096, so the probes are evenly spaced by construction */
			int ang = (i * 4096) / MP_ROAD_PROBES;
			int px = cx + (int)(((long)rsin(ang) * r) >> 12);
			int pz = cz + (int)(((long)rcos(ang) * r) >> 12);

			if (!JerRoadAt(px, cy, pz))
				continue;

			if (!MpWarpSpotOk(px, pz))
				continue;

			/* The car must fit the road here, not just touch it: the centre cell
			 * being road is how a spawn lands on the shoulder. Probe the car's
			 * half-width to each side of the road's heading; a spot whose body
			 * overhangs the drivable surface is refused and the search goes on. */
			{
				int fdir = -1;

				if (MpRoadHeadingAt(px, cy, pz, &fdir) && !MpRoadFootprintOk(px, cy, pz, fdir))
					continue;
			}

			cand[n++] = i;

			if (seed < 0)
				break;		/* nearest: first usable on this ring */
		}

		if (n == 0)
			continue;

		i = cand[0];

		if (seed >= 0 && n > 1)
		{
			rnd = rnd * 1103515245u + 12345u;
			i = cand[(rnd >> 16) % (unsigned int)n];
		}

		{
			int ang = (i * 4096) / MP_ROAD_PROBES;

			*outX = cx + (int)(((long)rsin(ang) * r) >> 12);
			*outZ = cz + (int)(((long)rcos(ang) * r) >> 12);

			if (outDir != NULL && !MpRoadHeadingAt(*outX, cy, *outZ, outDir))
			{
				/* Neither the segment's angle nor the road's own axis: leave it to
				 * the caller's heading (the level start's) and say so in the log,
				 * rather than inventing a direction. The old "face tangentially to
				 * the probe" guess was 90 degrees wrong whenever the road ran away
				 * from the point instead of past it. */
				*outDir = -1;
			}
		}

		if (outRing != NULL)
			*outRing = ring;

		return 1;
	}

	return 0;
}

static int MpFindRespawnSpot(int cx, int cz, int* outX, int* outZ, int* outDir)
{
	unsigned int rnd;
	int pass, t, i;
	MP_PLAYER* me = MpLocalPlayer();

	if (!jer_map_ready())
		return 0;

	rnd = gMp.seed ^ (unsigned int)MpNowMs() ^ (unsigned int)((me != NULL ? me->id + 3 : 3) * 2654435761u);

	for (pass = 0; pass < 2; pass++)
	{
		for (t = 0; t < MP_RESPAWN_TRIES; t++)
		{
			int ang, r, px, pz, x, z, dir = -1, region, clear = 1;
			long long dx, dz;

			rnd = rnd * 1103515245u + 12345u;
			ang = (int)((rnd >> 8) & 0xfff);
			r = MP_RESPAWN_MIN + (int)((rnd >> 16) % (unsigned int)(MP_RESPAWN_MAX - MP_RESPAWN_MIN));
			px = cx + (int)(((long)rsin(ang) * r) >> 12);
			pz = cz + (int)(((long)rcos(ang) * r) >> 12);

			region = jer_map_region_of(px, pz);
			if (!jer_map_region_has_data(region))
				continue;
			if (!jer_map_region_resident(region))
			{
				if (pass == 0)
					continue;
				/* aimed: a bare jer_map_spool_to streams whatever is under
				 * MainPlayer.spoolXZ (our wreck), not this candidate */
				if (!MpStreamRegionAt(px, pz))
					continue;
			}

			if (!MpFindRoadSpotNear(px, 0, pz, -1, &x, &z, &dir, NULL))
				continue;

			dx = (long long)x - cx;
			dz = (long long)z - cz;
			if (dx * dx + dz * dz < (long long)(MP_RESPAWN_MIN * 3 / 4) * (MP_RESPAWN_MIN * 3 / 4))
				continue;	/* the road search drifted back toward where we were */

			for (i = 0; i < MP_MAX_PLAYERS && clear; i++)
			{
				MP_PLAYER* o = MpGetPlayer(i);

				if (o != NULL && o->active && o != me && o->carId >= 0 && o->carId < MAX_CARS)
				{
					long long ox = (long long)car_data[o->carId].hd.where.t[0] - x;
					long long oz = (long long)car_data[o->carId].hd.where.t[2] - z;

					if (ox > -MP_RESPAWN_CLEAR && ox < MP_RESPAWN_CLEAR &&
						oz > -MP_RESPAWN_CLEAR && oz < MP_RESPAWN_CLEAR)
						clear = 0;
				}
			}
			if (!clear)
				continue;

			if (!MpWarpStreamTo(x, z))
				continue;

			/* A resident region can still answer 0 for a cell that is not in the
			 * barrel (the gather's measured note). For a DEATH respawn we can try
			 * another spot; landing on a void would just fall again. */
			{
				VECTOR gp;

				gp.vx = x;
				gp.vy = 0;
				gp.vz = z;
				gp.pad = 0;

				if (MapHeight(&gp) <= -1000)
					continue;
			}

			*outX = x;
			*outZ = z;
			*outDir = dir;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] respawn: road spot %d,%d in the %d-unit ring around %d,%d (pass %d, try %d)\n",
					x, z, r, cx, cz, pass, t);
			return 1;
		}
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] respawn: no clear road %d-%d units out from %d,%d - nearest road instead\n",
			MP_RESPAWN_MIN, MP_RESPAWN_MAX, cx, cz);
	return 0;
}

/* The road's heading at (x, z): its lane's when known; otherwise the axis,
 * turned to whichever of its two directions is closer to `want`. */
static int MpRoadHeadingNear(int x, int z, int want)
{
	int dir, how = MpRoadHeadingAt(x, 0, z, &dir);

	if (how == 0)
		return want;

	/* a lane heading is THE answer: the car drives with its lane's traffic */
	if (how == 2)
		return dir;

	if (((dir - want + 4096 + 1024) & 0xfff) >= 2048)
		dir = (dir + 2048) & 0xfff;

	return dir;
}

/* Put the joiner's car on a chosen spot, snap the camera with it, and log what
 * kind of spot it was. Returns 0 when there is no ground under it. */
static int MpPlaceSelfAt(CAR_DATA* cp, int x, int z, int dir, const char* how, int attempt)
{
	VECTOR gp;
	int ground;

	/* THE GROUND HAS TO BE REAL AT THE MOMENT OF PLACEMENT. An unstreamed cell
	 * answers with the sea/default plane -- a perfectly plausible height, which is
	 * how a car ends up standing in the air. JerRoadAt is the road map's own
	 * answer: if it no longer sees road here, refuse and let the caller try
	 * another spot or take the fallback. */
	if (!JerRoadAt(x, 0, z))
		return 0;

	gp.vx = x;
	gp.vy = 0;
	gp.vz = z;
	gp.pad = 0;
	ground = MapHeight(&gp);

	if (!MpPlaceOnGround(cp, x, z, (dir >= 0) ? dir : PlayerStartInfo[0]->rotation))
		return 0;

	cp->hd.speed = 0;
	player[0].cameraAngle = cp->hd.direction + 1536;

	/* snap the camera with the car instead of letting it swing across */
	player[0].pos[0] = x;
	player[0].pos[2] = z;
	InitCamera(&player[0]);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] live join: placed our car at %d,%d,%d dir %d, ground %d (%s, try %d, frame %d)\n",
			x, cp->hd.where.t[1], z, dir, ground, how, attempt, gMpPlaceSelfTry);

	return 1;
}

/* Stream the REGION under (x,z), without requiring the point itself to be a
 * landable cell. A region centre is only a stream target -- the road search
 * inside the region supplies the landing spot -- so it must not be put through
 * MpWarpSpotOk's CellEmpty/height gate. Returns 1 when the region has data and
 * is now resident. */
static int MpStreamRegionAt(int x, int z)
{
	static VECTOR sSpool;	/* spoolXZ must outlive the call */
	VECTOR* oldSpoolXZ;
	int ok;

	if (!jer_map_ready() || !jer_map_region_has_data(jer_map_region_of(x, z)))
		return 0;

	/* resident already: nothing to stream (and the resident spool pass stalls,
	 * see MpWarpStreamTo) */
	if (jer_map_region_resident(jer_map_region_of(x, z)))
		return 1;

	oldSpoolXZ = MainPlayer.spoolXZ;

	sSpool.vx = x;
	sSpool.vy = 0;
	sSpool.vz = z;
	sSpool.pad = 0;

	MainPlayer.spoolXZ = &sSpool;

	ok = jer_map_spool_to(x, z);

	MainPlayer.spoolXZ = oldSpoolXZ;

	return ok;
}

int MpFindRoadSpotWide(int carId, int* outX, int* outZ, int* outDir)
{
	unsigned int rnd;
	int regionWorld, margin, span, t;

	if (!jer_map_ready() || regions_across <= 0 || regions_down <= 0 ||
		units_across_halved <= 0 || units_down_halved <= 0)
		return 0;

	regionWorld = MAP_REGION_SIZE * MAP_CELL_SIZE;

	if (regionWorld <= 0)
		return 0;

	rnd = gMp.seed ^ (unsigned int)((gMpWideRegion + 1) * 2654435761u) ^ (unsigned int)MpNowMs();

	/* Choose and stream ONE region, once for this join. */
	if (gMpWideRegion < 0)
	{
		int startRegion = jer_map_region_of(PlayerStartInfo[0]->position.vx,
			PlayerStartInfo[0]->position.vz);
		int attempt;

		for (attempt = 0; attempt < 32; attempt++)
		{
			int r, rx, rz, cx, cz;

			rnd = rnd * 1103515245u + 12345u;

			r = (int)((rnd >> 8) % (unsigned int)(regions_across * regions_down));

			if (r == startRegion || !jer_map_region_has_data(r))
				continue;

			rx = r % regions_across;
			rz = r / regions_across;

			cx = rx * regionWorld - units_across_halved + regionWorld / 2;
			cz = rz * regionWorld - units_down_halved + regionWorld / 2;

			/* The ONLY spool this placement makes. A region with nothing to load
			 * is re-picked rather than landed in. The point is a stream target,
			 * not a landing spot, so it skips MpWarpSpotOk's cell test. */
			if (!MpStreamRegionAt(cx, cz))
				continue;

			gMpWideRegion = r;
			gMpWideTries = 0;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] live join: map-wide region %d chosen (center %d,%d)\n",
					r, cx, cz);
			break;
		}

		if (gMpWideRegion < 0)
			return 0;
	}

	{
		int rx = gMpWideRegion % regions_across;
		int rz = gMpWideRegion / regions_across;
		int x0 = rx * regionWorld - units_across_halved;
		int z0 = rz * regionWorld - units_down_halved;

		/* Keep each center's 7.2k road-probe ring inside the resident region. A
		 * region too small for that is probed from its middle; MpWarpSpotOk then
		 * refuses any ring point that fell into a neighbour. */
		margin = 12000;

		if (regionWorld < 2 * margin + 1000)
			margin = 0;

		span = regionWorld - 2 * margin;

		if (span < 1)
			span = 1;

		for (t = gMpWideTries; t < MP_JOIN_WIDE_TRIES; t++)
		{
			int cx, cz, x, z, dir = -1;

			rnd = rnd * 1103515245u + 12345u;
			cx = x0 + margin + (int)((rnd >> 8) % (unsigned int)span);
			rnd = rnd * 1103515245u + 12345u;
			cz = z0 + margin + (int)((rnd >> 8) % (unsigned int)span);

			gMpWideTries++;

			if (!MpFindRoadSpotNear(cx, 0, cz, gMpWideRegion + t * 97, &x, &z, &dir, NULL))
				continue;

			if (!MpWarpSpotOk(x, z))
				continue;

			if (!MpJoinSpotClearOfPlayers(x, z, -1, carId))
				continue;

			if (!MpWarpStreamTo(x, z))	/* confirm the final spot's area */
				continue;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] live join: map-wide road %d,%d dir %d in region %d (clear by %d, center %d,%d)\n",
					x, z, dir, gMpWideRegion, MP_JOIN_CLEAR, cx, cz);

			*outX = x;
			*outZ = z;
			*outDir = dir;
			return 1;
		}
	}

	return 0;
}

/* The last resort before the level start: stream the start region once (the
 * probes may never have had its road data resident), then ask the near-start
 * road search again. */
static int MpPlaceSelfFallback(CAR_DATA* cp, int carId, int seed)
{
	int sx = PlayerStartInfo[0]->position.vx;
	int sz = PlayerStartInfo[0]->position.vz;
	int fx = 0, fz = 0, fdir = -1;

	if (MpWarpStreamTo(sx, sz) &&
		MpFindRoadSpotNear(sx, 0, sz, seed, &fx, &fz, &fdir, NULL) &&
		MpJoinSpotClearOfPlayers(fx, fz, -1, carId) &&
		MpPlaceSelfAt(cp, fx, fz, fdir, "near-start fallback", 0))
		return 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] live join: near-start fallback found no clear road - keeping the level start\n");

	return 0;
}

void MpPlaceSelfTick(void)
{
	MP_PLAYER* me;
	CAR_DATA* cp;
	unsigned int rnd;
	int k, i, carId;

	if (gMpPlaceSelfFrames <= 0)
		return;

	if (!gMp.running || gMp.role != MP_ROLE_CLIENT)
	{
		if (!gMp.running && gMp.role == MP_ROLE_NONE)
			gMpPlaceSelfFrames = 0;
		return;
	}

	me = MpLocalPlayer();

	if (me == NULL || PlayerStartInfo[0] == NULL)
		return;

	/* THE FIRST FRAME THE CAR EXISTS. Our roster row adopts the engine's car in
	 * MpFollowLocalCar, which runs later in this same tick (MpSendOwnCarState), so
	 * waiting for me->carId would delay placement by a frame -- and the frame it
	 * delays is the one whose state the peers receive. The engine's own slot is
	 * the car the level just built for us, so use it now. */
	carId = (me->carId >= 0 && me->carId < MAX_CARS) ? me->carId : (int)player[0].playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	/* Only a car that is STILL ON THE LEVEL START is a fresh joiner's to move. One that
	 * has been driven away belongs to a player already in the match: never relocate it
	 * (or snap its camera), whatever armed this. */
	{
		int sdx = car_data[carId].hd.where.t[0] - PlayerStartInfo[0]->position.vx;
		int sdz = car_data[carId].hd.where.t[2] - PlayerStartInfo[0]->position.vz;

		if (sdx < -3000 || sdx > 3000 || sdz < -3000 || sdz > 3000)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] live join: our car is already %d,%d from the level start - not placing it\n", sdx, sdz);
			gMpPlaceSelfFrames = 0;
			return;
		}
	}

	/* HOLD THE CAR STILL WHILE WE SORT THE SPAWN. The engine's initial pose can be
	 * over ground this machine has not streamed, and a car that falls or rolls
	 * while the road search runs is one the driver sees move before it teleports.
	 * Pin x/y/z and the heading, and zero the speed, on every frame spent looking;
	 * the placement below writes the final pose. */
	{
		CAR_DATA* hold = &car_data[carId];
		MATRIX m;

		if (gMpPlaceSelfTry == 0)
			gMpPlaceSelfHoldY = hold->hd.where.t[1];

		hold->hd.speed = 0;
		hold->hd.where.t[0] = PlayerStartInfo[0]->position.vx;
		hold->hd.where.t[1] = gMpPlaceSelfHoldY;
		hold->hd.where.t[2] = PlayerStartInfo[0]->position.vz;
		hold->hd.direction = PlayerStartInfo[0]->rotation;

		_RotMatrixY(&m, (short)hold->hd.direction);
		memcpy(hold->hd.where.m, m.m, sizeof(hold->hd.where.m));
	}

	/* gMpPlaceSelfFrames: >1 = settle countdown, then 1..-TRIES = try each frame */
	if (gMpPlaceSelfFrames > 1)
	{
		gMpPlaceSelfFrames--;
		return;
	}

	gMpPlaceSelfTry++;

	if (gMpPlaceSelfTry == 1 && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] place-self reason: live join welcome (armed once); last rx %s conn %d frame %lu\n",
			gMpLastRxTag, gMpLastRxConn, gMpLastRxFrame);

	cp = &car_data[carId];

	/* FULL-CITY TAKE A RIDE (gBootMpLevel == 0): a random road ANYWHERE on the
	 * map, not the near-start ring. One region is streamed for this join; if it
	 * has no road for us, the near-start fallback follows rather than thrashing
	 * the streamer through region after region. */
	if (!gBootMpLevel)
	{
		int x = 0, z = 0, dir = -1;

		if (MpFindRoadSpotWide(carId, &x, &z, &dir) &&
			MpPlaceSelfAt(cp, x, z, dir, "map-wide", gMpWideTries))
		{
			gMpPlaceSelfFrames = 0;
			return;
		}

		/* No region could even be streamed yet: the level is still settling, so
		 * retry on the next tick instead of falling back on the first frame. */
		if (gMpWideRegion < 0 && gMpPlaceSelfTry < 8)
			return;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] live join: no map-wide road (region %d, %d center(s) tried) - near-start fallback\n",
				gMpWideRegion, gMpWideTries);

		(void)MpPlaceSelfFallback(cp, carId, me->id + 8192);
		gMpPlaceSelfFrames = 0;
		return;
	}

	/* SMALL ARENA: the near-start ring, clear of the others, is the right answer. */
	rnd = gMp.seed ^ (unsigned int)((me->id + 7) * 2654435761u) ^ (unsigned int)MpNowMs();

	for (k = 0; k < 8; k++)
	{
		int ang, r, cx, cz, x, z, dir = -1, clear = 1;

		rnd = rnd * 1103515245u + 12345u;
		ang = (int)((rnd >> 8) & 0xfff);
		r = 1500 + (int)((rnd >> 20) % 3000u);
		cx = PlayerStartInfo[0]->position.vx + (int)(((long)rsin(ang) * r) >> 12);
		cz = PlayerStartInfo[0]->position.vz + (int)(((long)rcos(ang) * r) >> 12);

		if (!MpFindRoadSpotNear(cx, 0, cz, me->id + k * 64, &x, &z, &dir, NULL) || !MpWarpSpotOk(x, z))
			continue;

		for (i = 0; i < MP_MAX_PLAYERS && clear; i++)
		{
			MP_PLAYER* o = MpGetPlayer(i);

			if (o != NULL && o->active && o != me && o->carId >= 0 && o->carId < MAX_CARS)
			{
				int dx = car_data[o->carId].hd.where.t[0] - x;
				int dz = car_data[o->carId].hd.where.t[2] - z;

				if (dx > -MP_JOIN_CLEAR && dx < MP_JOIN_CLEAR &&
					dz > -MP_JOIN_CLEAR && dz < MP_JOIN_CLEAR)
					clear = 0;
			}
		}

		if (!clear)
			continue;

		if (!MpPlaceSelfAt(cp, x, z, dir, "near-start", k))
			continue;

		gMpPlaceSelfFrames = 0;
		return;
	}

	if (gMpPlaceSelfTry < MP_PLACE_SELF_TRIES)
		return;		/* road data may still be streaming: try again next frame */

	/* LAST RESORT, BEFORE THE LEVEL START: stream the start region once and ask
	 * the near-start search again. Leaving the car on the level start is the final
	 * answer, not the first. */
	(void)MpPlaceSelfFallback(cp, carId, me->id + 4096);

	gMpPlaceSelfFrames = 0;
}

/* Put OUR on-foot pedestrian down on the chosen spot.
 *
 * The engine owns the y on foot: AnimatePed rewrites position.vy from MapHeight
 * every frame (standing height -130 for Tanner), so it is set the same way here
 * rather than left at whatever the last frame's value was. player[0].pos is what
 * the camera and the streamer read, and AnimatePed would only refresh it on the
 * next sim frame -- too late for a camera that is still over the old place. */
static int MpPlaceOwnPedAt(LPPEDESTRIAN ped, int x, int z, int dir)
{
	VECTOR gp;
	int ground;

	if (ped == NULL)
		return 0;

	gp.vx = x;
	gp.vy = 0;
	gp.vz = z;
	gp.pad = 0;

	ground = MapHeight(&gp);

	ped->position.vx = x;
	ped->position.vz = z;
	ped->position.vy = -130 - ground;	/* pedest.c's Tanner standing height */
	ped->dir.vy = (short)(dir & 0xFFF);
	ped->speed = 0;

	player[0].pos[0] = ped->position.vx;
	player[0].pos[1] = -ped->position.vy;
	player[0].pos[2] = ped->position.vz;
	player[0].cameraAngle = (dir & 0xFFF) + 1536;

	InitCamera(&player[0]);

	return 1;
}

/* Warp to another player, from a car OR on foot. The spot is the same either way:
 * beside/behind the target, on resident ground. What lands there is whatever this
 * machine is currently controlling -- the car if we are driving, our own
 * pedestrian if we are not. Being warped TO works on foot too: the target's pose
 * comes from the owner's MP_TAG_PED packets (tp->pedX/Y/Z below), so a peer does
 * not need a car to be reachable. */
int MpWarpToPlayer(int playerId)
{
	MP_PLAYER* me;
	MP_PLAYER* tp;
	CAR_DATA* myc = NULL;
	CAR_DATA* tgt;
	LPPEDESTRIAN myped = NULL;
	int onFoot;
	int tx, ty, tz, dir, fx, fz, rx, rz, i;
	int bestX = 0, bestZ = 0, bestRoad = 0, found = 0;

	if (!gMp.running)
		return 0;

	me = MpLocalPlayer();
	tp = MpGetPlayer(playerId);

	if (me == NULL)
	{
		MpNotify("Warp: no local player");
		return 0;
	}

	if (tp == NULL || tp->id == gMp.localPlayerId)
	{
		MpNotify("Warp: that player is not in the match");
		return 0;
	}

	onFoot = (me->carId < 0 || me->carId >= MAX_CARS);

	if (onFoot)
	{
		myped = MpLocalPed();

		if (myped == NULL)
		{
			MpNotify("Warp: no on-foot character to move");
			return 0;
		}
	}
	else
	{
		myc = &car_data[me->carId];
	}

	if (tp->carId >= 0 && tp->carId < MAX_CARS)
	{
		tgt = &car_data[tp->carId];
		tx = tgt->hd.where.t[0];
		ty = tgt->hd.where.t[1];
		tz = tgt->hd.where.t[2];
		dir = tgt->hd.direction;
	}
	else if (tp->pedLastMs != 0 && MpNowMs() - tp->pedLastMs < MP_WARP_PED_FRESH_MS)
	{
		/* ON FOOT: a player out of their car has no car_data slot, only the pose
		 * their owner keeps sending (MP_TAG_PED -> MpHandlePedState). Land beside
		 * that, facing their way (ped heading is the same 0..4095 yaw). This used
		 * to refuse with "that player has no car to go to". */
		tgt = NULL;
		tx = tp->pedX;
		ty = tp->pedY;
		tz = tp->pedZ;
		dir = tp->pedHeading & 0xFFF;
	}
	else
	{
		MpNotify("Warp: no position for that player yet");
		return 0;
	}
	(void)tgt;

	/* Aim the streamer at the TARGET first, so the region its neighbours fall in
	 * has a chance of being present before the candidates are judged. Harmless
	 * when the spot the target is standing on is not landable. */
	MpWarpStreamTo(tx, tz);

	/* The target's axes: forward (fx,fz) and a perpendicular (rx,rz). The offsets
	 * are in those, so "behind" stays behind whichever way they are pointing
	 * when the menu is used. */
	fx = (int)rsin(dir);
	fz = (int)rcos(dir);
	rx = fz;
	rz = -fx;

	for (i = 0; i < (int)(sizeof(kWarpAlong) / sizeof(kWarpAlong[0])); i++)
	{
		int k = (i + me->id) % (int)(sizeof(kWarpAlong) / sizeof(kWarpAlong[0]));	/* two warpers never take the same spot */
		int px = tx + ((fx * kWarpAlong[k] + rx * kWarpAcross[k]) >> 12);
		int pz = tz + ((fz * kWarpAlong[k] + rz * kWarpAcross[k]) >> 12);
		int onRoad;

		/* Test the GROUND this candidate stands on, not the target's: a region
		 * that is not resident is not a landing site, however good its coordinates
		 * look. No spooling here - one destination gets streamed below, because
		 * forcing region after region evicts the ones just loaded (four barrels). */
		if (!MpWarpSpotOk(px, pz))
			continue;

		onRoad = JerRoadAt(px, ty, pz) ? 1 : 0;

		/* A clear road point beats anything already found, and anything is better
		 * than nothing: this is also the "nearest road node to where we meant to
		 * go" fallback, since the ring starts beside the target and rings out. */
		if (!found || (onRoad && !bestRoad))
		{
			bestX = px;
			bestZ = pz;
			bestRoad = onRoad;
			found = 1;
		}

		if (bestRoad)
			break;
	}

	if (!found)
	{
		MpNotify("Warp: no ground there to land on");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] warp: nothing to land on near player %d (%s) at %d,%d "
				"(region %d data=%d resident=%d) - staying put\n",
				tp->id, tp->name, tx, tz, jer_map_region_of(tx, tz),
				jer_map_region_has_data(jer_map_region_of(tx, tz)),
				jer_map_region_resident(jer_map_region_of(tx, tz)));

		return 0;
	}

	/* Make the CHOSEN spot real, once, and confirm it: the single stream, and the
	 * answer afterwards is what says whether we may land. */
	if (!MpWarpStreamTo(bestX, bestZ))
	{
		MpNotify("Warp: could not stream the ground there");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] warp: chosen spot %d,%d (region %d) still not resident after "
				"spooling - REFUSING rather than landing in the void\n",
				bestX, bestZ, jer_map_region_of(bestX, bestZ));

		return 0;
	}

	if (onFoot)
	{
		/* ON FOOT: move our own pedestrian, not a car. The same landing spot,
		 * the same heading as the target. The pose goes out immediately rather
		 * than waiting for the next periodic send, so the teleport is one jump on
		 * every peer instead of a walk between the old and new positions. */
		if (!MpPlaceOwnPedAt(myped, bestX, bestZ, dir))
		{
			MpNotify("Warp: no ground at that spot");
			return 0;
		}

		MpSendOwnPedState();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] warp (on foot): to player %d (%s) at %d,%d,%d - %d units along "
				"their axis (negative = behind), %s\n",
				tp->id, tp->name, myped->position.vx, -myped->position.vy,
				myped->position.vz,
				(int)(((long)fx * (bestX - tx) + (long)fz * (bestZ - tz)) >> 12),
				bestRoad ? "on a road" : "off-road");
	}
	else
	{
		JER_ARGS_RESET_CAR rc;

		if (!MpPlaceOnGround(myc, bestX, bestZ, dir))
		{
			MpNotify("Warp: no ground at that spot");

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] warp: spot %d,%d has no ground under it - not moving\n",
					bestX, bestZ);

			return 0;
		}

		/* Arrive stopped, and pointing the way the target is - not pointing AT
		 * the target: the heading that matters is the one the driver can see out
		 * of, and a car dropped in at an arbitrary angle is one they have to sort
		 * out before they can drive. */
		myc->hd.speed = 0;
		myc->wheel_angle = 0;

		/* Re-seated because cameraAngle is an absolute world yaw, not an offset. */
		player[0].cameraAngle = myc->hd.direction + 1536;
		player[0].cameraCarId = myc->id;

		rc.carId = me->carId;
		jer_fire(JER_EVENT_RESET_CAR, &rc);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] warp: to player %d (%s) at %d,%d - %d units along their axis (negative = behind), %s\n",
				tp->id, tp->name, myc->hd.where.t[0], myc->hd.where.t[2],
				(int)(((long)fx * (bestX - tx) + (long)fz * (bestZ - tz)) >> 12),
				bestRoad ? "on a road" : "off-road");
	}

	return 1;
}

int MpRespawnOnRoad(void)
{
	MP_PLAYER* me;
	CAR_DATA* cp;
	int cx, cy, cz;
	int bestX = 0, bestZ = 0, found = 0, bestRing = 0, bestDir = -1;
	int slot;

	me = MpLocalPlayer();

	/* v24: out of the round, or the delayed respawn is coming */
	if (MpRespawnWaiting())
	{
		MpNotify(gMpRespawnOut ? "Respawn: out until the next round" : "Respawn: on its way");
		return 0;
	}

	/* The mode holds the respawn (the round is over or counting down): the
	 * countdown's own respawn places everyone. */
	if (MpRespawnHeldNow())
	{
		MpNotify("Respawn: not until the next round starts");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] respawn (pause menu / Restart row / test) refused: mode %d holds respawn in phase %d\n",
				gMp.gamemode, MpMarkPhase());
		return 0;
	}

	/* ON FOOT: back in the parked car, else the nearest road. */
	if (player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
	{
		if (MpRespawnOnFoot(me))
			return 1;

		MpNotify("Respawn: no parked car or road near enough");
		return 0;
	}

	slot = MpRespawnCarSlot(me);

	if (slot < 0)
	{
		MpNotify("Respawn: no car to respawn");
		return 0;
	}

	cp = &car_data[slot];

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] respawn reason: Respawn (pause menu / Restart row / test) - slot %d damage %d felony %d; last rx %s conn %d frame %lu (now %lu)\n",
			slot, cp->totalDamage, (int)cp->felonyRating, gMpLastRxTag, gMpLastRxConn, gMpLastRxFrame, gMp.frame);

	cx = cp->hd.where.t[0];
	cy = cp->hd.where.t[1];
	cz = cp->hd.where.t[2];

	/* THE NEAREST ROAD, not the death-respawn ring.
	 *
	 * MpFindRespawnSpot deliberately picks a spot 12-30k units away: a DEATH
	 * should not put you back on the road you crashed on. The pause row is the
	 * other question ("I am wedged" / "I fell in the void"), and using the death
	 * search here teleported the car 21k units to somewhere the ground may not
	 * have streamed - the "Respawn, then keep falling" loop. */
	found = MpFindRoadSpotNear(cx, cy, cz, -1, &bestX, &bestZ, &bestDir, &bestRing);

	if (!found)
	{
		MpNotify("Respawn: no road near enough to put you on");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] respawn: no road within %d units of %d,%d (region %d data=%d) "
				"- leaving the car where it is\n",
				MP_ROAD_RINGS * MP_ROAD_RING_STEP, cx, cz,
				jer_map_region_of(cx, cz), jer_map_region_has_data(jer_map_region_of(cx, cz)));

		return 0;
	}

	/* Make the chosen road point real, and confirm it - the same gate, and the same
	 * reason, as the warp: MapHeight answers 0 for ground that is not streamed, and
	 * 0 is also a perfectly good height. */
	if (!MpWarpStreamTo(bestX, bestZ))
	{
		MpNotify("Respawn: could not stream the road there");

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] respawn: road point %d,%d (region %d) still not resident after "
				"spooling - REFUSING rather than respawning into the void\n",
				bestX, bestZ, jer_map_region_of(bestX, bestZ));

		return 0;
	}

	{
		JER_ARGS_RESET_CAR rc;
		int placeDir = (bestDir >= 0) ? bestDir : MpRoadHeadingNear(bestX, bestZ, cp->hd.direction & 0xfff);

		/* InitCarPhysics builds an UPRIGHT orientation out of the heading, and
		 * MpPlaceOnGround resolves the y from the road there. y=0 was the old
		 * shortcut and it put the car below a road at y=2048: it fell, the engine
		 * called it a death, and the respawn dropped it again. */
		if (!MpPlaceOnGround(cp, bestX, bestZ, placeDir))
		{
			MpNotify("Respawn: no ground on that road");

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] respawn: road point %d,%d has no ground under it - staying put\n",
					bestX, bestZ);

			return 0;
		}

		cp->hd.speed = 0;
		cp->wheel_angle = 0;

		/* the other half: repaired. The same fields the Repair row clears. */
		cp->totalDamage = 0;
		memset(cp->ap.damage, 0, sizeof(cp->ap.damage));
		CreateDentableCar(cp);
		CarReapplyHubcapState(cp->id);
		cp->ap.needsDenting = 0;

		/* Respawn implies repair AND a clean slate: drop the wanted level so the
		 * cops we just escaped are not instantly back on us from the old rating.
		 * (Repair alone leaves the felony, which is the "I repaired but they are
		 * still chasing me" surprise.) */
		MpSelfClearFelony();

		player[0].cameraAngle = cp->hd.direction + 1536;
		player[0].cameraCarId = cp->id;

		rc.carId = slot;
		jer_fire(JER_EVENT_RESET_CAR, &rc);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] respawn: on the nearest road at %d,%d - ring %d, from (%d,%d) - repaired and upright\n",
				cp->hd.where.t[0], cp->hd.where.t[2], bestRing, cx, cz);
	}

	return 1;
}

/* A join during WAITING / COUNTDOWN / ENDED is not mid-round: the joiner picks a
 * car and is counted in the wait. Only a first snapshot that is already LIVE
 * (and any later one, if we never saw a pre-GO phase) arms mid-round placement.
 * Take a Ride has no pre-game and keeps the welcome-time liveJoin. */
void MpSessionAdoptLiveJoin(void)
{
	if (!MpModeHasPreGame(gMp.gamemode))
		return;

	if (!MpMarkHaveState())
	{
		gMp.liveJoin = 0;
		return;
	}

	if (MpMarkPhase() == MP_MARK_PHASE_LIVE)
	{
		if (!gMpSawPreLive && !gMp.liveJoin)
		{
			gMp.liveJoin = 1;
			if (!gMp.localPlaced)
			{
				gMpPlaceSelfTry = 0;
				gMpPlaceSelfFrames = MP_PLACE_SELF_SETTLE;
			}
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] live join: round is already LIVE - our car will be placed on a clear road\n");
		}
		return;
	}

	gMpSawPreLive = 1;
	gMp.liveJoin = 0;
}

static int MpExtraModel(int lvl, int which)
{
	return MP_CITY_EXTRA[lvl & 3][which];
}

/* JER_EVENT_CAR_DATA_SOURCE: fired once per level, before any CARMODEL_* file is
 * read, with the resident model table and a per-slot source city in hand. A match
 * uses it to ask the level for enough distinct cars to seat every player. */
int MpOnCarDataSource(void* userdata, void* args)
{
	JER_ARGS_CAR_DATA_SOURCE* a = (JER_ARGS_CAR_DATA_SOURCE*)args;
	int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
	int i;

	(void)userdata;

	gMpExtraSet = 0;

	for (i = 0; i < MP_EXTRA_SLOTS; i++)
		gMpExtraModel[i] = -1;

	if (a == NULL)
		return JER_RESULT_CONTINUE;

	/* MP_EXTRA_SLOTS="slot:model[,slot:model]" chooses the match's spare cars by hand
	 * -- and does it WITHOUT a live session, which is what made the table above
	 * measurable: one `-level <city>` boot per candidate says whether that level's
	 * own table has the model (or takes the game down trying it, which is an answer
	 * too). Inert unless set. */
	{
		const char* over = getenv("MP_EXTRA_SLOTS");
		const char* p = over;

		while (p != NULL && *p != '\0')
		{
			int slot = 0, model = 0;

			while (*p >= '0' && *p <= '9')
				slot = slot * 10 + (*p++ - '0');

			if (*p == ':')
				p++;

			while (*p >= '0' && *p <= '9')
				model = model * 10 + (*p++ - '0');

			if (slot >= 5 && slot < 5 + MP_EXTRA_SLOTS && model > 0)
				gMpExtraModel[slot - 5] = model;

			while (*p != '\0' && *p != ',')
				p++;

			if (*p == ',')
				p++;
		}
	}

	/* Only in a live session (or when asked by hand): a single-player level must not
	 * load cars nobody is going to drive. */
	if (!gMp.running && getenv("MP_EXTRA_SLOTS") == NULL)
		return JER_RESULT_CONTINUE;

	if (a->models == NULL || a->modelSource == NULL ||
	    a->count < 5 + MP_EXTRA_SLOTS)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MP_EXTRA_SLOTS; i++)
	{
		int slot = 5 + i;
		int model = (gMpExtraModel[i] > 0) ? gMpExtraModel[i] : MpExtraModel(lvl, i);

		/* A spare slot another module already claimed is ITS business -- keep what
		 * it put there and let the assignment name it. */
		if (a->models[slot] > 0)
		{
			gMpExtraModel[i] = a->models[slot];

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] resident slot %d already holds model %d (not ours) -- player %d will drive it\n",
					slot, a->models[slot], 5 + i);
			continue;
		}

		a->models[slot] = model;
		a->modelSource[slot] = -1;	/* the level's OWN table: the one source that works */
		gMpExtraModel[i] = model;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] resident slot %d = model %d (the level's own), so player %d has a car of their own\n",
				slot, model, 5 + i);
	}

	gMpExtraSet = 1;

	/* The local player's car was decided before the level loaded, when the spare
	 * slots were not set yet (the launch happens before LoadMission). If this player
	 * is one of the players those slots exist for, name the model that is ACTUALLY
	 * in the slot -- and only when they made no explicit pick, so -mpcar keeps the
	 * last word. */
	if (gMp.localPlayerId >= 5 && gMp.localPlayerId < 5 + MP_EXTRA_SLOTS &&
	    gMp.config.car < 0)
	{
		int i2 = gMp.localPlayerId - 5;

		if (gMpExtraModel[i2] > 0)
		{
			wantedCar[0] = gMpExtraModel[i2];

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] player %d: our own car is the spare slot's model %d\n",
					gMp.localPlayerId, wantedCar[0]);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* The car EVERY machine assigns a player who did not choose one (no -mpcar). It
 * must be the SAME number on every machine AND different per player, so the cars
 * are both distinguishable and agreed on. The level's own car table
 * (carNumLookup, indexed by player id) is exactly that: the same for every city,
 * and READABLE at level init -- unlike the resident models, which the engine has
 * not filled yet at this point (the diag prints residents= 0 0 0 0 ...).
 *
 * Indexed by PLAYER ID, with no modulo. `playerId % 4` wrapped ids 4..7 back onto
 * ids 0..3, so from the FIFTH player two players were handed the same car -- and
 * with the palette owner-authoritative the two machines then showed two cars
 * nobody could tell apart. Eight ids, eight distinct models:
 *
 *   ids 0..4  the level's five domestic cars (carNumLookup's own first five, which
 *             are the five models every level has resident)
 *   ids 5..6  the spare resident slots mp fills (see MpOnCarDataSource)
 *   id 7      the level's special (slot 7)
 *
 * Ids 0..3 keep the models they had before, so a pair behaves exactly as it did.
 *
 * The old code applied this to REMOTE players only and left the LOCAL player on
 * the level's default, so the machines disagreed about the local player's car
 * (the client saw the host as slot 0 with a palette that did not match, because
 * the owner's palette was applied to a different model), and two un-chosen
 * players could both come out as the same model if the level's default coincided
 * with one of the assigned ones. */
int MpAssignedCarModel(int playerId)
{
	extern char carNumLookup[CITY_COUNT][10];
	int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;

	if (playerId <= 0)
		return carNumLookup[lvl][0];		/* the host */

	if (playerId < 5)
		return carNumLookup[lvl][playerId];	/* the level's own five cars */

	if (playerId < 5 + MP_EXTRA_SLOTS)
	{
		int i = playerId - 5;

		if (gMpExtraSet && gMpExtraModel[i] > 0)
			return gMpExtraModel[i];	/* what the slot really holds */

		return MpExtraModel(lvl, i);
	}

	return MP_CITY_SPECIAL[lvl];			/* the 8th player: the special */
}

/* The city MpAssignedCarModel's model number belongs to: ALWAYS the session's
 * own -- that function indexes the level's car table. Paired with it so a caller
 * names both halves of an identity. */
int MpAssignedCarCity(void)
{
	return MP_CAR_CITY_SESSION;
}

/* Is this CAR_DATA slot OWNED by somebody? A join may only ever build a car in a
 * slot that nobody drives here and nobody has a claim on:
 *
 *   - the local engine car (slot 0 / player[0].playerCarId) and any
 *     CONTROL_TYPE_PLAYER slot;
 *   - any car an active roster row names (MpGetPlayerByCar is the same test seen
 *     through the registry);
 *   - ANY row's leftCarSlot1 -- the body a player got OUT of and is standing
 *     next to. That body is CIV_AI + the quiet pad, so every other test calls it
 *     free, and InitPlayer on it is exactly the "put back on a road, repaired,
 *     felony cleared" reset players report when someone joins. This test must
 *     not be limited to active rows: a roster can drop a row for one poll gap
 *     while the body still belongs to that player.
 *
 * `ignoreId` is the player being seated; their own row is not a conflict. */
int MpCarSlotOwnedByPlayer(int slot, int ignoreId)
{
	int i;

	if (slot < 0 || slot >= MAX_CARS)
		return 1;

	if (slot == 0 || slot == (int)player[0].playerCarId)
		return 1;

	if (car_data[slot].controlType == CONTROL_TYPE_PLAYER)
		return 1;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		const MP_PLAYER* p = &gMp.players[i];

		if (p->id == ignoreId)
			continue;

		if (p->active && p->carId == slot)
			return 1;

		if (p->leftCarSlot1 == slot + 1)
			return 1;
	}

	return 0;
}

/* Is (x,z) at least MP_JOIN_CLEAR from every OTHER player's car? `ignoreId` is
 * the player being seated and `ignoreSlot` the CAR_DATA being placed, if any
 * (a live joiner's own car is still on the level start while we choose its new
 * spot, and must not veto it). Slot -1 is the local engine car, checked
 * explicitly because the host-side spawn must not rely on a roster row naming
 * it. */
int MpJoinSpotClearOfPlayers(int x, int z, int ignoreId, int ignoreSlot)
{
	long long dx, dz;
	int i;

	for (i = -1; i < MP_MAX_PLAYERS; i++)
	{
		int slot;

		if (i < 0)
		{
			slot = (int)player[0].playerCarId;
		}
		else
		{
			const MP_PLAYER* o = &gMp.players[i];

			if (!o->active || o->id == ignoreId)
				continue;

			slot = o->carId;
		}

		if (slot < 0 || slot >= MAX_CARS || slot == ignoreSlot)
			continue;

		if (car_data[slot].controlType == CONTROL_TYPE_NONE)
			continue;		/* an engine-cleared slot is not a car */

		dx = (long long)car_data[slot].hd.where.t[0] - x;
		dz = (long long)car_data[slot].hd.where.t[2] - z;

		if (dx > -MP_JOIN_CLEAR && dx < MP_JOIN_CLEAR &&
			dz > -MP_JOIN_CLEAR && dz < MP_JOIN_CLEAR)
			return 0;
	}

	return 1;
}

/* JERICHO: is the LOCAL player's car a real choice yet? See mp.h. A joiner that has
 * not picked is driving whatever its own machine happened to have - measured: a client
 * advertises "CHICAGO model 0" while it is still on the title screen. carhacks must not
 * fold that in: it takes a resident slot nobody drives, and the real pick then finds
 * its canonical slot occupied by it. This is the same flag the roster publishes
 * (MP_ROSTER_FLAG_CAR_READY), and the host is always "chosen" - it is the authority for
 * its own car and never sends a pick message. */
int MpLocalCarChosen(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = MpGetPlayer(i);

		if (p != NULL && p->isLocal)
			return MpPlayerCarReady(p);
	}

	return 0;
}

/* Give a car to any player who has none, exactly the way the engine's own
 * player-creation loop does it. A peer that joins a match already in progress
 * gets no car from the engine at all -- and a player with no car is invisible
 * on every screen, has no input applied to anything, and cannot be hit. That is
 * the "no client shows up on the host" report.
 *
 * A slot is CLAIMED by whoever drives it, so the slot for a new car is the
 * smallest CAR_DATA slot no player is already driving -- NOT "how many rows have
 * one". Counting is only right while the cars fill a contiguous [0, n); once a
 * player leaves, its slot is free but the count points at an occupied one, and
 * two players end up in the same car. player[] is MAX_PLAYERS (16) entries, so
 * this is safe past slot 1. */
void MpSpawnLateJoiners(void)
{
	int id, slot, spawned = 0;

	/* THE LEVEL'S OWN START RECORDS MUST EXIST FIRST. The placement below copies
	 * player 0's record and both call sites can arrive while a level is still
	 * coming up -- a city change reloads one -- and PlayerStartInfo[0] is NULL
	 * until the engine builds it during the load (main.c:3485). Dereferencing it
	 * is the 0xC0000005 reported as "changing cities as a client joining the
	 * game": MpSpawnLateJoiners+0x188 in the JERICHO dump.
	 *
	 * Ask again next frame instead of giving up: the caller clears the request
	 * before calling this, so a deferred pass has to re-arm itself. */
	if (PlayerStartInfo[0] == NULL)
	{
		gMp.pendingSpawn = 1;
		return;
	}

	/* GAME-MODE DECISION POINTS. A late joiner is seated at once at the session's
	 * own placement below (SPAWN_SESSION: the level start / a per-player road spot,
	 * x/z only, the engine resolving the ground). v24 LATEJOIN_SPECTATE (the round
	 * modes) seats them the same way; the referee benches them WAITING until the next
	 * COUNTDOWN, and MpRoundHoldsCar / MpSpectateTick hold the car and watch the
	 * round. Mode-chosen spawns (arenas) still get the session placement and one log
	 * line. */
	{
		int rule = MpModeSpawnRule(gMp.gamemode);

		/* CLUSTER seats a joiner exactly as SESSION does; the cluster itself is a
		 * round-start placement (MpMarkClusterTick), not a seat */
		if (rule != MP_MODE_SPAWN_SESSION && rule != MP_MODE_SPAWN_CLUSTER)
			MpModeFallbackOnce(MP_MODE_FALLBACK_SPAWN, "spawn rule", rule);
	}

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		int rot, k;
		char padid;

		/* Only a player who has CHOSEN a car gets one -- see carConfirmed. And
		 * never one the owner has told us is ON FOOT: the request can also be
		 * armed by the roster, and re-seating a player who is walking about is
		 * what made the stand-in pedestrian flicker. */
		if (p == NULL || p->isLocal || p->carId >= 0 || p->onFoot || !MpPlayerCarReady(p))
			continue;

		/* NO PLACEHOLDER FOR A LIVE JOINER: wait until its owner has reported a
		 * pose. Building now would put the car on a near-start road (or the level
		 * start) until the first carstate corrects it -- the visible "spawns in the
		 * normal place, then teleports". A returning player already has a pose from
		 * earlier in the match, so this only delays a brand-new body. */
		if (gMp.running && !p->joinedPose)
			continue;

		/* The smallest CAR_DATA slot no player is already driving. Counting the
		 * rows was only correct while the cars occupied a contiguous [0, n): after
		 * a player LEAVES, its slot is free but the count still points at a slot
		 * that is taken. Slot 0 is the local player's, and the same scan skips it
		 * because the local row drives it. */
		/* A free PLAYER slot: no player row owns it, and it is not a car someone is
		 * driving on this machine. Slot 0 and our own driven car are never handed out:
		 * InitPlayer on them rebuilt OUR car as a fresh one on a road near the level
		 * start (repaired, felony 0) the moment a peer joined -- when our own row did
		 * not (yet, or any more) name the car we sit in. */
		slot = -1;
		for (k = 1; k < MAX_CARS && k < MAX_PLAYERS; k++)
		{
			/* a car somebody got out of (stopped, empty, quiet pad): only if
			 * nothing else is free - see the second pass */
			if (car_data[k].controlType == CONTROL_TYPE_CIV_AI && car_data[k].ai.padid == &gMpQuietPad)
				continue;

			if (MpCarSlotOwnedByPlayer(k, id))
				continue;

			slot = k;
			break;
		}

		for (k = 1; slot < 0 && k < MAX_CARS && k < MAX_PLAYERS; k++)
		{
			if (car_data[k].controlType == CONTROL_TYPE_CIV_AI && car_data[k].ai.padid == &gMpQuietPad &&
				!MpCarSlotOwnedByPlayer(k, id))
				slot = k;
		}

		if (slot < 1 || slot >= MAX_CARS || slot >= MAX_PLAYERS)
		{
			/* past the engine's own player table: InitPlayer below would write
			 * through the end of player[] (MAX_PLAYERS), so a player who has no
			 * slot in it cannot be seated -- say so rather than corrupt. */
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] spawn: no car slot for player %d (found %d, engine holds %d player rows)\n",
					id, slot, MAX_PLAYERS);
			continue;
		}

		/* THE LEVEL'S OWN START, one lane per player -- NOT a copy of player 0's record.
		 *
		 * The engine builds every player's start itself: slot 0 from the mission or
		 * levelstartpos, and slot 1 at +600 in x beside it (main.c:3401-3405), with
		 * ONLY x and z set (main.c:3384 leaves vy = 0) so that placement resolves the
		 * height later (main.c:641-642). A LIVE join has no engine record for this
		 * slot -- the level was built for one player and the start table was filled
		 * then -- so build the same shape here: the local record's start point, offset
		 * one 600-unit lane per player id, the level's own heading, and no y.
		 *
		 * Copying slot 0's whole record (what this did) put every late joiner on the
		 * MISSION START POINT OF PLAYER 0 -- the player who is already in the match --
		 * and the placement below then forced the local car's LIVE y on top of it. A y
		 * from one x/z applied at another is the "late joiners spawn above the host"
		 * report, and the engine has to pull the car down afterwards. */
		PlayerStartInfo[slot] = &ReplayStreams[slot].SourceType;

		memset((char*)PlayerStartInfo[slot], 0, sizeof(STREAM_SOURCE));

		PlayerStartInfo[slot]->type = 1;
		PlayerStartInfo[slot]->controlType = CONTROL_TYPE_PLAYER;
		PlayerStartInfo[slot]->flags = 0;

		{
			/* A RANDOM ROAD POINT near the level start, facing along the road.
			 *
			 * The engine's own step for player 2 is 600 units in +x (main.c), and
			 * following it for every id put player 15 at +9000 -- past the edge of a
			 * multiplayer ARENA, where placement resolves the ground to nothing and
			 * the car drops into the void. A grid fixed the void; it did not stop
			 * sixteen players appearing in one tidy lay-by. So use the same ring
			 * search the warp and the respawn already trust (MpFindRoadSpotNear),
			 * asking for a RANDOM road point on the nearest ring that has road, and
			 * face the car along the road it landed on: a car dropped in at an
			 * arbitrary angle is one the driver has to sort out before driving.
			 *
			 * The grid stays as the FALLBACK -- a join must never fail to place just
			 * because no road was in reach (an arena's start can be a car park). id 0
			 * is never here: the engine built its own record for the local player. */
			int col = 0, row = 0, dx = 0, dz = 0;
			int rx = 0, rz = 0, rdir = -1;
			int onRoad = 0;
			int baseX = PlayerStartInfo[0]->position.vx;
			int baseZ = PlayerStartInfo[0]->position.vz;

			/* AT THE POSE ITS OWNER ANNOUNCED. This is the live-joiner case: the
			 * first carstate arrived before this machine assigned a slot, so build
			 * the body where it actually is instead of at a near-start road and then
			 * moving it. The y stays the engine's (resolved from the ground under
			 * x/z, main.c's own convention); the owner's next sample carries its
			 * exact y, so a region this machine has not streamed cannot keep it in
			 * the air. */
			if (p->joinedPose)
			{
				PlayerStartInfo[slot]->position.vx = p->joinPoseX;
				PlayerStartInfo[slot]->position.vz = p->joinPoseZ;
				PlayerStartInfo[slot]->rotation = (u_short)(p->joinPoseHeading & 0xfff);
				onRoad = 2;		/* 2 = the owner's announced pose */
			}
			else if (id > 0)
			{
				int tries;

				/* The level-start region may not be resident any more: the host can
				 * have driven away while this joiner was loading. A search there sees
				 * no road, and InitPlayer then resolves vy over no ground -- the
				 * "joiner in the air" copy. Centre on the host's LIVE x/z instead;
				 * only the x/z is a search centre, never the car's height (that is
				 * still the engine's, from the ground under the chosen spot). */
				if (!jer_map_region_resident(jer_map_region_of(baseX, baseZ)) &&
					player[0].playerCarId >= 0 && player[0].playerCarId < MAX_CARS)
				{
					baseX = car_data[player[0].playerCarId].hd.where.t[0];
					baseZ = car_data[player[0].playerCarId].hd.where.t[2];

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] spawn: level start region %d is not resident - searching around the host at %d,%d\n",
							jer_map_region_of(PlayerStartInfo[0]->position.vx,
								PlayerStartInfo[0]->position.vz), baseX, baseZ);
				}

				/* No spooling here on purpose: MpWarpSpotOk already refuses a spot
				 * whose ground is not resident, and moving the streamer while a join
				 * is being placed is how players end up in the void. Near the level
				 * start the region is resident by definition.
				 *
				 * CLEAR OF EVERY OTHER CAR. The ring search stops at the first ring
				 * with a road, which can be a short hop from the host's own car --
				 * the "late joiner on top of the host" report. Retry around centres
				 * pushed one clearance-step out in all four directions, so the first
				 * ring the search can return is already clear. */
				for (tries = 0; tries < 4 && !onRoad; tries++)
				{
					int sx = 0, sz = 0, sdir = -1;
					int cx = baseX;
					int cz = baseZ;
					int step = MP_JOIN_CLEAR + 1200;

					if (tries == 0)
						cx += step;
					else if (tries == 1)
						cx -= step;
					else if (tries == 2)
						cz += step;
					else
						cz -= step;

					if (!MpFindRoadSpotNear(cx, 0, cz, p->id + tries * 64, &sx, &sz, &sdir, NULL))
						continue;

					if (!MpJoinSpotClearOfPlayers(sx, sz, id, -1))
						continue;

					rx = sx;
					rz = sz;
					rdir = sdir;
					onRoad = 1;
				}
			}

			if (onRoad == 1)
			{
				PlayerStartInfo[slot]->position.vx = rx;
				PlayerStartInfo[slot]->position.vz = rz;
				/* vy stays 0 -- the engine's convention, not an oversight: placement
				 * resolves the ground under the car. */
				PlayerStartInfo[slot]->rotation = (u_short)
					((rdir >= 0) ? rdir : PlayerStartInfo[0]->rotation);
			}
			else if (onRoad == 0)
			{
				int cell = id - 1;

				col = cell % 4;
				row = cell / 4;

				dx = col * 600 - 900;	/* across: -900, -300, +300, +900 */
				dz = (row + 1) * 600;	/* back:   +600, +1200, +1800, +2400 */

				PlayerStartInfo[slot]->position.vx = PlayerStartInfo[0]->position.vx + dx;
				PlayerStartInfo[slot]->position.vz = PlayerStartInfo[0]->position.vz + dz;
				PlayerStartInfo[slot]->rotation = PlayerStartInfo[0]->rotation;
			}
			/* onRoad == 2: the announced pose is already written above; it must
			 * NOT be replaced by the road/grid blocks (they would write rx/rz,
			 * which are unset on this path -- that was the origin spawn). */

			if (gMpCtx != NULL)
			{
				if (onRoad == 2)
					gMpCtx->jer_log(gMpCtx,
						"[mp] spawn: player %d -> slot %d at its announced pose %d,%d heading %d (no placeholder at the level start)\n",
						id, slot, PlayerStartInfo[slot]->position.vx,
						PlayerStartInfo[slot]->position.vz, PlayerStartInfo[slot]->rotation);
				else if (onRoad)
					gMpCtx->jer_log(gMpCtx,
						"[mp] spawn: player %d -> slot %d on a road at %d,%d facing %d (%s), clear of every other car by %d\n",
						id, slot, PlayerStartInfo[slot]->position.vx,
						PlayerStartInfo[slot]->position.vz, PlayerStartInfo[slot]->rotation,
						(rdir >= 0) ? "along the road" : "level heading, no road angle",
						MP_JOIN_CLEAR);
				else
					gMpCtx->jer_log(gMpCtx,
						"[mp] spawn: player %d -> slot %d at the level's own start %d,%d (+%d,%d grid %d,%d), no clear road in reach - grid fallback NOT checked against other cars, no y\n",
						id, slot, PlayerStartInfo[slot]->position.vx,
						PlayerStartInfo[slot]->position.vz, dx, dz, col, row);
			}
		}

		/* Same pool-checked per-player vehicle as the level-init spawn, via the
		 * shared helper (see MpSetStartCar). */
		{
			int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
			int cid = MpSetStartCar(slot, p);

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] late joiner: player %d -> slot %d model %d (city %d)\n",
					id, slot, cid, lvl);
		}

		/* The car keeps the height InitPlayer took from the record (0, the engine's own
		 * convention) and the engine's placement resolves the ground under it, exactly
		 * as it does for a car created at level init. There is deliberately NO y
		 * override from the local car: a y from one x/z applied at another is what put
		 * late joiners above the host and then dropped them in. */
		rot = PlayerStartInfo[slot]->rotation;

		if (MpRemoteTargetsLocalCar(slot, "spawn (InitPlayer)", id))
			continue;

		/* BELT AND BRACES, and the answer to the open reset report: the scan above
		 * already refuses owned slots, but a join must never reach InitPlayer on a
		 * body that belongs to someone even if a future scan regresses. Refuse and
		 * say which slot, so a log is conclusive about what was not touched. */
		if (MpCarSlotOwnedByPlayer(slot, id))
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] spawn: refused slot %d for player %d - car is owned (controlType %d, our car %d, named by a player row %d)\n",
					slot, id, (int)car_data[slot].controlType, (int)player[0].playerCarId,
					MpGetPlayerByCar(slot) != NULL ? 1 : 0);
			continue;
		}

		padid = (char)-slot;

		InitPlayer(&player[slot], &car_data[slot], CONTROL_TYPE_PLAYER, rot,
			(LONGVECTOR4*)&PlayerStartInfo[slot]->position,
			PlayerStartInfo[slot]->model, PlayerStartInfo[slot]->palette, &padid);

		/* the same field the engine sets for a player car it created itself */
		car_data[slot].ap.needsDenting = 1;

		/* InitPlayer resolves the start record, so place the car after it and
			 * rebuild the matrix (the collision box is built from the matrix) */
		car_data[slot].hd.where.t[0] = PlayerStartInfo[slot]->position.vx;
		car_data[slot].hd.where.t[2] = PlayerStartInfo[slot]->position.vz;
		car_data[slot].hd.direction = rot;

		if (MpDebugOn() && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] spawn: player %d placed at %d,%d,%d (y is the record's, not the local car's)\n",
				id, car_data[slot].hd.where.t[0], car_data[slot].hd.where.t[1],
				car_data[slot].hd.where.t[2]);
		{
			MATRIX m;

			_RotMatrixY(&m, (short)rot);
			memcpy(car_data[slot].hd.where.m, m.m, sizeof(car_data[slot].hd.where.m));
		}

		p->carId = slot;

		/* The body exists now: the next carstate should record a FRESH pose, so a
		 * later re-seat is not anchored to the pose this build used. */
		p->joinedPose = 0;

		/* CARS PERSIST. A player who got out left a quiet-pad body and a
		 * leftCarSlot1 claim. Giving them a new car does not delete that body:
		 * it stays parked and keeps the claim (the claim is overwritten on their
		 * next get-out). This is deliberate -- the old car is a world car another
		 * player can walk to, not a duplicate to clean up. The ownership guards
		 * (MpCarSlotTakenByOther and the SAME_BODY gate) keep another row from
		 * deleting or reclaiming it. */
		spawned++;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] player %d had no car on this machine - given the free slot %d\n", id, slot);
	}

	if (spawned > 0)
	{
		/* tell everyone, so the clients build the car too instead of waiting for
		 * a snapshot to paste a transform onto whatever is in that slot */
		MpHostSendRoster();
	}
}

/* Every peer the last roster named no car for is ON FOOT (A1): called once the
 * match is known to be running -- the live-join welcome, or a dedicated server's
 * START (MpHandleStart). `self` is our own id, which the pre-welcome roster
 * adopted before we knew it was us. */
void MpMarkRosterNoCarOnFoot(int self)
{
	int k;

	for (k = 0; k < MP_MAX_PLAYERS; k++)
	{
		MP_PLAYER* o = MpGetPlayer(k);

		if (o != NULL && k != self && !o->isLocal && o->rosterNoCar)
			o->onFoot = 1;
	}
}

/* JER_EVENT_NET_SPAWN: create one car per remote player. Slots past the local
 * players get negative pad ids in the spawn loop; the module keeps them placed
 * from the network each frame. */
int MpOnNetSpawn(void* userdata, void* args)
{
	JER_ARGS_NET_SPAWN* sp = (JER_ARGS_NET_SPAWN*)args;
	int slot, i;

	(void)userdata;

	sp->added = 0;

	if (!MpIsActive() || gMp.playerCount <= 1)
		return JER_RESULT_CONTINUE;

	/* GAME-MODE DECISION POINT: where the level-load cars go. Take a Ride uses the
	 * session's own line-up below (the level start record, +900 x per slot, y left to
	 * the engine). Any other rule falls back to it until arenas exist. */
	{
		int rule = MpModeSpawnRule(gMp.gamemode);

		/* CLUSTER: the level-load line-up is SESSION's; the round start moves
		 * everyone into the cluster later (MpMarkClusterTick) */
		if (rule != MP_MODE_SPAWN_SESSION && rule != MP_MODE_SPAWN_CLUSTER)
			MpModeFallbackOnce(MP_MODE_FALLBACK_SPAWN, "spawn rule", rule);
	}

	slot = sp->numPlayers;

	/* ASCENDING PLAYER ID, not registry-row order. Rows are handed out
	 * first-free, so walking them lets two machines put the same two players in
	 * opposite slots -- and then each drives the other's car, or reads a level
	 * AI car as a player. Player ids are the one ordering both sides agree on. */
	for (i = 0; i < MP_MAX_PLAYERS && slot < sp->maxPlayers; i++)
	{
		MP_PLAYER* p = MpGetPlayer(i);

		/* Only a player who has CHOSEN a car gets one built here. A peer whose pick has
		 * not arrived is left with NO car on purpose: their vehicle must not appear on
		 * every screen before they have picked it in the car select. MpHandleCar builds
		 * it the moment the pick lands. */
		if (p == NULL || p->isLocal || !MpPlayerCarReady(p))
			continue;

		/* ...and nobody who is ON FOOT (MpSpawnLateJoiners skips them too): a car
		 * built here is claimed as their left car by the NO_CAR row, so the real
		 * parked body is never materialized at its LEFT_CAR pose (#56). */
		if (p->onFoot)
			continue;

		PlayerStartInfo[slot] = &ReplayStreams[slot].SourceType;
		memcpy((u_char*)PlayerStartInfo[slot], (u_char*)PlayerStartInfo[0], sizeof(STREAM_SOURCE));

		PlayerStartInfo[slot]->type = 1;
		PlayerStartInfo[slot]->controlType = CONTROL_TYPE_PLAYER;
		PlayerStartInfo[slot]->flags = 0;

		/* POOL-CHECKED vehicle, via the shared helper (see MpSetStartCar). Only remote
		 * cars reach here -- the local player is skipped above. */
		{
			int lvl = (GameLevel >= 0 && GameLevel < 4) ? GameLevel : 0;
			int cid = MpSetStartCar(slot, p);

			if (gMpCtx != NULL)
			{
				VECTOR g, nrm, out;
				sdPlane* plane = NULL;

				g.vx = PlayerStartInfo[slot]->position.vx;
				g.vy = PlayerStartInfo[slot]->position.vy;
				g.vz = PlayerStartInfo[slot]->position.vz;
				FindSurfaceD2(&g, &nrm, &out, &plane);

				gMpCtx->jer_log(gMpCtx,
					"[mp] netspawn: player %d -> slot %d model %d (city %d, asked %d)\n",
					i, slot, cid, lvl, p->car);

				gMpCtx->jer_log(gMpCtx,
					"[mp] spawn y: slot %d vy=%d (local slot 0 vy=%d) surface y=%d\n",
					slot, PlayerStartInfo[slot]->position.vy,
					PlayerStartInfo[0]->position.vy, out.vy);
			}
		}

		/* ONLY x/z/rotation come from the level-start record. This is NOT a live
		 * car: the hook fires from InitGameVariables, which has just rebuilt slot
		 * 0's record from levelstartpos (main.c), and the engine's own spawn loop
		 * below resolves the ground from the record's x/z. The height stays the
		 * engine's convention (0, main.c), never a y read from a car -- a y taken
		 * at one x/z and applied at another is the "late joiner above the host"
		 * report this path used to cause. */
		PlayerStartInfo[slot]->position.vy = 0;
		PlayerStartInfo[slot]->position.vx = PlayerStartInfo[0]->position.vx + 900 * slot;
		PlayerStartInfo[slot]->position.vz = PlayerStartInfo[0]->position.vz;
		PlayerStartInfo[slot]->rotation = PlayerStartInfo[0]->rotation;

		p->carId = slot;	/* the CAR_DATA slot this player drives */
		slot++;
	}

	sp->added = slot - sp->numPlayers;

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] added %d remote player car(s)\n", sp->added);

	return JER_RESULT_CONTINUE;
}
