/*
 * mp_test.c -- split out of mp_session.c
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"

#define MP_TEST_CYCLE_MAX_CARS (MP_CAR_LIST_MAX * 4)

static const char* gTestLeaveStr;

static const char* gTestKickStr;

static const char* gTestOnFootStr;

static const char* gTestEnterLeftStr;

static const char* gTestCarChangeStr;

static const char* gTestCityChangeStr;

static const char* gTestCarCycleStr;

static const char* gTestFallOffStr;

static const char* gTestChatKeyStr;

static const char* gTestCarSelectStr;

static const char* gTestFrontendJoinStr;

void MpResolveTestLevers(void);
static const char* MpLeftCarRejectReason(const MP_PLAYER* p, int left);
static void MpTestShuffle(int (*items)[2], int count, int* seed);

/* Called from MpSessionReset: this module's state is per session, so
 * the next match starts from the same values the module loaded with. */
void MpTestReset(void)
{
	/* no session state of its own */
}

void MpResolveTestLevers(void)
{
	static int resolved = 0;

	if (resolved)
		return;

	resolved = 1;

	gHeartbeatStr = getenv("MP_HEARTBEAT");
	gTestLeaveStr = getenv("MP_TEST_LEAVE");
	gTestKickStr = getenv("MP_TEST_KICK");
	gTestOnFootStr = getenv("MP_TEST_ONFOOT");
	gTestEnterLeftStr = getenv("MP_TEST_ENTER_LEFT");
	gTestCarChangeStr = getenv("MP_TEST_CARCHANGE");
	gTestCityChangeStr = getenv("MP_TEST_CITYCHANGE");
	gTestCarCycleStr = getenv("MP_TEST_CARCYCLE");
	gTestFallOffStr = getenv("MP_TEST_FALLOFF");
	gTestChatKeyStr = getenv("MP_TEST_CHATKEY");
	gTestCarSelectStr = getenv("MP_TEST_CARSELECT");
	gTestFrontendJoinStr = getenv("MP_TEST_FRONTEND_JOIN");
}

/* MP_TEST_FRONTEND_JOIN: is a joining machine meant to KEEP the menus?
 *
 * Every other client lever (-join, MP_AUTOSTART=join) sets autoSession, i.e.
 * "no menus: launch as soon as we are in" -- so a client never opens the vehicle
 * select and a headless run could not answer "does a CLIENT reach the car screen
 * and its roster?". With this set the join keeps autoSession off, and the client
 * takes the route a human at the machine takes: WELCOME -> MpUiOpenCarSelect ->
 * screen 14 -> the menu -> Ride (which hands the launch back through
 * JER_EVENT_MP_FRONTEND).
 *
 * Set = any non-empty value other than "0", like the other levers. */
const char* MpTestFrontendJoin(void)
{
	MpResolveTestLevers();
	return gTestFrontendJoinStr;
}

/* MP_TEST_CHATKEY is read by the frame hook in mp.c, so it is exposed rather
 * than duplicated: one resolution point for every test lever. */
const char* MpTestChatKey(void)
{
	MpResolveTestLevers();
	return gTestChatKeyStr;
}

/* MP_TEST_CARSELECT, read once: see the launch lever in MpLaunchLocal. */
const char* MpTestCarSelect(void)
{
	MpResolveTestLevers();
	return gTestCarSelectStr;
}

/* MP_TEST_FELONY=<level> -- keep a WANTED LEVEL up, so the ENGINE spawns police
 * cars and they pursue (inert unless set).
 *
 * Why this exists: the siren path only runs for a cop that is actually chasing
 * somebody, and a cop car turns up in a pair run "after a while" if it turns up
 * at all -- which makes the one behaviour worth checking unreproducible. Raising
 * the felony is the engine's OWN trigger, so the cars that appear are the ones a
 * player would get (PURSUER_AI, on duty), not a module-spawned stand-in that
 * could pass while the real path stays broken. It is re-asserted every 30 frames
 * because the engine decays it once nobody is being chased. */
void MpTestFelonyTick(void)
{
	static int level = -2;
	static unsigned nextFrame = 0;
	MP_PLAYER* me = MpLocalPlayer();

	if (level == -2)
	{
		const char* e = getenv("MP_TEST_FELONY");
		level = (e != NULL) ? atoi(e) : -1;
	}

	if (level < 0 || !gMp.running || me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	if (nextFrame != 0 && gMp.frame < nextFrame)
		return;

	nextFrame = gMp.frame + 30;

	car_data[me->carId].felonyRating = (u_char)(level > 4 ? 4 : level);
	pedestrianFelony = car_data[me->carId].felonyRating;
}

/* MP_TEST_TRAFFIC_HIT=<secs> -- every N seconds, send ONE synthetic traffic-contact
 * push for the nearest PEER mirror, so the traffic-hit wire path can be exercised
 * without a physical collision (the bots avoid traffic, so a pair run never makes
 * one). The impulse is fixed and deliberately large so the owner's move is
 * unambiguous. A test lever; inert unless set. */
void MpTestTrafficHitTick(void)
{
	static int secs = -2;
	static unsigned nextFrame = 0;
	MP_PLAYER* me = MpLocalPlayer();
	int i, best = -1, owner;
	long long bestD = -1;
	MP_HIT h;

	if (secs == -2)
	{
		const char* e = getenv("MP_TEST_TRAFFIC_HIT");
		secs = (e != NULL) ? atoi(e) : -1;
	}

	if (secs <= 0 || !gMp.running || me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	if (nextFrame == 0)
		nextFrame = gMp.frame + 30;	/* let the match settle first */

	if (gMp.frame < nextFrame)
		return;

	nextFrame = gMp.frame + (unsigned)secs * 30;

	for (i = 0; i < MAX_CARS; i++)
	{
		long long d;

		if (!gMpPeerTraffic[i])
			continue;

		d = MpDistSq64(car_data[i].hd.where.t[0] - car_data[me->carId].hd.where.t[0],
			       car_data[i].hd.where.t[2] - car_data[me->carId].hd.where.t[2]);

		if (bestD < 0 || d < bestD)
		{
			bestD = d;
			best = i;
		}
	}

	if (best < 0)
		return;

	owner = MpTrafficSlotOwnerRank(best);

	if (owner < 0)
		return;

	memset(&h, 0, sizeof(h));
	h.flags = MP_HIT_F_TRAFFIC;
	h.targetId = (uint8_t)best;
	h.impulse[2] = 3000;	/* a shove along +z, big enough to see */

	if (MpIsHost())
	{
		int ci = MpConnFindByPlayer(owner);

		if (ci >= 0)
			MpSendConn(ci, MP_TAG_HIT, 0, &h, sizeof(h));
	}
	else
		MpSendToHost(MP_TAG_HIT, 0, &h, sizeof(h));

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] TEST traffic hit: slot %d -> owner %d (impulse %ld,%ld,%ld)\n",
			best, owner, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
}

/* ------------------------------------------------------------------ */
/* Owner-authoritative car replication                                 */
/*                                                                     */
/* Each machine sends the ONE car it owns, every frame. Its owner is   */
/* the truth for that car and every other machine adopts the state, so */
/* nobody is guessing where somebody else's car is from delayed input. */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* MP_TEST_CARCHANGE=<seconds> -- test lever for a MID-SESSION CAR CHANGE.
 *
 * N seconds into a live match, get out of our car and into the nearest civilian
 * one, which is exactly what a player does in Take a Ride. It calls the ENGINE's
 * own ChangePedPlayerToCar -- the function the ped mechanic calls -- so the path
 * under test is the real one and the mod does not poke player[] itself.
 * Inert unless the env var is set. */
/* MP_TEST_LEAVE=<secs> -- leave the session cleanly that many seconds after the
 * match starts, then drop to the frontend, exactly as the pause menu's Exit
 * does. The point is the CONTRAST: a deliberate quit must read completely
 * differently in the log from a connection that died, on BOTH machines, and
 * that is not something a human can be asked to reproduce on demand.
 *
 * Reads the env into a variable rather than testing getenv() inline, like the
 * other test levers, so it cannot look like a debug switch to
 * tools/check_debug_independence.py. */
void MpTestLeaveTick(void)
{
	static int done;
	static unsigned long startMs;
	const char* s;
	unsigned long now = MpNowMs();

	if (done || !gMp.running)
		return;

	/* Client only: the interesting case is the HOST watching a client say
	 * goodbye, and running it on both machines at once would make the two logs
	 * ambiguous about who actually left. */
	if (gMp.role != MP_ROLE_CLIENT)
		return;

	s = gTestLeaveStr;

	if (s == NULL)
		return;

	if (startMs == 0)
		startMs = now;

	if ((now - startMs) < (unsigned long)(atoi(s) * 1000))
		return;

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: leaving the session cleanly %ss in (MP_TEST_LEAVE)\n", s);

	MpLeaveSession();
	MpReturnToFrontend();
}

/* MP_TEST_KICK=<secs> -- the HOST kicks its first client that many seconds in,
 * through the same MpKickPlayer the pause menu will call, so the wire path and
 * the kicked client's teardown are checkable from the log. Host only: running
 * it on both machines would make the two logs ambiguous. */
void MpTestKickTick(void)
{
	static int done;
	static unsigned long startMs;
	const char* s;
	unsigned long now = MpNowMs();

	if (done || !gMp.running)
		return;

	if (gMp.role != MP_ROLE_HOST)
		return;

	s = gTestKickStr;

	if (s == NULL)
		return;

	if (startMs == 0)
		startMs = now;

	if ((now - startMs) < (unsigned long)(atoi(s) * 1000))
		return;

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: host kicking player 1 %ss in (MP_TEST_KICK)\n", s);

	MpKickPlayer(1);
}

/* MP_TEST_ONFOOT=<secs> -- get the local player OUT of their car that many
 * seconds in, so the on-foot path can be exercised without a human at the wheel.
 *
 * Uses the engine's own way out (ChangeCarPlayerToPed), the same call the
 * car-change lever makes. The car we leave stays exactly where it is: that is the
 * designed behaviour (see MpReleaseRemoteCar -- handing a module-made car to the
 * traffic AI is a crash, not a feature).
 *
 * Separate from MP_TEST_CARCHANGE on purpose: that lever has to find a civilian
 * car of a different model before it will do anything, which makes it useless as
 * a way to test THIS. */
void MpTestOnFootTick(void)
{
	static int done;
	static int noted;
	static unsigned long startMs;
	const char* s;
	unsigned long now = MpNowMs();
	int carId = -1;

	if (done)
		return;

	s = gTestOnFootStr;

	if (s == NULL)
		return;

	/* Say ONCE that the lever was seen. Without it, a run that does not fire is
	 * ambiguous: never called, called before the match, or called and not yet due
	 * all look identical in the log. */
	if (!noted)
	{
		noted = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_ONFOOT=%s armed (frame %lu)\n",
				s, gMp.frame);
	}

	if (startMs == 0)
		startMs = MpNowMs();

	/* In a session the roster says which car is ours; in single player the
	 * engine's own player[0] is the authority (the same rule the character apply
	 * uses). -1 while already on foot. */
	if (gMp.running)
	{
		MP_PLAYER* me = MpLocalPlayer();

		if (me != NULL)
			carId = me->carId;
	}
	else if (!gInFrontend && GameLevel >= 0)
	{
		carId = (int)player[0].playerCarId;
	}

	/* MP_DEBUG: name the wait and the car state, so "it did not fire" can be read
	 * rather than guessed at. Same inline-condition form as MpLinkTick, so nothing
	 * is conditional on the flag. */
	if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % 60) == 0)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: onfoot waited %lums of %d000ms, carId %d, running %d\n",
			now - startMs, atoi(s), carId, gMp.running);

	if ((now - startMs) < (unsigned long)(atoi(s) * 1000))
		return;

	if (carId < 0 || carId >= MAX_CARS)
		return;			/* not in a car (yet): nothing to get out of */

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: getting OUT now, %ss in (MP_TEST_ONFOOT)\n", s);

	/* The engine's real leave-car path (handling.c) calls
	 * ActivatePlayerPedestrian FIRST: it creates the player's ped and links it
	 * in player[0].pPed, and ChangeCarPlayerToPed then points spoolXZ at that
	 * ped. Calling ChangeCarPlayerToPed directly leaves spoolXZ pointing at a
	 * NULL ped, and the next civ-AI pass (CivControl -> CheckPingOut) faults on
	 * it. Mirror the engine: activate the ped, then step out. */
	ActivatePlayerPedestrian(&car_data[carId], NULL, 0, NULL, TANNER_MODEL);
	ChangeCarPlayerToPed(0);
}

/* Why a claimed left car cannot be entered here, or NULL when every gate the
 * lever applies passes. The named reasons are the point: "no left car" covered
 * a missing claim, a rejected body and a machine that never held the car, and
 * those need different fixes. */
static const char* MpLeftCarRejectReason(const MP_PLAYER* p, int left)
{
	if (left < 0 || left >= MAX_CARS)
		return "slot out of range";

	if (car_data[left].controlType != CONTROL_TYPE_CIV_AI)
		return "controlType is not CIV_AI";

	if (car_data[left].ap.carCos == NULL)
		return "no cosmetics (carCos NULL)";

	if (car_data[left].ap.model != p->leftCarModel)
		return "model does not match the recorded leftCarModel";

	if (car_data[left].ai.padid != &gMpQuietPad)
		return "pad is not the quiet pad";

	return NULL;
}

/* MP_TEST_ENTER_LEFT=<secs>[,<playerId>] -- walk into the car ANOTHER player
 * left, from a headless run.
 *
 * The get-out paths leave the body CIV_AI with the quiet pad, which is exactly
 * what TannerCanEnterCar accepts; the missing half of issue #56 is whether any
 * other machine actually holds that body. A headless run has no way to walk up
 * and press the key, so this lever performs the same gate and reports the
 * answer: find the target row's leftCarSlot1, check the body is the parked
 * CIV_AI car with that model and pad, call TannerCanEnterCar, and only then
 * call ChangePedPlayerToCar. The log line is the evidence either way.
 *
 * <playerId> defaults to 0xFFFF = the first non-local row with a left car. The
 * local player must be ON FOOT; this is the "another player" path, not the
 * owner walking back into their own car (MP_TEST_PAUSECAR covers that).
 *
 * A third field, <exitSecs>, makes the lever walk back OUT that many seconds
 * after it entered (the engine's own ActivatePlayerPedestrian +
 * ChangeCarPlayerToPed), parking the commandeered body. That is how the
 * commandeer-then-get-out case is exercised headlessly: the new owner's row
 * keeps leftCarSlot1 on the body, and the original owner's return must not
 * delete it. */
void MpTestEnterLeftTick(void)
{
	static int done;
	static int noted;
	static int notedOnFoot;
	static int notedMissing;
	static unsigned long startMs;
	static unsigned long exitAtMs;
	const char* s;
	const char* comma;
	int secs, want = 0xFFFF, exitSecs = 0, i, slot = -1, owner = -1;
	const MP_PLAYER* seen = NULL;		/* target row, even with no claim */
	const MP_PLAYER* claim = NULL;		/* first claimed slot that failed a gate */
	const char* why = NULL;			/* its rejection reason */
	int claimLeft = -1;
	MP_PLAYER* me;
	unsigned long now = MpNowMs();

	if (!gMp.running)
		return;

	if (done)
	{
		if (exitAtMs != 0 && (long)(now - exitAtMs) >= 0)
		{
			exitAtMs = 0;

			if (player[0].playerCarId >= 0 && player[0].playerCarId < MAX_CARS)
			{
				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] test: enter left car - getting OUT again, parking the commandeered car\n");

				ActivatePlayerPedestrian(&car_data[player[0].playerCarId], NULL, 0, NULL, TANNER_MODEL);
				ChangeCarPlayerToPed(0);
			}
		}

		return;
	}

	s = gTestEnterLeftStr;

	if (s == NULL)
		return;

	if (!noted)
	{
		noted = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: MP_TEST_ENTER_LEFT=%s armed (frame %lu)\n",
				s, gMp.frame);
	}

	secs = atoi(s);
	comma = strchr(s, ',');

	if (comma != NULL)
	{
		const char* comma2 = strchr(comma + 1, ',');

		want = atoi(comma + 1);

		if (comma2 != NULL)
			exitSecs = atoi(comma2 + 1);
	}

	if (startMs == 0)
		startMs = MpNowMs();

	if ((now - startMs) < (unsigned long)(secs * 1000))
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId >= 0 || player[0].playerCarId >= 0)
	{
		if (!notedOnFoot)
		{
			notedOnFoot = 1;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] test: enter left car needs the local player ON FOOT (carId %d)\n",
					me != NULL ? me->carId : -99);
		}

		return;
	}

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		const MP_PLAYER* p = &gMp.players[i];
		int left;

		if (!p->active || p->isLocal)
			continue;

		if (want != 0xFFFF && p->id != want)
			continue;

		if (seen == NULL)
			seen = p;

		if (p->leftCarSlot1 <= 0)
			continue;

		left = p->leftCarSlot1 - 1;

		/* A claimed body is the only thing the lever can enter. Remember the
		 * first one that fails so the log names WHY, instead of saying there is
		 * no left car at all. */
		if (MpLeftCarRejectReason(p, left) == NULL)
		{
			slot = left;
			owner = p->id;
			break;
		}

		if (claim == NULL)
		{
			claim = p;
			claimLeft = left;
			why = MpLeftCarRejectReason(p, left);
		}
	}

	if (slot < 0)
	{
		if (!notedMissing)
		{
			notedMissing = 1;

			if (gMpCtx != NULL)
			{
				if (want != 0xFFFF && seen == NULL)
				{
					gMpCtx->jer_log(gMpCtx,
						"[mp] test: enter left car - no left car for player %d on this machine: "
						"no active non-local roster row with that id\n",
						want);
				}
				else if (claim != NULL)
				{
					int inRange = (claimLeft >= 0 && claimLeft < MAX_CARS);

					gMpCtx->jer_log(gMpCtx,
						"[mp] test: enter left car - no left car for player %d on this machine: "
						"player %d active=1 local=0 carId=%d onFoot=%d leftCarSlot1=%d "
						"slot=%d controlType=%d model=%d leftModel=%d quietpad=%d carCos=%s - rejected: %s\n",
						claim->id, claim->id, claim->carId, claim->onFoot, claim->leftCarSlot1,
						claimLeft,
						inRange ? (int)car_data[claimLeft].controlType : -1,
						inRange ? (int)car_data[claimLeft].ap.model : -1,
						claim->leftCarModel,
						(inRange && car_data[claimLeft].ai.padid == &gMpQuietPad) ? 1 : 0,
						(inRange && car_data[claimLeft].ap.carCos != NULL) ? "present" : "NULL",
						(why != NULL) ? why : "unknown");
				}
				else
				{
					const MP_PLAYER* row = seen;

					gMpCtx->jer_log(gMpCtx,
						"[mp] test: enter left car - no left car for player %d on this machine: "
						"player %d active=%d local=0 carId=%d onFoot=%d leftCarSlot1=%d leftCarModel=%d - "
						"no claim on this machine (it never held the body, or the NO_CAR rows arrived before a local slot was assigned)\n",
						(row != NULL) ? row->id : ((want == 0xFFFF) ? -1 : want),
						(row != NULL) ? row->id : -1,
						(row != NULL) ? 1 : 0,
						(row != NULL) ? row->carId : -99,
						(row != NULL) ? row->onFoot : -1,
						(row != NULL) ? row->leftCarSlot1 : -1,
						(row != NULL) ? row->leftCarModel : -1);
				}
			}
		}

		return;
	}

	done = 1;

	{
		int can = TannerCanEnterCar(&car_data[slot], 0);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: enter left car - player %d slot %d model %d control %d "
				"quietpad %d TannerCanEnterCar=%d\n",
				owner, slot, (int)car_data[slot].ap.model,
				(int)car_data[slot].controlType,
				car_data[slot].ai.padid == &gMpQuietPad ? 1 : 0, can);

		if (can)
		{
			/* THE ENGINE'S GET-IN PATH PUTS THE LOCAL TANNER AWAY, and this
			 * lever must do the same. ChangePedPlayerToCar clears
			 * player[0].pPed first, so capture the ped before the call and
			 * remove it after -- exactly as MpChangeCarRun's re-entry does.
			 *
			 * Without this the ped from the get-out stays in the pool with
			 * padId 0, numTannerPeds never drops, and the next get-out builds
			 * a SECOND ped for the same player. The stale one is then driven
			 * after its player has moved on; the last time this ran, the exit
			 * wrote a world coordinate over vsync_callback and the intr thread
			 * called it (intrThreadMain+0x7E). */
			LPPEDESTRIAN ped = player[0].pPed;

			ChangePedPlayerToCar(0, &car_data[slot]);
			RemovePlayerPedestrian(ped);

			if (exitSecs > 0)
			{
				exitAtMs = now + (unsigned long)exitSecs * 1000;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] test: enter left car - will get OUT again in %ds, parking the commandeered car\n",
						exitSecs);
			}
		}
	}
}

/* MP_TEST_PAUSECAR=<secs>[,<city>,<model>][;<secs>[,<city>,<model>]...] -- run the
 * pause menu's Change car path that many seconds into a match, exactly as pressing
 * Apply does. A `;`-separated LIST runs several switches, each once and in order; every
 * <secs> counts from the moment the lever armed (not from the previous switch), so
 * `30,2,1;45,3,1` switches to VEGAS 1 at 30 s and RIO 1 at 45 s. The single form is
 * the one-entry list and behaves as it always did.
 *
 * The picker itself needs a human at a menu, so without this the one part of the
 * feature that can go wrong quietly -- the swap and its replication, and (with a
 * list) whether each switch gives the previous car's slot back -- would only ever be
 * looked at by hand. <city> defaults to the session's own and <model> to the SECOND
 * car in that city's roster (the first is usually the one already being driven,
 * which would make the run say nothing). Inert unless set. */
void MpTestPauseCarTick(void)
{
	static int next, noted;
	static unsigned long startMs;
	const char* s;
	const char* e;
	const char* end;
	unsigned long now;
	int secs, city, model = -1, idx, total = 1;

	if (!gMp.running)
	{
		/* SINGLE PLAYER: the same pause-menu row is reachable in a solo game, so the
		 * lever drives it there too -- that is the only padless way to exercise the
		 * single-player swap. It does need the player to BE in a car; MpChangeCar
		 * reports that itself if they are not. */
		if (MainPlayer.playerType != PLAYER_TYPE_CAR)
			return;
	}

	s = getenv("MP_TEST_PAUSECAR");

	if (s == NULL)
		return;

	for (e = s; *e != '\0'; e++)
	{
		if (*e == ';' && e[1] != '\0')
			total++;
	}

	if (!noted)
	{
		noted = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_PAUSECAR=%s armed - %d switch(es) (frame %lu)\n",
				s, total, gMp.frame);
	}

	if (startMs == 0)
		startMs = MpNowMs();

	/* the entry to run next */
	e = s;

	for (idx = 0; idx < next && e != NULL; idx++)
	{
		e = strchr(e, ';');

		if (e != NULL)
			e++;
	}

	if (e == NULL || *e == '\0')
		return;			/* every entry has run */

	now = MpNowMs();
	secs = atoi(e);

	if ((int)((now - startMs) / 1000) < secs)
		return;

	next++;

	city = (gMp.city >= 0 && gMp.city < 4) ? gMp.city : 0;
	end = strchr(e, ';');

	{
		const char* c1 = strchr(e, ',');

		if (c1 != NULL && (end == NULL || c1 < end))
		{
			const char* c2;

			city = atoi(c1 + 1);
			c2 = strchr(c1 + 1, ',');

			if (c2 != NULL && (end == NULL || c2 < end))
				model = atoi(c2 + 1);
		}
	}

	if (model < 0)
	{
		int slots[MP_CAR_LIST_MAX];
		int models[MP_CAR_LIST_MAX];
		char list[192];
		size_t used;
		int n, k;

		if (city < 0 || city >= CITY_COUNT)
			city = 0;

		/* The SAME answer the pause picker builds its menu from (MpCarListForCity), so a
		 * padless run can still see what the menu would offer - and the two cannot drift,
		 * because it is one call. Printing the roster (not just a count) is the point: the
		 * question this lever keeps being used to answer is "which cars can players pick?". */
		n = MpCarListForCity(city, slots, models, MP_CAR_LIST_MAX);

		list[0] = 0;

		for (k = 0; k < n; k++)
		{
			used = strlen(list);

			if (used < sizeof(list) - 24)
				snprintf(list + used, sizeof(list) - used, "%s[%d]=%d",
					(k > 0) ? " " : "", slots[k], models[k]);
		}

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: PAUSECAR city %d roster: %d slot(s) - %s\n",
				city, n, (list[0] != 0) ? list : "(none)");

		/* the SECOND car in the list, as this lever always did: a change onto a different
		 * model, so a run can tell the switch happened */
		if (n > 0)
			model = models[(n > 1) ? 1 : 0];
	}

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] test: PAUSECAR change %d of %d now to city %d model %d\n",
			next, total, city, model);

	MpChangeCar(city, model);
}

/* MP_TEST_RESTART=<secs> -- ask for the multiplayer Restart that many seconds
 * into a match, exactly as pressing Restart in the pause menu does.
 *
 * It FIRES THE ENGINE'S EVENT rather than calling the reset directly, so it can
 * only pass when the whole chain is wired: the hook in main.c, mp's claim, and
 * MpSoftRestart. Inert unless set. */
void MpTestRestartTick(void)
{
	static int done, noted;
	static unsigned long startMs;
	const char* s;
	unsigned long now;

	if (done || !gMp.running)
		return;

	s = getenv("MP_TEST_RESTART");

	if (s == NULL)
		return;

	if (!noted)
	{
		noted = 1;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_RESTART=%s armed (frame %lu)\n",
				s, gMp.frame);
	}

	if (startMs == 0)
		startMs = MpNowMs();

	now = MpNowMs();

	if ((int)((now - startMs) / 1000) < atoi(s))
		return;

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] test: pressing RESTART now\n");

	{
		JER_ARGS_GAME_QUIT q;

		q.code = JER_PAUSE_QUIT_RESTART;

		jer_fire(JER_EVENT_GAME_QUIT, &q);
	}
}

/* MP_TEST_FALLOFF=<secs> -- drop our car off the map that many seconds in.
 *
 * The only way to reach the game-over refusal (JER_PAUSE_GAMEOVER) without driving off a
 * cliff by hand: the engine arms the death fade when the car's y goes below -1000, so this
 * just puts it there. What proves the refusal is the pair of log lines - the engine's fade
 * arming and then our "[mp] death in a session: respawned ... game over refused" - with no
 * pause menu and no black screen after it. */
void MpTestFallOffTick(void)
{
	static int done;
	static unsigned long startMs;
	const char* s;
	MP_PLAYER* me;

	if (done || !gMp.running)
		return;

	s = gTestFallOffStr;

	if (s == NULL)
		return;

	if (startMs == 0)
		startMs = MpNowMs();

	if (MpNowMs() - startMs < (unsigned long)atoi(s) * 1000UL)
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_FALLOFF - dropping our car off the map\n");

	car_data[me->carId].hd.where.t[1] = -2000;
}

void MpTestCarChangeTick(void)
{
	static int stage;
	static unsigned long startMs;
	const char* s;
	const char* comma;
	int secs, exitSecs = 0, i, best = -1, bestDiff = -1;
	long bestD = 0, bestDiffD = 0;
	MP_PLAYER* me;
	CAR_DATA* mine;
	unsigned long now;

	if (stage >= 2)
		return;

	s = gTestCarChangeStr;

	if (s == NULL || !gMp.running)
		return;

	secs = atoi(s);

	/* MP_TEST_CARCHANGE=<changeSecs>[,<exitSecs>] -- the second number makes it
	 * also GET OUT that long after the change, so the peer-side on-foot path (and
	 * the engine's own player -> CIV_AI handover) is exercised as well. */
	comma = strchr(s, ',');
	if (comma != NULL)
		exitSecs = atoi(comma + 1);

	if (startMs == 0)
		startMs = MpNowMs();

	now = MpNowMs();

	if (stage == 1)
	{
		if (exitSecs <= 0 || (int)((now - startMs) / 1000) < secs + exitSecs)
			return;

		stage = 2;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] TEST car change: getting OUT now\n");

		/* Same as MP_TEST_ONFOOT: activate the ped first so ChangeCarPlayerToPed
		 * has a valid player[0].pPed to point spoolXZ at (else the next civ-AI
		 * pass faults in CivControl -> CheckPingOut). */
		me = MpLocalPlayer();

		if (me != NULL && me->carId >= 0)
			ActivatePlayerPedestrian(&car_data[me->carId], NULL, 0, NULL, TANNER_MODEL);

		ChangeCarPlayerToPed(0);
		return;
	}

	if ((int)((now - startMs) / 1000) < secs)
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0)
		return;

	mine = &car_data[me->carId];

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		long dx, dy, dz, d;

		/* ONLY a proper civilian car. Taking over a car that is NOT one (a spooled
		 * placeholder, a special) and then vacating it hands the traffic AI a car
		 * whose civ-AI state was never set up, and it crashes in CivSteerAngle. Keep
		 * the lever to the same kind of car a player could actually get into. */
		if (i == me->carId || cp->controlType != CONTROL_TYPE_CIV_AI)
			continue;

		/* squared distances are 64 bit: a 32-bit one wraps negative on a far car
		 * and reads as the NEAREST one */
		dx = (long)cp->hd.where.t[0] - mine->hd.where.t[0];
		dy = (long)cp->hd.where.t[1] - mine->hd.where.t[1];
		dz = (long)cp->hd.where.t[2] - mine->hd.where.t[2];
		d = dx * dx + dy * dy + dz * dz;

		if (best < 0 || d < bestD)
		{
			best = i;
			bestD = d;
		}

		/* prefer a car of a DIFFERENT model, so the test always exercises the
		 * model swap and not just a move between two cars of the same kind */
		if (cp->ap.model != mine->ap.model && (bestDiff < 0 || d < bestDiffD))
		{
			bestDiff = i;
			bestDiffD = d;
		}
	}

	if (bestDiff >= 0)
	{
		best = bestDiff;
		bestD = bestDiffD;
	}

	if (best < 0)
		return;		/* no traffic nearby yet -- try again next frame */

	/* what was there to choose from, and what we are -- so a run says whether the
	 * takeover could even change the model */
	if (gMpCtx != NULL)
	{
		char list[160];
		int c, shown = 0;

		list[0] = 0;

		for (c = 0; c < MAX_CARS && shown < 8; c++)
		{
			size_t used;

			if (c == me->carId || car_data[c].controlType == CONTROL_TYPE_NONE)
				continue;

			used = strlen(list);
			snprintf(list + used, sizeof(list) - used, " %d:m%d", c, car_data[c].ap.model);
			shown++;
		}

		gMpCtx->jer_log(gMpCtx,
			"[mp] TEST car change: we are model %d; what is in the world:%s\n",
			mine->ap.model, list);
	}

	stage = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] TEST car change: taking over slot %d (model %d), %ld away\n",
			best, car_data[best].ap.model, bestD);

	ChangePedPlayerToCar(0, &car_data[best]);
}

/* MP_TEST_CITYCHANGE=<secs>,<city>[,<slot>] -- the pause menu's "Respawn as this car",
 * taken by itself at <secs> seconds, for a car from a DIFFERENT city.
 *
 * MP_TEST_CARCHANGE above takes over the nearest LOCAL traffic car, and that is the wrong
 * test for the mid-match import: the peer is in the same level, so it already holds that
 * car and there is nothing to import at all. What has to be imported is a car the peer does
 * NOT hold, and a car from another city is exactly that -- so this lever drives the real
 * case, through the same entry point the menu uses (MpChangeCar), deliberately, rather than
 * through a second path that could drift away from it.
 *
 * city indexes the cities the car mods know (MpCarQueryCities), slot indexes the engine's
 * own per-city model table (carNumLookup), and both are explicit so a pair run can send each
 * seat to a different city and exercise both directions at once.
 *
 * Set = "<secs>,<city>[,<slot>]". Any other shape does nothing, so an absent or malformed
 * value can never change a car by itself. */
void MpTestCityChangeTick(void)
{
	static int done;
	static unsigned long startMs;
	const char* s;
	const char* comma;
	extern char carNumLookup[CITY_COUNT][10];
	int secs, city, slot = 0, i, n, model;
	int cities[8];
	MP_PLAYER* me;
	unsigned long now;

	if (done)
		return;

	s = gTestCityChangeStr;

	if (s == NULL || !gMp.running)
		return;

	comma = strchr(s, ',');

	if (comma == NULL)
		return;			/* "<secs>,<city>" is the minimum */

	secs = atoi(s);
	city = atoi(comma + 1);

	comma = strchr(comma + 1, ',');

	if (comma != NULL)
	{
		slot = atoi(comma + 1);

		if (slot < 0 || slot >= 10)
			slot = 0;
	}

	if (startMs == 0)
		startMs = MpNowMs();

	now = MpNowMs();

	if ((int)((now - startMs) / 1000) < secs)
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0)
		return;			/* no car of ours yet: try again next frame */

	/* Only a city the mods know, and only an index the engine's own model table has:
	 * MpChangeCar resolves the pair against resident slots, so anything else is a failed
	 * switch reported as if it were a real one. */
	if (city < 0 || city >= CITY_COUNT)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: MP_TEST_CITYCHANGE city %d is out of range - nothing done\n", city);

		done = 1;
		return;
	}

	n = MpCarQueryCities(cities, (int)(sizeof(cities) / sizeof(cities[0])));

	for (i = 0; i < n; i++)
		if (cities[i] == city)
			break;

	if (i == n)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: MP_TEST_CITYCHANGE city %d is not one the mods know - nothing done\n",
				city);

		done = 1;
		return;
	}

	model = (int)(unsigned char)carNumLookup[city][slot];

	done = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: MP_TEST_CITYCHANGE -> %s model %d (we are model %d)\n",
			MpCarCityName(city), model, car_data[me->carId].ap.model);

	MpChangeCar(city, model);
}

/* MP_TEST_CARCYCLE=<interval_ms>[,<passes>]
 *
 * Drive this seat through EVERY car the session can offer, in a random order, as fast as
 * asked - the stress rig for the whole car path. Three seats on one machine at 700 ms each
 * is a car change somewhere in the session every ~230 ms, far past anything a player does,
 * and it is where slot reuse, the palette rows, the texture pages and the peer folds either
 * hold or start to leak, go invisible or refuse.
 *
 * The list comes from MpCarListForCity - the SAME answer the pause picker builds its menu
 * from - so "every car" is every car this session would let a player pick, rather than a
 * guess at model numbers that the level may not hold. Each seat shuffles with its own seed
 * (its player id), so the seats walk the same list in different orders: one is importing a
 * car while another is switching away from it, which is the interesting overlap.
 *
 * Every change goes through MpChangeCar, the call the pause menu's Apply row makes, so this
 * exercises the real path and not a shortcut. Each completed pass is logged, and so is every
 * change, because the rig counts them.
 *
 * passes = 0 (the default) keeps reshuffling until the run ends. */
static void MpTestShuffle(int (*items)[2], int count, int* seed)
{
	int i;

	for (i = count - 1; i > 0; i--)
	{
		int j, a, b;

		*seed = *seed * 1103515245 + 12345;
		j = (int)(((unsigned)(*seed >> 8)) % (unsigned)(i + 1));

		if (j == i)
			continue;

		a = items[i][0];
		b = items[i][1];
		items[i][0] = items[j][0];
		items[i][1] = items[j][1];
		items[j][0] = a;
		items[j][1] = b;
	}
}

void MpTestCarCycleTick(void)
{
	static int built;
	static int intervalMs = -1;
	static int passes;
	static unsigned long nextAt;
	static int idx, total, pass, seed;
	static int passLanded;	/* of this pass, how many actually landed */
	static int cars[MP_TEST_CYCLE_MAX_CARS][2];
	MP_PLAYER* me;

	if (gTestCarCycleStr == NULL || !gMp.running)
		return;

	me = MpLocalPlayer();

	if (me == NULL || me->carId < 0)
		return;

	if (intervalMs < 0)
	{
		const char* comma = strchr(gTestCarCycleStr, ',');

		intervalMs = atoi(gTestCarCycleStr);

		if (intervalMs < 50)
			intervalMs = 50;		/* below this a car cannot be built before the next ask */

		passes = (comma != NULL) ? atoi(comma + 1) : 0;
	}

	if (!built)
	{
		int city;

		built = 1;
		seed = (int)(0x2545F491u * (unsigned)(me->id + 1));	/* per seat: different orders */

		/* JERICHO: the row capacity is the bound, but a bank row that is not
		 * registered is not a city - a configured bank with no files leaves a hole,
		 * and cycling a car out of it would build nothing on any peer. */
		for (city = 0; city < CITY_COUNT && total < MP_TEST_CYCLE_MAX_CARS; city++)
		{
			int models[MP_CAR_LIST_MAX];
			int slots[MP_CAR_LIST_MAX];
			int count;
			int i;

			if (!JerCityRegistered(city))
				continue;

			count = MpCarListForCity(city, slots, models, MP_CAR_LIST_MAX);

			for (i = 0; i < count && total < MP_TEST_CYCLE_MAX_CARS; i++)
			{
				cars[total][0] = city;
				cars[total][1] = models[i];
				total++;
			}
		}

		if (total == 0)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] test: car cycle: no car is offered - nothing to cycle\n");

			gTestCarCycleStr = NULL;
			return;
		}

		MpTestShuffle(cars, total, &seed);
		nextAt = MpNowMs();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: car cycle: %d car(s), a change every %d ms, %s\n",
				total, intervalMs, (passes > 0) ? "a fixed number of passes" : "until the run ends");
	}

	if (MpNowMs() < nextAt)
		return;

	nextAt = MpNowMs() + (unsigned long)intervalMs;

	/* The outcome is what the rig is FOR, and it is not a given: a change can be waiting
	 * for a spare slot (MpChangeCar remembers it and retries) or refused outright. So
	 * say which it was. An earlier version told the rig every car had been driven
	 * because it assumed the call always landed -- the rig then compared THAT against
	 * a list of asks, and a run that changed nothing still read as full coverage. */
	if (MpChangeCar(cars[idx][0], cars[idx][1]))
	{
		passLanded++;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] test: car cycle: drove %s model %d\n",
				MpCarCityName(cars[idx][0]), cars[idx][1]);
	}

	idx++;

	if (idx < total)
		return;

	pass++;
	idx = 0;
	MpTestShuffle(cars, total, &seed);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] test: car cycle PASS %d done - %d of the %d car(s) landed as asked\n",
			pass, passLanded, total);

	passLanded = 0;

	if (passes > 0 && pass >= passes)
		gTestCarCycleStr = NULL;		/* asked for a number of passes and they are done */
}

/* MP_TEST_RELOADCARS=<sim frame>: run carhacks' "Reload all cars" once at that frame.
 * A headless run cannot open the pause menu, and this is the operation that has to
 * release and re-read every imported asset, so it needs a lever. Inert unless set. */
void MpTestReloadCarsTick(void)
{
	static int done;
	static int at = -1;

	if (done)
		return;

	if (at < 0)
	{
		const char* s = getenv("MP_TEST_RELOADCARS");

		if (s == NULL || *s == '\0')
		{
			done = 1;
			return;
		}

		at = atoi(s);

		if (at < 1)
			at = 1;
	}

	if (!gMp.running || gMp.frame < (unsigned long)at)
		return;

	done = 1;

	{
		extern int chkImportReloadAll(void);
		int n = chkImportReloadAll();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] test: Reload all cars -> %d imported slot(s) rebuilt (MP_TEST_RELOADCARS)\n", n);
	}
}

/* Headless verification lever: import Jericho at level start, regardless of the
 * player being in a car or a session. Inert unless MP_TEST_JERICHO is set. */
void MpTestJerichoImport(void)
{
	const char* s = getenv("MP_TEST_JERICHO");
	int verbose = (s != NULL && s[0] != '0');
	int ok;

	if (GameLevel < 0)
		return;

	/* The shipping v5 archive carries the selectable entries, so warm it at
	 * level start: the picker can then list every entry before the player first
	 * gets out. MP_TEST_JERICHO still forces the diagnostic import. */
	if (!verbose && !jer_ped_archive_baked_available())
		return;

	ok = MpEnsureJerichoImported() && MpEnsureJerichoInstalled();

	if (gMpCtx != NULL)
	{
		if (verbose)
			gMpCtx->jer_log(gMpCtx, "[mp] test: MP_TEST_JERICHO import %s (level %d)\n",
				ok ? "succeeded" : "FAILED", GameLevel);
		else if (ok)
			gMpCtx->jer_log(gMpCtx, "[mp] character: archive entries ready (%d)\n",
				jer_ped_archive_entry_count());
	}
}
