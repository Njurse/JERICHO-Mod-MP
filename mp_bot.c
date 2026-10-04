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
static int gBotGapEase = 2500;
static int gBotGapTurnback = 5000;
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
 * 1 (random), 2 (chase), 3 (fight) or 4 (pursuit: host hunts, joiner runs). */
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

static int MpBotFleeWant(CAR_DATA* mine, int away)
{
	static int lastLogged = -1;
	int best = away, bestScore = -9999, bestDepth = 0, i;

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
	}

	/* Make the scan visible: without this there is no way to tell a fleer that is
	 * choosing open ground from one that is not. Only when it differs from straight
	 * back, and only on change, so a run is readable rather than a wall of lines. */
	if (best != away && best != lastLogged && gMpCtx != NULL)
	{
		lastLogged = best;
		gMpCtx->jer_log(gMpCtx,
			"[mp] chase: flee scan - straight back is not the way out, heading %d of 4096 (open %d range(s)%s)\n",
			best, bestDepth, bestDepth >= MPBOT_FLEE_ROAD ? ", a road" : "");
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
	static int stuckFrames, turnFrames, turnDir, turnPulse, backFrames, backDir, holding, everMoved;
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

					stuckFrames = 0;
					turnDir ^= 1;

					if (!ahead)
					{
						/* A wall in front: a turn with the throttle on cannot move a car
						 * that cannot move, so back it off the wall first - the one place
						 * a reverse is right. Bounded and non-alternating, so it is not
						 * the old shuffle. */
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
								"[mp] chase: recover - wedged (speed %d), ahead clear=0, backing out (dir %d)\n",
								mine->hd.speed, backDir);
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
			/* Badly off line: EASE OFF and steer. Powering through a big
			 * correction is what made them bobble and slide into the scenery. */
			pad = (diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT;
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

int MpBotPadForLocalCar(void)
{
	switch (MpBotMode())
	{
	case 4:  return MpBotPursuit();
	case 3:  return MpBotChase(1);
	case 2:  return MpBotChase(0);
	case 1:  return MpBotCanned();
	default: return 0;
	}
}
