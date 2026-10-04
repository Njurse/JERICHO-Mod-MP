/* ------------------------------------------------------------------ */
/* The multiplayer TEST BOT.                                            */
/*                                                                      */
/* A testing component: it drives a real player's car so two instances  */
/* can be exercised without two humans. NOTHING in the mod's session,   */
/* networking or gameplay path may depend on it, and it is inert unless */
/* MP_BOT asks for it (see mp_bot.h).                                   */
/* ------------------------------------------------------------------ */
#include "jericho.h"
#include "jer_events.h"

#include "driver2.h"
#include "cars.h"
#include "pad.h"
#include "players.h"	/* player[]: the on-foot bot drives OUR pedestrian */
#include "objcoll.h"	/* CellEmpty: the engine's own scenery test */
#include "dr2roads.h"	/* JerRoadAt / JerRoadInfoAt: the engine's own road network */
#include "ai/aimap.h"	/* the AI's world model */
#include "ai/aistar.h"	/* the pathfinder */
#include "ai/ailocal.h"	/* the local road / flee-goal search */
#include "jer_hud.h"	/* MP_BOT_DRAW: the live readout */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mp.h"
#include "mp_bot.h"

/* How wide a gap each body needs when probing for scenery. A car is a car; a
 * person fits down gaps a car does not, and the on-foot bot must use its own
 * number or it will refuse to walk up a pavement. */
#define MPBOT_CAR_CLEAR	350
#define MPBOT_PED_CLEAR	90

/* The canned manoeuvre: a pad that changes every 0.5-4 s (steer left/right,
 * reverse, wheelspin, handbrake). Because the local car is what we replicate,
 * a peer seeing it move proves the controls arrived. */
static int MpBotCanned(void)
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

/* The fleeing host's two distance thresholds, in world units, from
 * MP_BOT_GAP=<ease>,<turnback> (default 2500,5000). Runtime levers on purpose: how
 * close the pair should stay is a feel question, and feel should not need a rebuild.
 * Resolved once, like MP_BOT itself. */
/* How far the fleer runs before it stops widening the gap (ease) and before it gives up and
drives back at the chasers (turnback). Both were 2500/5000, which a pair hits almost at once -
the host was looping back before the chaser had closed any distance at all, so the chase never
really happened. Extended 3x, for the same reason the thresholds exist in the first place. */
static int gBotGapEase = 7500;
static int gBotGapTurnback = 15000;
static int gBotGapResolved;

static void MpBotResolveGap(void)
{
	const char* v;

	if (gBotGapResolved)
		return;

	gBotGapResolved = 1;

	v = getenv("MP_BOT_GAP");

	if (v == NULL)
		return;

	gBotGapEase = atoi(v);

	{
		const char* comma = strchr(v, ',');

		gBotGapTurnback = (comma != NULL) ? atoi(comma + 1) : gBotGapEase * 2;
	}

	if (gBotGapTurnback < gBotGapEase)
		gBotGapTurnback = gBotGapEase;
}

/* Which bot, if any. OFF unless MP_BOT says otherwise -- these levers drive a
 * real player's car, so nothing here may be on by default. Returns 0 (none),
 * 1 (random), 2 (chase), 3 (fight), 4 (pursuit: host hunts, joiner runs) or
 * 5 (catmouse: the same pair, DRIVEN by the pathfinder). */
static int MpBotMode(void)
{
	const char* m = getenv("MP_BOT");

	MpBotResolveGap();

	if (m == NULL)
		return (getenv("MP_TESTDRIVE") != NULL) ? 1 : 0;

	if (strcmp(m, "chase") == 0)
		return 2;
	if (strcmp(m, "fight") == 0)
		return 3;
	if (strcmp(m, "pursuit") == 0)
		return 4;
	if (strcmp(m, "catmouse") == 0)
		return 5;
	if (strcmp(m, "off") == 0 || strcmp(m, "0") == 0)
		return 0;

	return 1;
}

/* Is the spot `len` units along heading `dir` clear of scenery? The engine's own test
 * (CellEmpty, what the civ AI uses), with the same clearance -- so this asks the game,
 * not a private model of it. Shared by the car and the pedestrian bots: only the body's
 * position and how wide a gap it needs differ. */
static int MpBotSpotClearAt(int x, int y, int z, int dir, int len, int radius)
{
	VECTOR p;

	p.vx = x + (int)(((long)rsin(dir) * len) >> 12);
	p.vy = y;
	p.vz = z + (int)(((long)rcos(dir) * len) >> 12);

	return CellEmpty(&p, radius);
}

/* CONTACT. The forward trace starts `len` units ahead of the car and CellEmpty only looks
 * at the cell THAT POINT falls in (objcoll.c:41-44) - `radius` widens the box test, it does
 * not widen the cell search. So a wall the car is already touching can sit BEHIND the first
 * probe and the trace reads "clear" while the car is embedded in it, which is exactly how a
 * car gets pinned on scenery it believes is not there. Testing the car's OWN position closes
 * that hole: a collision object inside a car-radius of the centre means contact, whatever
 * the forward trace says. Only used for the wedge test - it is the same answer for every
 * heading, so it must never take part in choosing one. */
static int MpBotContacted(CAR_DATA* mine)
{
	return !MpBotSpotClearAt(mine->hd.where.t[0], mine->hd.where.t[1], mine->hd.where.t[2],
		mine->hd.direction, 0, MPBOT_CAR_CLEAR);
}

static int MpBotSpotClear(CAR_DATA* mine, int dir, int len)
{
	return MpBotSpotClearAt(mine->hd.where.t[0], mine->hd.where.t[1], mine->hd.where.t[2],
		dir, len, MPBOT_CAR_CLEAR);
}

/* chase/fight: drive the LOCAL car at (or, for the fleeing host, away from) the
 * nearest other player's car. Two instances then close the gap on their own,
 * which is how car-to-car collision and the two players meeting each other get
 * exercised without two humans. Returns 0 (coast) when there is nobody else. */
/* ------------------------------------------------------------------ */
/* TURN AROUND, DO NOT REVERSE                                          */
/*                                                                     */
/* Both driving bots used to recover by REVERSING: "backing out"         */
/* (CAR_PAD_BRAKE | steer), and, on a big heading error, "reversing round */
/* is the only coherent way about". Measured in the pair harness, that    */
/* reads as the cars SHUFFLING instead of chasing round the map: the       */
/* reverse half of a recovery puts the car back where the previous half     */
/* started, and a stopped car given steer-only cannot turn at all, so the   */
/* pair stayed wedged in the scenery and never met.                        */
/*                                                                     */
/* A turn-around does the same job in one movement and leaves the car      */
/* pointing somewhere new. Two rules, both from what this engine's car      */
/* physics needs:                                                           */
/*   * the wheels must be ROLLING for a handbrake to pivot the car, so the  */
/*     handbrake is held only for the first frames of the manoeuvre and     */
/*     only while there is speed - on a stopped car it is a locked axle;     */
/*   * the throttle stays on throughout, because steer-only cannot move a    */
/*     car - that is what left the old "spinning round" recovery parked.     */
/* The driving bots never select CAR_PAD_BRAKE any more.                     */
/* ------------------------------------------------------------------ */
#define MPBOT_TURN_FRAMES	20	/* the handbrake part, ~0.3 s at 60 Hz */
#define MPBOT_TURN_MIN_SPEED	10	/* below this a handbrake is a locked axle, not a turn */

/* A dodge that is re-decided every half second reads as indecision: the car wiggles down
the road instead of going anywhere. ~1.1 s at 60 Hz is long enough to commit to one side
and short enough to abandon it when it stops helping. */
#define MPBOT_DODGE_FRAMES	65

/* Wedged is speed-below-4 for this long before anything is done about it. 40 frames was
0.7 s of sitting in the corner first, which is most of why the pair looked lazy. */
#define MPBOT_STUCK_FRAMES	24

/* The ONE reverse the driving bots still use: backing out of a wall that is dead ahead,
where turning on the throttle cannot work because the car cannot move at all. Bounded,
and it does not alternate, so it cannot become the old reversing shuffle. */
#define MPBOT_BACK_FRAMES	22	/* ~0.4 s */

/* Following at minimum distance is not a reason to do anything dramatic: hold station
inside IN and only take the chase up again once the gap has reached OUT. Two thresholds,
not a derivative, so the car cannot flicker between chasing and waiting. */
#define MPBOT_FOLLOW_IN		900L
#define MPBOT_FOLLOW_OUT	2200L

/* One frame of a turn-around. `dir` uses the callers' steering convention
 * (non-zero = left). `*frames` is the manoeuvre and `*pulse` the handbrake part
 * of it; both are decremented here. */
static int MpBotTurnPad(int* frames, int* pulse, int dir, int spd)
{
	int left = (dir != 0);
	int pad = CAR_PAD_ACCEL | (left ? CAR_PAD_LEFT : CAR_PAD_RIGHT);

	if (spd < 0)
		spd = -spd;

	(*frames)--;

	if (*pulse > 0)
	{
		(*pulse)--;

		if (spd > MPBOT_TURN_MIN_SPEED)
			pad = CAR_PAD_HANDBRAKE | (left ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	return pad;
}

/* FLEE SCANNING. Running exactly 180 degrees from the pursuer is ONE fixed heading:
 * when that heading is a wall the fleer has nothing else in mind, so it circles in
the corner it just ran into. Instead, sweep a fan of headings that all still gain
ground on the pursuer and take the one with the most room ahead - the flee then heads
for open space rather than for whatever happens to be behind it. Cheaper than a route
and it fixes the symptom that is actually visible. */
#define MPBOT_FLEE_FAN	256	/* 22.5 deg steps */
#define MPBOT_FLEE_SPAN	6	/* +/- 135 deg: anywhere open beats a wall behind him */
#define MPBOT_FLEE_WIDTH	128	/* 11 deg either side must be clear too: no grazing */

/* Is there a CORRIDOR along `dir`, not merely a line? Probing the centre line alone lets
the fleer pick a heading that clears a wall by centimetres - which is what "it grazes
along walls" is - so a heading only counts as open if the lines either side of it are
open too. No trigonometry needed: the side lines are just the heading rotated. */
static int MpBotCorridorClear(CAR_DATA* mine, int dir, int len)
{
	return MpBotSpotClear(mine, dir, len) &&
		MpBotSpotClear(mine, (dir - MPBOT_FLEE_WIDTH) & 0xfff, len) &&
		MpBotSpotClear(mine, (dir + MPBOT_FLEE_WIDTH) & 0xfff, len);
}

/* How far the corridor stays open, in range steps. THIS is the "that is a road" test: a
 * heading still clear at 4800 units is a street, one that clears 1200 and then stops is a
 * driveway into a wall. The flee wants the road even when the road is not straight back
 * the way it came. */
#define MPBOT_FLEE_STEP		350	/* a probe every car-radius, so coverage is CONTINUOUS */
#define MPBOT_FLEE_REACH	14	/* 14 * 350 = ~4900 units of look-ahead */
#define MPBOT_FLEE_ROAD		8	/* open for ~2800 units counts as a road, not a gap */

static int MpBotCorridorDepth(CAR_DATA* mine, int dir)
{
	int k, depth = 0;

	/* DENSE SAMPLING IS THE POINT. These were four widely spaced probes, so a light
	 * post or a bollard between two of them was invisible until the car was on top of
	 * it, and a thin obstacle slightly off the line read as clear ground. At one probe
	 * per probe-radius the corridor is swept rather than dotted. The early break keeps
	 * the usual cost at two or three samples. */
	for (k = 1; k <= MPBOT_FLEE_REACH; k++)
	{
		if (!MpBotCorridorClear(mine, dir, k * MPBOT_FLEE_STEP))
			break;

		depth++;
	}

	return depth;
}

#define MPBOT_FLEE_ROAD_AT	1600	/* how far along a heading the road question is asked */

/* IS THAT HEADING ON A ROAD? The engine already knows where the roads are, so ask it instead
 * of guessing from geometry: a scenery probe cannot tell a street from the MAP EDGE, and the
 * map edge is exactly what the flee used to pick as "away", cornering itself against the
 * boundary. This is the fix for "stay along a road path". */
static int MpBotOnRoadTo(CAR_DATA* mine, int dir, int len)
{
	return JerRoadAt(
		mine->hd.where.t[0] + (int)(((long)rsin(dir) * len) >> 12),
		mine->hd.where.t[1],
		mine->hd.where.t[2] + (int)(((long)rcos(dir) * len) >> 12));
}

#define MPBOT_FLEE_KEEP		10	/* margin: a heading this much better is worth switching for.
					 * A neighbouring heading that gains ONE depth step is only 5 better
					 * (depth is worth 3, the off-axis penalty 2), which is not enough -
					 * the car then alternates between two adjacent steps and wobbles. */

static int MpBotFleeWant(CAR_DATA* mine, int away)
{
	static int lastLogged = -1;
	static int held = -1, heldDepth = 0;
	int best = away, bestScore = -9999, bestDepth = 0, i;
	int road = away, roadScore = -9999, roadDepth = 0, foundRoad = 0;
	int heldScore = -9999, heldSeen = 0;

	for (i = -MPBOT_FLEE_SPAN; i <= MPBOT_FLEE_SPAN; i++)
	{
		int d = (away + i * MPBOT_FLEE_FAN) & 0xfff;
		int depth = MpBotCorridorDepth(mine, d);

		/* A ROAD BEATS A GAP: depth (how far it stays open) is worth more than
		 * pointing exactly away, so the flee turns down a street rather than running
		 * at the wall behind it. The |i| penalty still stops it turning tail and
		 * driving AT the pursuer just because that way happens to be a long road. */
		int score = depth * 3 - (i < 0 ? -i : i) * 2;

		if (score > bestScore)
		{
			bestScore = score;
			bestDepth = depth;
			best = d;
		}

		/* where the heading we are ALREADY committed to scored this frame */
		if (held >= 0 && d == held)
		{
			heldSeen = 1;
			heldScore = score;
			heldDepth = depth;
		}

		/* a second pass for the road: any heading that is on the road network beats
		 * any heading that is not, which is what keeps the flee on the map and moving
		 * rather than cornering itself at the edge */
		if (MpBotOnRoadTo(mine, d, MPBOT_FLEE_ROAD_AT) && score > roadScore)
		{
			foundRoad = 1;
			roadScore = score;
			roadDepth = depth;
			road = d;
		}
	}

	if (foundRoad)
	{
		best = road;
		bestDepth = roadDepth;
	}

	/* HYSTERESIS. Re-deciding the heading every frame is what makes the car wobble down
	 * the road: two headings that score within a point of each other alternate, so the
	 * steering never settles and the flee looks unsure of itself. Keep the heading we
	 * already chose unless a new one is CLEARLY better. */
	if (heldSeen && heldScore >= bestScore - MPBOT_FLEE_KEEP)
	{
		best = held;
		bestDepth = heldDepth;
	}
	else
	{
		held = best;
		heldDepth = bestDepth;
	}

	/* Make the scan visible: without this there is no way to tell a fleer that is
	 * choosing open ground from one that is not. Only when it differs from straight
	 * back, and only on change, so a run is readable rather than a wall of lines. */
	if (best != away && best != lastLogged && gMpCtx != NULL)
	{
		lastLogged = best;
		gMpCtx->jer_log(gMpCtx,
			"[mp] chase: flee scan - straight back is not the way out, heading %d of 4096 (open %d range(s)%s%s)\n",
			best, bestDepth, bestDepth >= MPBOT_FLEE_ROAD ? ", a road" : "",
			foundRoad ? ", on the road network" : ", OFF the road network");
	}
	else if (best == away)
		lastLogged = -1;

	return best;
}

static int MpBotChase(int fight)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* mine;
	CAR_DATA* tgt = NULL;
	static int stuckFrames, turnFrames, turnDir, turnPulse, backFrames, backDir, holding, everMoved, modeLogged;
	int k;

	if (me == NULL || me->carId < 0)
		return 0;

	for (k = 0; k < MP_MAX_PLAYERS; k++)
	{
		MP_PLAYER* p = &gMp.players[k];

		if (p->active && p->carId >= 0 && p->carId != me->carId)
		{
			tgt = &car_data[p->carId];
			break;
		}
	}

	if (tgt == NULL)
		return 0;

	mine = &car_data[me->carId];

	/* The very primitive "pathfinder": a straight line at the peer is enough on
	 * an open map, but the cars wedge on the first building and never meet again.
	 * A wedge is cleared by TURNING (MpBotTurnPad) - a reverse moves the car back
	 * to where the wedge started, which is how the pair ended up shuffling on the
	 * spot instead of chasing - EXCEPT when a wall is dead ahead, where turning on
	 * the throttle cannot work and a short back-out is the only thing that frees
	 * the car. NO WHEELSPIN either: spinning the wheels is a grip loss, which is
	 * what made the cars bobble and slide into the scenery. */
	if (backFrames > 0)
	{
		backFrames--;

		return CAR_PAD_BRAKE | (backDir ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	if (turnFrames > 0)
		return MpBotTurnPad(&turnFrames, &turnPulse, turnDir, mine->hd.speed);

	{
		int dx = tgt->hd.where.t[0] - mine->hd.where.t[0];
		int dz = tgt->hd.where.t[2] - mine->hd.where.t[2];
		int flee = (!fight && MpIsHost());	/* chase: the host runs, the joiner chases. fight: both charge. */

		/* Say which way round this seat is running, ONCE. "Both seats chased" and "both
		 * seats fled" are the first things to rule out when a pair behaves strangely,
		 * and without this line it can only be inferred from which side logs recoveries.
		 * The role is the LOCAL one (gMp.role == MP_ROLE_HOST), and MP_ROLE_NONE is 0
		 * rather than HOST, so a seat that has not joined yet is not accidentally a
		 * host - it chases until it is given a role. */
		if (!modeLogged && gMpCtx != NULL)
		{
			modeLogged = 1;
			gMpCtx->jer_log(gMpCtx, "[mp] chase: this seat is the %s, so it %s\n",
				MpIsHost() ? "HOST" : "JOINER", flee ? "FLEES" : "CHASES");
		}
		int want = flee ? MpBotFleeWant(mine, (ratan2(dx, dz) + 2048) & 0xfff)
		                : (ratan2(dx, dz) & 0xfff);
		int diff, adiff;
		long dist;
		int pad;
		int easeOff = 0;
		int looping = 0;	/* the flee gave up running and is coming back */

		/* The flee has no business opening the gap forever: measured, the host ran
		 * to 38,000 units and stayed there, so the pair never met and nothing was
		 * exercised. Two thresholds, from MP_BOT_GAP=<ease>,<turnback> (default
		 * 2500,5000): beyond `ease` the host lifts the throttle so it stops
		 * widening the gap, and beyond `turnback` it stops fleeing and drives AT
		 * the chasers instead - a meeting from both ends, which is what keeps the
		 * pair close enough to actually collide. */
		dist = (long)dx * (long)dx + (long)dz * (long)dz;

		if (flee && gBotGapTurnback > 0 &&
			dist > (long)gBotGapTurnback * (long)gBotGapTurnback)
		{
			looping = 1;
			flee = 0;			/* from here it is a chaser: same steering, closing */
			want = ratan2(dx, dz) & 0xfff;
		}
		else if (flee && gBotGapEase > 0 &&
			dist > (long)gBotGapEase * (long)gBotGapEase)
		{
			easeOff = 1;
		}

		/* Scenery awareness, using the engine's OWN test: CellEmpty is what the
		 * civ AI uses to know a spot is clear. Probe ALONG THE CAR'S VELOCITY
		 * VECTOR (falling back to where we want to go when parked), at TWO ranges:
		 * one look ahead is enough to notice a wall and not enough to steer around
		 * one, because the car covers the near range before it can turn.
		 *
		 * The chosen sidestep is HELD for a while instead of re-decided every frame:
		 * measured, the old code flipped between +448 and -448 from frame to frame
		 * (the swinging `diff` in the log) and the car went nowhere. The hold is
		 * dropped the moment the way it actually wants is clear again. */
		{
			/* same reason as the flee's sampling: two probes let a thin obstacle sit
			 * between them and read as open ground */
			static const int ranges[4] = { 550, 1100, 1650, 2200 };
			static const int step[6] = { 448, -448, 896, -896, 1344, -1344 };	/* nearest angle first */
			static int holdDir = -1;
			static int holdFrames = 0;
			int pdir = want;
			int vx = mine->st.n.linearVelocity[0];
			int vz = mine->st.n.linearVelocity[2];

			if ((long)vx * (long)vx + (long)vz * (long)vz > 400L * 400L)
				pdir = ratan2(vx, vz) & 0xfff;

			if (holdFrames > 0)
			{
				holdFrames--;

				/* the way it wants is clear again: stop dodging. Corridor, not centre
				 * line, so a post just off the line counts too. */
				if (MpBotCorridorClear(mine, want, ranges[1]))
					holdFrames = 0;
				else
					pdir = holdDir;		/* committed: keep going round the same side */
			}

			if (!MpBotCorridorClear(mine, pdir, ranges[1]) ||
				!MpBotCorridorClear(mine, pdir, ranges[3]))
			{
				int i, chosen = -1;

				for (i = 0; i < 6; i++)
				{
					int d = (pdir + step[i]) & 0xfff;

					if (MpBotCorridorClear(mine, d, ranges[1]) && MpBotCorridorClear(mine, d, ranges[3]))
					{
						chosen = d;
						break;
					}
				}

				if (chosen >= 0)
				{
					holdDir = chosen;
					holdFrames = MPBOT_DODGE_FRAMES;	/* commit: see the define */
					want = chosen;
				}
			}
		}

		diff = ((want - mine->hd.direction + 2048) & 4095) - 2048;	/* DIFF_ANGLES */
		adiff = (diff < 0) ? -diff : diff;

		{
			int spd = mine->hd.speed;

			if (spd < 0)
				spd = -spd;

			/* A car that has NEVER MOVED is not wedged - it is a car that has just
			 * spawned. Calling that wedged made the bot do a full handbrake donut on
			 * the spot and then drive off into whatever was behind it, which is exactly
			 * what a client was seen doing while facing the host at spawn. The wedge
			 * test only arms once the car has actually been rolling. */
			if (spd > 20)
				everMoved = 1;

			/* Wedged counts whether we are trying to go forward or just sitting
			 * there facing away: a car stopped against a wall with the peer behind
			 * it never moves, and a throttle-only test missed exactly that. */
			if (spd < 4 && everMoved)
			{
				if (++stuckFrames > MPBOT_STUCK_FRAMES)
				{
					/* WHY the recovery fired, and whether there is anything in front of the
					 * car: a wall dead ahead is the one case a reverse is actually the right
					 * move, so the line has to carry it. One line per decision, not per
					 * frame, so a pair run can be read for it. */
					int ahead = MpBotSpotClear(mine, mine->hd.direction, 1100);
					int contact = MpBotContacted(mine);

					stuckFrames = 0;
					turnDir ^= 1;

					if (!ahead || contact)
					{
						/* A wall in front, OR something the forward trace cannot see because the
						 * car is already inside it: a turn with the throttle on cannot move a car
						 * that cannot move, so back it off first - the one place a reverse is right.
						 * Bounded and non-alternating, so it is not the old shuffle. */
						backDir = turnDir;
						backFrames = MPBOT_BACK_FRAMES;

						/* and turn the OTHER way out of it. Backing out and then steering
						 * the way we were already going drives the car straight back into
						 * the wall it just left - the "same arc" bounce. */
						turnDir = !backDir;
						turnFrames = MPBOT_TURN_FRAMES + 40;
						turnPulse = MPBOT_TURN_FRAMES;

						if (gMpCtx != NULL)
							gMpCtx->jer_log(gMpCtx,
								"[mp] chase: recover - wedged (speed %d), ahead clear=%d, in contact=%d, backing out (dir %d)\n",
								mine->hd.speed, ahead, contact, backDir);
					}
					else
					{
						turnFrames = MPBOT_TURN_FRAMES + 40;
						turnPulse = MPBOT_TURN_FRAMES;

						if (gMpCtx != NULL)
							gMpCtx->jer_log(gMpCtx,
								"[mp] chase: recover - wedged (speed %d), ahead clear=%d, handbrake turn (dir %d)\n",
								mine->hd.speed, ahead, turnDir);
					}
				}
			}
			else
			{
				stuckFrames = 0;
			}
		}

		dist = (long)dx * (long)dx + (long)dz * (long)dz;

		/* FOLLOWING AT MINIMUM DISTANCE is not a reason to do anything dramatic. The
		 * chaser used to panic-turn the moment the heading error went large, which at
		 * close range happens constantly as the two cars swap sides, so the pair never
		 * settled. Hysteresis rather than a derivative: inside MPBOT_FOLLOW_IN stop and
		 * wait with the nose on them, and only take the chase up again once the gap has
		 * actually reached MPBOT_FOLLOW_OUT. */
		if (flee)
			holding = 0;
		else if (dist < MPBOT_FOLLOW_IN * MPBOT_FOLLOW_IN)
			holding = 1;
		else if (dist > MPBOT_FOLLOW_OUT * MPBOT_FOLLOW_OUT)
			holding = 0;

		if (holding)
			pad = (adiff > 96) ? ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT) : 0;
		else if (adiff > 1500)
		{
			/* The peer is behind us: TURN ROUND, never reverse. Full lock with the
			 * rear locked for the first frames brings the nose round, and the
			 * throttle then drives us out of it - a reverse moved us AWAY from
			 * them, which is what made the pair shuffle instead of meet. */
			if (turnFrames <= 0 && gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] chase: recover - peer behind (adiff %d, ahead clear=%d), handbrake turn\n",
					adiff, MpBotSpotClear(mine, mine->hd.direction, 1100));

			turnFrames = MPBOT_TURN_FRAMES + 20;
			turnPulse = MPBOT_TURN_FRAMES;
			turnDir = diff;

			pad = MpBotTurnPad(&turnFrames, &turnPulse, turnDir, mine->hd.speed);
		}
		else if (adiff > 700)
			/* Badly off line: EASE OFF and steer. Powering through a big correction is
			 * what made them bobble and slide into the scenery - but steer ALONE cannot
			 * move a car, so a chaser that was stopped or barely rolling here just pivoted
			 * on the spot and never closed the gap, which is what "it does not really
			 * target the host" looks like. Below a walking pace it turns AND goes. */
			pad = ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT) |
				(((mine->hd.speed < 40) && (mine->hd.speed > -40)) ? CAR_PAD_ACCEL : 0);
		else if (adiff > 120)
			pad = (easeOff ? 0 : CAR_PAD_ACCEL) | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
		else
			pad = easeOff ? 0 : CAR_PAD_ACCEL;	/* coasting IS the ease-off: the flee stops widening the gap while the chasers close it */

		if ((gMp.frame % 60) == 0 && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] chase: %s d=%d,%d want=%d dir=%d diff=%d pad=%#x stuck=%d\n",
				looping ? "loop" : (flee ? (easeOff ? "flee-hold" : "flee") : (fight ? "fight" : "chase")),
				dx, dz, want, mine->hd.direction, diff, pad, stuckFrames);

		return pad;
	}
}

/* ------------------------------------------------------------------ */
/* pursuit: MUTUAL chase -- both cars hunt each other                   */
/*                                                                      */
/* Both instances run the same mode. Making one of them flee kept the    */
/* pair apart for the whole run, so they never met and the car-to-car    */
/* contact was never exercised. Both hunt, so they converge and collide. */
/* ------------------------------------------------------------------ */
#define MPBOT_PROBE	2400	/* how far ahead the pathfinder looks -- long, so it sees a
				 * wall at an intersection and commits to the turn BEFORE
				 * reaching it, instead of nosing into it */
#define MPBOT_MID	1400	/* the middle range, used to tell a ROAD from a POCKET */
#define MPBOT_NEAR	650	/* ... and the near probe that stops it nosing into a wall */

/* How far ahead heading `a` is clear, as a count of ranges: 0 = blocked at the near probe,
 * 3 = clear all the way to the far probe.
 *
 * A COUNT and not a yes/no, because "clear for 650 units" (a pocket, a corner mouth, a dead
 * end) and "clear for 2400" (the road) are the difference between the fleeing host darting
 * away down the street and backing itself into a corner. Asking only "is it clear?" picked
 * whichever small deviation happened to be clear first.
 *
 * Shared by the car and the pedestrian: pass the body's position, the clearance radius it
 * needs, and its own three probe ranges. */
static int MpBotClearanceAt(int x, int y, int z, int a, int radius, const int* r)
{
	VECTOR p;
	int i;

	for (i = 0; i < 3; i++)
	{
		p.vx = x + (int)(((long)rsin(a) * r[i]) >> 12);
		p.vy = y;
		p.vz = z + (int)(((long)rcos(a) * r[i]) >> 12);

		if (!CellEmpty(&p, radius))
			return i;			/* clear up to here, blocked on this one */
	}

	return 3;
}

/* THE FEELER: a few degrees, swept smoothly with rsin, used to REFINE a chosen heading rather
 * than to steer with.
 *
 * The candidate fan works in 15-degree steps, so its answer can leave the car aimed at the edge
 * of a gap - it then noses in, stops, re-decides, and twitches: "the chase AI quickly gets
 * confused". Sweeping a few degrees either side of the best candidate on a slow sine finds
 * where the road actually is, and because the result is a HELD heading the oscillation is never
 * visible as steering. Period is a couple of seconds at 60 fps. */
#define MPBOT_FEEL	90		/* about 8 degrees */
static int MpBotFeeler(void)
{
	static unsigned long t;

	t += 1;

	return (int)(((long)rsin((int)((t * 40) & 0xfff)) * MPBOT_FEEL) >> 12);
}

/* Pick a heading for a body at (x,y,z) to move along, near `desired`, that is as
 * far clear as possible. `radius` and `r` are the body's clearance radius and its
 * three probe ranges, and `heldp` is the caller's hysteresis slot -- one per body,
 * so the car and the pedestrian keep their own chosen heading. */
static int MpBotClearHeadingAt(int x, int y, int z, int desired, int radius, const int* r, int* heldp)
{
	const int STEP = 0x1000 / 24;	/* 15 degrees -- a full fan, so a heading
					 * AROUND a wall (up to a U-turn) is
					 * findable, not just +-90 degrees */
	/* HYSTERESIS. Deciding the heading from scratch every frame made the steering
	 * FLAP: as `desired` drifted, the first clear candidate flipped between +30
	 * and -30 and the car twitched left/right. Once we have a heading, KEEP it
	 * while it is still clear and still points roughly the way we want; re-decide
	 * only when it is blocked or the goal has moved a long way off it. */
	int held = *heldp;
	int i, best = -1, bestClr = 0;

	if (held >= 0)
	{
		int clr = MpBotClearanceAt(x, y, z, held, radius, r);
		int off = ((held - desired + 2048) & 4095) - 2048;

		if (off < 0)
			off = -off;

		/* keep it while it is worth keeping: clear past the middle probe (a road, not a
		 * pocket) and still roughly the way we want to go */
		if (clr >= 2 && off < 900)
			return held;
	}

	/* Prefer the FARTHEST-clear candidate and take the nearest angle only as the tie-break.
	 * The order of these two tests is the whole "runs away down the road instead of backing
	 * into a corner" fix. */
	for (i = 0; i < 24; i++)
	{
		int k = (i + 1) / 2;
		int a = (i == 0) ? desired : ((desired + ((i & 1) ? (k * STEP) : (-k * STEP))) & 0xfff);
		int clr = MpBotClearanceAt(x, y, z, a, radius, r);

		if (clr > bestClr)
		{
			bestClr = clr;
			best = a;

			if (clr >= 3)		/* nothing can beat this */
				break;
		}
	}

	if (best < 0)
		return desired;			/* boxed in on every heading we try: keep going */

	/* refine it with the feeler, so the choice lands in the middle of the gap */
	if (bestClr < 3)
	{
		int f = MpBotFeeler();
		int a = (best + f) & 0xfff;

		if (MpBotClearanceAt(x, y, z, a, radius, r) > bestClr)
			best = a;
	}

	*heldp = best;

	return best;
}

static int MpBotClearHeading(CAR_DATA* mine, int desired)
{
	static const int r[3] = { MPBOT_NEAR, MPBOT_MID, MPBOT_PROBE };
	static int held = -1;

	return MpBotClearHeadingAt(mine->hd.where.t[0], mine->hd.where.t[1], mine->hd.where.t[2],
		desired, MPBOT_CAR_CLEAR, r, &held);
}

/* Returns 0 (coast) when there is nobody else to chase. */
static int MpBotPursuit(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* mine;
	CAR_DATA* tgt = NULL;
	static int stuckFrames, turnFrames, turnDir, turnPulse, backFrames, backDir;
	int k;

	if (me == NULL || me->carId < 0)
		return 0;

	for (k = 0; k < MP_MAX_PLAYERS; k++)
	{
		MP_PLAYER* p = &gMp.players[k];

		if (p->active && p->carId >= 0 && p->carId != me->carId)
		{
			tgt = &car_data[p->carId];
			break;
		}
	}

	if (tgt == NULL)
		return 0;

	mine = &car_data[me->carId];

	if (turnFrames > 0)
		return MpBotTurnPad(&turnFrames, &turnPulse, turnDir, mine->hd.speed);

	{
		int dx = tgt->hd.where.t[0] - mine->hd.where.t[0];
		int dz = tgt->hd.where.t[2] - mine->hd.where.t[2];
		int evade = 0;	/* MUTUAL pursuit: everyone hunts (see the note above) */
		int desired = evade ? ((ratan2(dx, dz) + 2048) & 0xfff) : (ratan2(dx, dz) & 0xfff);
		int want = MpBotClearHeading(mine, desired);
		int diff = ((want - mine->hd.direction + 2048) & 4095) - 2048;
		int adiff = (diff < 0) ? -diff : diff;
		long dist = (long)dx * (long)dx + (long)dz * (long)dz;
		int spd = mine->hd.speed;
		int pad;

		if (spd < 0)
			spd = -spd;

		/* same wedge detection as chase: a car stopped against scenery has to be
		 * turned out (never reversed); the runner wedges at least as often as the
		 * pursuer */
		/* Constant action: a wedge is cleared in a fraction of a second, not after
		 * seconds of sitting still. A stopped car is a wasted frame of the test. */
		if (spd < 3)
		{
			if (++stuckFrames > 20)
			{
				int ahead = MpBotSpotClear(mine, mine->hd.direction, 1100);

				turnFrames = MPBOT_TURN_FRAMES + 20;
				turnPulse = MPBOT_TURN_FRAMES;
				turnDir ^= 1;
				stuckFrames = 0;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] bot: %s recover - wedged (speed %d), ahead clear=%d, handbrake turn (dir %d)\n",
						evade ? "evade" : "pursue", mine->hd.speed, ahead, turnDir);
			}
		}
		else
		{
			stuckFrames = 0;
		}

		/* NO PROGRESS = TURN ROUND. A car that is moving but NOT closing (steered
		 * away by the pathfinder, or grinding along a wall the wrong way) will
		 * "drive" forever and never meet the other car. If the gap has not shrunk
		 * for a while, the answer is a decisive U-turn, not more of the same. */
		{
			static long best;
			static int noProg;

			if (dist < 300L * 300L)
			{
				best = 0;
				noProg = 0;
			}
			else if (best == 0 || dist < best - 300L * 300L)
			{
				best = dist;
				noProg = 0;
			}
			else if (++noProg > 150)
			{
				turnFrames = MPBOT_TURN_FRAMES + 35;
				turnPulse = MPBOT_TURN_FRAMES;
				turnDir ^= 1;
				best = 0;
				noProg = 0;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] bot: no progress for 150 frames (gap %d), turning round (dir %d)\n",
						(int)dist, turnDir);
			}
		}

		/* Speed on the straights, DECISIVE turns in the corners. Powering into a big
		 * heading error is what scrapes the car along the wall: it understeers,
		 * nose-first, for as long as the turn takes. So a large error BRAKES into the
		 * turn (the move that replaces scraping along the wall), a medium one coasts
		 * through it, and only a roughly aligned car gets full throttle. */
		if (adiff > 1200)
		{
			/* a real U-turn: turn hard, with the handbrake for the first frames,
			 * and drive out of it. Never a reverse - see the note on MpBotTurnPad. */
			if (turnFrames <= 0 && gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] bot: recover - peer behind (adiff %d, ahead clear=%d), handbrake turn\n",
					adiff, MpBotSpotClear(mine, mine->hd.direction, 1100));

			turnFrames = MPBOT_TURN_FRAMES + 20;
			turnPulse = MPBOT_TURN_FRAMES;
			turnDir = diff;

			pad = MpBotTurnPad(&turnFrames, &turnPulse, turnDir, spd);
		}
		else if (adiff > 420)
			/* a corner. NEVER steer-only: a stationary car cannot steer, and the
			 * old steer-only pad left the bot sitting still "thinking" for
			 * seconds whenever it came to rest facing the wrong way. */
			pad = CAR_PAD_ACCEL | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
		else if (adiff > 70)
			pad = CAR_PAD_ACCEL | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
		else
			pad = CAR_PAD_ACCEL;

		/* a car that has STOPPED always gets the throttle back, so it never idles
		 * in place. Nothing here brakes any more: a turn-around drives out of the
		 * corner under power, and a handbrake pad already carries the throttle. */
		if (spd < 3)
			pad |= CAR_PAD_ACCEL;

		/* the pursuer keeps the power on when it is closing, so the collision is
		 * actually tested; the runner never relents either */
		if (!evade && dist < 400L * 400L)
			pad |= CAR_PAD_ACCEL;

		if ((gMp.frame % 60) == 0 && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] bot: %s d=%d,%d want=%d dir=%d diff=%d pad=%#x stuck=%d\n",
				evade ? "evade" : "pursue", dx, dz, want, mine->hd.direction, diff, pad, stuckFrames);

		return pad;
	}
}

/* ------------------------------------------------------------------ */
/* ON FOOT: the same steering, for Tanner                               */
/*                                                                    */
/* The engine gives Tanner TANK controls (pad.h: TANNER_PAD_GOFORWARD /  */
/* GOBACK turn him on the spot, TANNER_PAD_TURNLEFT / TURNRIGHT rotate   */
/* him), which is close enough to a car that the SAME driving logic      */
/* works on him: probe the scenery with the engine's own CellEmpty, pick  */
/* a clear heading, steer toward it. Only two things change -- the body   */
/* whose position and heading are read (our pedestrian, not our car) and  */
/* the pad that comes out. That is what gives an on-foot test run some    */
/* MOTION instead of a Tanner standing at the spawn point, which is all   */
/* the on-foot path could be tested with before.                          */
/*                                                                    */
/* He walks at the nearest other player -- their car if they are driving  */
/* one, else their stand-in -- and presses ACTION when he gets there, so  */
/* the whole get-out -> walk -> get-back-in loop runs hands-free.         */
/* ------------------------------------------------------------------ */
#define MPBOT_PED_NEAR	170
#define MPBOT_PED_MID	380
#define MPBOT_PED_PROBE	700

int MpBotTannerPad(void)
{
	static const int r[3] = { MPBOT_PED_NEAR, MPBOT_PED_MID, MPBOT_PED_PROBE };
	static int held = -1;
	static int stuckFrames, backFrames;
	LPPEDESTRIAN ped = player[0].pPed;
	MP_PLAYER* me = MpLocalPlayer();
	int x, y, z, dir, tgtx = 0, tgtz = 0, have = 0, k;
	int diff = 0, adiff, pad = 0;
	long dist = 0;

	/* Not on foot (or not our ped): stand down and forget the chosen heading,
	 * so the next time he gets out he starts fresh rather than committed to a
	 * direction chosen in another part of the map. */
	if (ped == NULL || me == NULL || me->carId >= 0)
	{
		held = -1;
		stuckFrames = 0;
		backFrames = 0;
		return 0;
	}

	for (k = 0; k < MP_MAX_PLAYERS; k++)
	{
		MP_PLAYER* p = &gMp.players[k];

		if (!p->active || p->isLocal)
			continue;

		if (p->carId >= 0 && p->carId < MAX_CARS)
		{
			tgtx = car_data[p->carId].hd.where.t[0];
			tgtz = car_data[p->carId].hd.where.t[2];
			have = 1;
			break;
		}

		if (p->ped != NULL)
		{
			LPPEDESTRIAN other = (LPPEDESTRIAN)p->ped;

			tgtx = other->position.vx;
			tgtz = other->position.vz;
			have = 1;
			break;
		}
	}

	x = ped->position.vx;
	y = ped->position.vy;
	z = ped->position.vz;
	dir = ped->dir.vy & 0xfff;

	{
		int desired;

		if (have)
		{
			int dx = tgtx - x;
			int dz = tgtz - z;

			dist = (long)dx * (long)dx + (long)dz * (long)dz;
			desired = ratan2(dx, dz) & 0xfff;
		}
		else
		{
			/* nobody to walk at: keep turning the heading we came in with, so
			 * he walks a curve rather than standing still */
			desired = (dir + 200) & 0xfff;
		}

		{
			int want = MpBotClearHeadingAt(x, y, z, desired, MPBOT_PED_CLEAR, r, &held);

			diff = ((want - dir + 2048) & 4095) - 2048;
		}
	}

	adiff = (diff < 0) ? -diff : diff;

	/* A walker that has stopped is wedged (a wall, a fence, a kerb): back up
	 * for a moment, which is the one thing that frees a tank-steered body. */
	if (ped->speed == 0)
	{
		if (++stuckFrames > 40)
		{
			backFrames = 20;
			stuckFrames = 0;
			held = -1;
		}
	}
	else
	{
		stuckFrames = 0;
	}

	if (backFrames > 0)
	{
		backFrames--;
		return TANNER_PAD_GOBACK;
	}

	/* Tank controls: he turns FASTER while running, and PedUserRunner takes the
	 * forward bit and the turn bit together, so hold both. Never steer-only -- a
	 * stationary Tanner turns slowly and, worse, produces no motion to replicate,
	 * which is the whole point of driving him. */
	if (adiff > 80)
		pad |= (diff > 0) ? TANNER_PAD_TURNLEFT : TANNER_PAD_TURNRIGHT;

	pad |= TANNER_PAD_GOFORWARD;

	/* Standing at one of their cars: press ACTION, which is how the engine's own
	 * ped mechanic gets in. Out and back in without a human. */
	if (have && dist < 300L * 300L)
		pad |= TANNER_PAD_ACTION;

	if ((gMp.frame % 60) == 0 && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] bot: tanner at %d,%d dir=%d diff=%d spd=%d pad=%#x\n",
			x, z, dir, diff, ped->speed, pad);

	return pad;
}

int MpBotEnabled(void)
{
	return MpBotMode() != 0;
}

/* ------------------------------------------------------------------ */
/* DRIVING: the shared brain                                           */
/*                                                                     */
/* Everything above this point decides WHERE to go, and used to steer  */
/* straight at a heading. This decides HOW to get there, and it is the */
/* part that was missing: a heading is not a route, so a car wedged on */
/* the first building and then bounced off it in the same arc, and a   */
/* pair shuffled on the spot instead of chasing across the map.        */
/*                                                                     */
/* The world is re-probed and the route re-planned on an interval -    */
/* not every frame, or the car would re-decide its way down a road -   */
/* and between plans it follows waypoints.                             */
/* ------------------------------------------------------------------ */

#define MPBOT_PLAN_MS		700	/* how often a route is refreshed */
#define MPBOT_PLAN_GOAL_MOVE	700	/* world units the goal may drift before a replan */
#define MPBOT_AIM_AHEAD		900	/* steer at the first waypoint at least this far out */
#define MPBOT_PUSH_MS		900	/* shove at something invisible this long before giving up */
#define MPBOT_CONTACT_CLEAR	1100	/* how far ahead "is anything in front" is asked */

typedef struct MPBOT_AI
{
	AIMAP	map;
	AIPATH	path;
	int	plannedAt;	/* MpBotNowMs() when this route was made */
	int	goalX, goalZ;	/* what it was made for */
	int	plans;		/* replans, for the log */
	int	incomplete;	/* the last plan could not reach its goal */
	int	pushing;	/* in contact with something the probes cannot see */
	int	pushAt;		/* ...since when */
} MPBOT_AI;

/* Milliseconds since the session started, from the frame count: the sim already counts
 * frames, and a second clock is one more thing to get wrong. */
static int MpBotNowMs(void)
{
	return (int)((long)gMp.frame * 1000L / 60L);
}

/* MP_BOT_DRAW=1 shows the AI's thinking on the HUD, so the pathing can be WATCHED
 * happening rather than reconstructed from the log afterwards. Resolved once, like every
 * other lever here, and off unless asked for. The SDK has no world-space line primitive,
 * so this is a live readout - goal, route shape, aim point, gap, search cost - rather than
 * lines drawn on the ground; drawing the route in the world wants a new engine hook. */
static int MpBotDraw(void)
{
	static int on = -1;

	if (on < 0)
	{
		const char* v = getenv("MP_BOT_DRAW");

		on = (v != NULL && v[0] != '0') ? 1 : 0;
	}

	return on;
}

static void MpBotDrawPlan(const char* what, const AIGOAL* goal, const struct MPBOT_AI* ai,
	int aimX, int aimZ, long gap2, unsigned int pad);

/* The engine's own world-space debug line (DebugOverlay.obj, always linked - the collision
 * and civ-AI debug views draw with exactly this). Raw world frame; colour components are
 * PSX-style 0..250. */
extern void Debug_AddLine(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);

/* The route, drawn IN THE WORLD: red for the route, yellow for the goal, green for where
 * the car is aiming this frame. Drawn every frame while MP_BOT_DRAW is on, because a line
 * that only appears a few times a second reads as a flicker rather than as a route. */
static void MpBotDrawRoute(const struct MPBOT_AI* ai, CAR_DATA* mine, const AIGOAL* goal,
	int aimX, int aimZ)
{
	CVECTOR cRoute = { 250, 60, 60 };
	CVECTOR cGoal = { 250, 250, 60 };
	CVECTOR cAim = { 60, 250, 60 };
	VECTOR a, b;
	int k, y;
	const int CROSS = 600;

	if (!MpBotDraw() || mine == NULL)
		return;

	/* lifted off the ground so the line does not z-fight the road it is drawn over, and at
	 * the car's own height, which is what the engine's road debug does too */
	y = mine->hd.where.t[1] + 60;

	for (k = 1; k < ai->path.waypoints; k++)
	{
		a.vx = ai->path.wx[k - 1];
		a.vy = y;
		a.vz = ai->path.wz[k - 1];

		b.vx = ai->path.wx[k];
		b.vy = y;
		b.vz = ai->path.wz[k];

		Debug_AddLine(a, b, cRoute);
	}

	/* where we are aiming, so it is visible that the aim leads the car */
	a.vx = mine->hd.where.t[0];
	a.vy = y;
	a.vz = mine->hd.where.t[2];

	b.vx = aimX;
	b.vy = y;
	b.vz = aimZ;

	Debug_AddLine(a, b, cAim);

	/* the goal as a cross on the ground: a single distant line is hard to find */
	a.vx = goal->x - CROSS;
	a.vy = y;
	a.vz = goal->z;

	b.vx = goal->x + CROSS;
	b.vy = y;
	b.vz = goal->z;

	Debug_AddLine(a, b, cGoal);

	a.vx = goal->x;
	a.vz = goal->z - CROSS;
	b.vx = goal->x;
	b.vz = goal->z + CROSS;

	Debug_AddLine(a, b, cGoal);
}

/* Plan a route to a world point, or keep the one we have. Returns 1 when there is a route
 * to follow at all. */
static int MpBotRouteTo(MPBOT_AI* ai, CAR_DATA* mine, int goalX, int goalZ, int force)
{
	int mx = goalX - ai->goalX;
	int mz = goalZ - ai->goalZ;

	if (mx < 0) mx = -mx;
	if (mz < 0) mz = -mz;

	if (!force && ai->path.waypoints > 0 &&
		(MpBotNowMs() - ai->plannedAt) < MPBOT_PLAN_MS &&
		mx < MPBOT_PLAN_GOAL_MOVE && mz < MPBOT_PLAN_GOAL_MOVE)
		return 1;		/* the route we have is still the right one */

	ai->plannedAt = MpBotNowMs();
	ai->goalX = goalX;
	ai->goalZ = goalZ;
	ai->plans++;

	/* the world is re-probed with the plan, centred on where the car is NOW */
	AiMapBuild(&ai->map, mine->hd.where.t[0], mine->hd.where.t[1], mine->hd.where.t[2]);

	if (!AiMapValid(&ai->map))
	{
		ai->path.waypoints = 0;
		return 0;		/* no level: the caller falls back to what it did before */
	}

	ai->incomplete = !AiStarPlan(&ai->map, mine->hd.where.t[0], mine->hd.where.t[2],
		goalX, goalZ, &ai->path);

	return ai->path.waypoints > 0;
}

/* The point to steer at: the furthest waypoint within the aim distance, so the car drives
 * THROUGH a corner instead of crawling to each turning point. */
static int MpBotAimPoint(const MPBOT_AI* ai, CAR_DATA* mine, int* outX, int* outZ)
{
	int k, best = 1;

	if (ai->path.waypoints <= 0)
		return 0;

	/* waypoint 0 is where the plan started, so the first thing to drive at is 1 - unless
	 * that is all there was, in which case it is the goal itself */
	if (ai->path.waypoints == 1)
		best = 0;

	for (k = 1; k < ai->path.waypoints; k++)
	{
		int dx = ai->path.wx[k] - mine->hd.where.t[0];
		int dz = ai->path.wz[k] - mine->hd.where.t[2];

		best = k;

		if ((long)dx * (long)dx + (long)dz * (long)dz >= (long)MPBOT_AIM_AHEAD * MPBOT_AIM_AHEAD)
			break;
	}

	*outX = ai->path.wx[best];
	*outZ = ai->path.wz[best];

	return 1;
}

/* Steer at a POINT with the same thresholds the heading code uses, because it is the same
 * car and the same handling: never reverse to correct, handbrake only while rolling, and
 * keep the throttle on when badly off line or a slow car can never turn. */
static int MpBotSteerToPoint(CAR_DATA* mine, int aimX, int aimZ, int* turnFrames, int* turnPulse,
	int* turnDir, int* backFrames, int* backDir)
{
	int dx = aimX - mine->hd.where.t[0];
	int dz = aimZ - mine->hd.where.t[2];
	int want = ratan2(dx, dz) & 0xfff;
	int diff = ((want - mine->hd.direction + 2048) & 0xfff) - 2048;
	int adiff = (diff < 0) ? -diff : diff;
	int spd = mine->hd.speed;

	if (spd < 0) spd = -spd;

	if (*backFrames > 0)
	{
		(*backFrames)--;
		return CAR_PAD_BRAKE | (*backDir ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	if (*turnFrames > 0)
		return MpBotTurnPad(turnFrames, turnPulse, *turnDir, mine->hd.speed);

	if (adiff > 1500)
	{
		/* the target is behind us: turn round, never reverse - and note that with a
		 * route this happens at the START of a leg, not every time the car passes
		 * something, which is what the heading version got wrong */
		*turnFrames = MPBOT_TURN_FRAMES + 20;
		*turnPulse = MPBOT_TURN_FRAMES;
		*turnDir = diff;

		return MpBotTurnPad(turnFrames, turnPulse, *turnDir, mine->hd.speed);
	}

	if (adiff > 700)
		/* badly off line: steer, and keep the throttle on unless already moving well */
		return ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT) | ((spd < 40) ? CAR_PAD_ACCEL : 0);

	return CAR_PAD_ACCEL | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
}

/* ------------------------------------------------------------------ */
/* THE CONTACT POLICY                                                  */
/*                                                                     */
/* One place, three answers. Every ad-hoc "back out" / "spin round" /  */
/* "wedged, ahead clear=N" branch the bots used to have is one of      */
/* these:                                                              */
/*                                                                     */
/*   DRIVE  nothing is in the way: follow the route                    */
/*   TURN   something the engine CAN see is in front: turn out of it   */
/*   PUSH   stopped, in contact, and nothing visible ahead: KEEP THE    */
/*          THROTTLE ON. CellEmpty skips MODEL_FLAG_SMASHABLE (and     */
/*          chairs) by design - objcoll.c:49 - so a fence, a barrel or */
/*          a bollard is invisible to every probe we have, while the   */
/*          physics stops the car dead on it. A player drives through  */
/*          those; so do we. Backing away from something the engine    */
/*          says is not there is exactly how a car ends up stuck on a  */
/*          fence for the rest of the match.                           */
/*                                                                     */
/* A real wall lands in PUSH too, because no probe can see the one the */
/* car is already inside, so PUSH is BOUNDED: after MPBOT_PUSH_MS of   */
/* shoving, back out. That is the one reverse the bots use, and it does */
/* not alternate.                                                      */
/* ------------------------------------------------------------------ */

/* Returns the pad to use, or 0 when the car is free to drive its route. */
static int MpBotContact(MPBOT_AI* ai, CAR_DATA* mine, int* turnFrames, int* turnPulse,
	int* turnDir, int* backFrames, int* backDir, int* stuckFrames)
{
	int spd = mine->hd.speed;

	if (spd < 0) spd = -spd;

	if (spd >= 4)
	{
		*stuckFrames = 0;
		ai->pushing = 0;
		return 0;			/* moving: nothing to do here */
	}

	if (++(*stuckFrames) <= 24)
		return 0;			/* not stopped long enough to be a wedge */

	*stuckFrames = 0;

	{
		int ahead = MpBotSpotClear(mine, mine->hd.direction, MPBOT_CONTACT_CLEAR);
		int contact = MpBotContacted(mine);
		int pushMs = MpBotNowMs() - ai->pushAt;

		if (ahead && contact)
		{
			/* in contact with something no probe can see: shove it */
			if (!ai->pushing)
			{
				ai->pushing = 1;
				ai->pushAt = MpBotNowMs();

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] catmouse: in contact with something the probes cannot see (a fence?) - pushing through\n");
			}
			else if (pushMs < MPBOT_PUSH_MS)
			{
				return CAR_PAD_ACCEL;	/* still shoving */
			}
			else
			{
				/* it did not give way, so it is a wall the probe cannot see: the one case
				 * a bounded reverse is right, and it turns out the OTHER way so the car
				 * does not drive back into what it just left */
				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] catmouse: pushed for %d ms and it did not give - backing out\n", pushMs);

				ai->pushing = 0;
				*backDir = (mine->hd.direction >> 6) & 1;
				*backFrames = MPBOT_BACK_FRAMES;
				*turnDir = !(*backDir);
				*turnFrames = MPBOT_TURN_FRAMES + 40;
				*turnPulse = MPBOT_TURN_FRAMES;

				return CAR_PAD_BRAKE | (*backDir ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
			}
		}

		/* something visible is in front, or we are not in contact at all: turn out */
		*turnDir = (mine->hd.direction >> 6) & 1;
		*turnFrames = MPBOT_TURN_FRAMES + 40;
		*turnPulse = MPBOT_TURN_FRAMES;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] catmouse: wedged (speed %d, ahead clear=%d, contact=%d) - turning out (dir %d)\n",
				mine->hd.speed, ahead, contact, *turnDir);

		return MpBotTurnPad(turnFrames, turnPulse, *turnDir, mine->hd.speed);
	}
}

/* ------------------------------------------------------------------ */
/* CAT AND MOUSE: the pair, actually driving                           */
/*                                                                     */
/* The same roles as chase - the host runs, the joiners chase - but    */
/* the MOUSE picks a place to run TO (far from the cat, preferring the */
/* road, with room to move) and the CAT plans to where the mouse is.   */
/* Both follow a route instead of a heading, which is what turns "both */
/* cars shuffle" into a pursuit.                                       */
/*                                                                     */
/* If the world cannot be sampled, or no route exists at all, the car  */
/* is handed straight back to the old heading logic: the last rung of  */
/* the ladder is the behaviour that was already there, unchanged.      */
/* ------------------------------------------------------------------ */
static int MpBotCatMouse(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	static MPBOT_AI ai;
	static int stuckFrames, turnFrames, turnPulse, turnDir, backFrames, backDir;
	CAR_DATA* mine;
	CAR_DATA* tgt = NULL;
	int k, pad, aimX, aimZ;
	AIGOAL goal;
	int isMouse;

	if (me == NULL || me->carId < 0)
		return 0;

	for (k = 0; k < MP_MAX_PLAYERS; k++)
	{
		MP_PLAYER* p = &gMp.players[k];

		if (p == me || p->carId < 0)
			continue;

		if (!p->connected)
			continue;

		tgt = &car_data[p->carId];
		break;
	}

	if (tgt == NULL)
		return 0;

	mine = &car_data[me->carId];

	if (!AiMapWorldLoaded())
		return MpBotChase(0);		/* no level: the old logic, unchanged */

	isMouse = MpIsHost();

	/* the world has to exist before a goal can be chosen from it */
	if (ai.map.sampled == 0)
		AiMapBuild(&ai.map, mine->hd.where.t[0], mine->hd.where.t[1], mine->hd.where.t[2]);

	if (!AiMapValid(&ai.map))
		return MpBotChase(0);

	if (isMouse)
	{
		int onRoad = JerRoadAt(mine->hd.where.t[0], mine->hd.where.t[1], mine->hd.where.t[2]);

		if (!onRoad)
		{
			/* GET BACK ON THE ROAD FIRST. Running for the furthest open point scored
			 * well between houses - far from the cat, and "open" to the probe - and the
			 * car then wedged in a gap it could not get out of, which is the "turns left
			 * between the houses instead of right, back to the road" complaint. A mouse
			 * that is not on a road has exactly one sensible destination: the nearest road
			 * it can actually reach. The running starts once it is back on one. */
			if (!AiLocalGoal(&ai.map, mine->hd.where.t[0], mine->hd.where.t[2], &goal))
				return MpBotChase(0);
		}
		else if (!AiLocalFleeGoal(&ai.map, mine->hd.where.t[0], mine->hd.where.t[2],
				tgt->hd.where.t[0], tgt->hd.where.t[2], &goal))
		{
			/* nothing in the ring is worth running to: the nearest road, or failing that
			 * the most open ground, will do */
			if (!AiLocalGoal(&ai.map, mine->hd.where.t[0], mine->hd.where.t[2], &goal))
				return MpBotChase(0);
		}
	}
	else
	{
		/* the cat drives at the mouse. When the route cannot reach it, the pathfinder
		 * hands back the best partial one - heading the right way down the road - and
		 * that is still better than pointing at a heading and hoping. */
		goal.x = tgt->hd.where.t[0];
		goal.z = tgt->hd.where.t[2];
		goal.kind = AIGOAL_OPEN;
		goal.samples = 0;
		goal.clearance = 0;
	}

	if (!MpBotRouteTo(&ai, mine, goal.x, goal.z, 0))
		return MpBotChase(0);		/* no route: the old logic, unchanged */

	pad = MpBotContact(&ai, mine, &turnFrames, &turnPulse, &turnDir, &backFrames, &backDir, &stuckFrames);

	if (pad != 0)
		return pad;

	if (!MpBotAimPoint(&ai, mine, &aimX, &aimZ))
		return MpBotChase(0);

	pad = MpBotSteerToPoint(mine, aimX, aimZ, &turnFrames, &turnPulse, &turnDir, &backFrames, &backDir);

	/* the plan log, rate-limited: a run should read as a story, not a wall of lines */
	if ((gMp.frame % 60) == 0 && gMpCtx != NULL)
	{
		gMpCtx->jer_log(gMpCtx,
			"[mp] catmouse: %s goal=(%d,%d) %s waypoints=%d aim=(%d,%d) age=%dms expanded=%d gap2=%ld pad=%#x\n",
			isMouse ? "MOUSE running to" : "CAT driving to",
			goal.x, goal.z,
			(goal.kind == AIGOAL_ROAD) ? "road" : "open",
			ai.path.waypoints, aimX, aimZ,
			MpBotNowMs() - ai.plannedAt, ai.path.expanded,
			/* the GAP, squared: no square root in the frame path is worth the instruction */
			(long)(tgt->hd.where.t[0] - mine->hd.where.t[0]) * (tgt->hd.where.t[0] - mine->hd.where.t[0]) +
			(long)(tgt->hd.where.t[2] - mine->hd.where.t[2]) * (tgt->hd.where.t[2] - mine->hd.where.t[2]),
			pad);
	}

	{
		/* the live readout: the route shape and where the car is aiming, a few times a
		 * second, straight onto the HUD - and the route itself drawn in the world */
		long rgx = tgt->hd.where.t[0] - mine->hd.where.t[0];
		long rgz = tgt->hd.where.t[2] - mine->hd.where.t[2];

		MpBotDrawPlan(isMouse ? "MOUSE" : "CAT", &goal, &ai, aimX, aimZ,
			rgx * rgx + rgz * rgz, pad);

		MpBotDrawRoute(&ai, mine, &goal, aimX, aimZ);
	}

	return pad;
}

static void MpBotDrawPlan(const char* what, const AIGOAL* goal, const struct MPBOT_AI* ai,
	int aimX, int aimZ, long gap2, unsigned int pad)
{
	char line[220];
	int k, j;

	if (!MpBotDraw())
		return;

	if ((gMp.frame % 20) != 0)
		return;		/* about three times a second: readable, not a wall */

	j = snprintf(line, sizeof(line), "%s GOAL %d,%d %s | wp %d:",
		what, goal->x, goal->z, (goal->kind == AIGOAL_ROAD) ? "ROAD" : "open",
		ai->path.waypoints);

	for (k = 0; k < ai->path.waypoints && j > 0 && j < (int)sizeof(line) - 26; k++)
	{
		int n = snprintf(line + j, sizeof(line) - j, " (%d,%d)",
			ai->path.wx[k], ai->path.wz[k]);

		if (n < 0)
			break;

		j += n;
	}

	if (j > 0 && j < (int)sizeof(line) - 80)
		snprintf(line + j, sizeof(line) - j, " | aim %d,%d gap %d exp %d pad %#x",
			aimX, aimZ, (int)gap2, ai->path.expanded, pad);

	jer_hud_message(line, 30);
}

int MpBotPadForLocalCar(void)
{
	switch (MpBotMode())
	{
	case 4:  return MpBotPursuit();
	case 5:  return MpBotCatMouse();
	case 3:  return MpBotChase(1);
	case 2:  return MpBotChase(0);
	case 1:  return MpBotCanned();
	default: return 0;
	}
}
