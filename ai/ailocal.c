/*
 * ailocal.c -- one flood of the window, two rungs of the answer. See ailocal.h.
 *
 * Static scratch rather than stack: this runs from the game loop, the arrays are sized
 * to the grid, and the module is single-threaded (the same reasoning as aistar's node
 * pool). Nothing is allocated and nothing grows.
 */

#include <string.h>

#include "ai/aimap.h"
#include "ai/ailocal.h"

#define AILOCAL_NODES	(AIMAP_SIZE * AIMAP_SIZE)

static unsigned char sSeen[AILOCAL_NODES];
static int sDist[AILOCAL_NODES];
static int sQueue[AILOCAL_NODES];

static void AiLocalFail(AIGOAL* out)
{
	if (out == NULL)
		return;

	out->kind = AIGOAL_NONE;
	out->x = 0;
	out->z = 0;
	out->samples = 0;
	out->clearance = 0;
}

/* ------------------------------------------------------------------ */
/* One flood of the window, shared by every rung below                 */
/* ------------------------------------------------------------------ */
/* Breadth-first from the car's own sample, over the samples a car can actually drive
 * between, writing sSeen[]/sDist[] and the BFS-ordered sQueue[] (so the queue is also
 * "nearest first"). Both rungs read the same result, which is the point: "reachable"
 * has to mean ONE thing, or the road rung and the flee rung will disagree about the map.
 *
 * The corner-cut rule is not decoration: a diagonal step between two blocked samples is
 * a gap narrower than a car, and letting the flood leak through one would call a road
 * "reachable" that the car cannot drive to - the same class of lie the old line-of-sight
 * gate told in the opposite direction. */
static int AiLocalFlood(const AIMAP* map, int sx, int sz)
{
	int head = 0, tail = 0;
	int start = sz * AIMAP_SIZE + sx;
	int dx, dz;

	memset(sSeen, 0, sizeof(sSeen));

	/* the start must be a real sample: every caller feeds this AiMapSampleIndex output that
	 * AiMapNearestOpen accepted, but the flood does not trust that - an out-of-range start
	 * would index sSeen out of bounds on the very first write. */
	if (sx < 0 || sz < 0 || sx >= AIMAP_SIZE || sz >= AIMAP_SIZE)
		return 0;

	sSeen[start] = 1;
	sDist[start] = 0;
	sQueue[tail++] = start;

	while (head < tail)
	{
		int cur = sQueue[head++];
		int cx = cur % AIMAP_SIZE;
		int cz = cur / AIMAP_SIZE;
		int d = sDist[cur];

		for (dz = -1; dz <= 1; dz++)
		{
			for (dx = -1; dx <= 1; dx++)
			{
				int nx, nz, n;

				if (dx == 0 && dz == 0)
					continue;

				nx = cx + dx;
				nz = cz + dz;

				if (nx < 0 || nz < 0 || nx >= AIMAP_SIZE || nz >= AIMAP_SIZE)
					continue;

				n = nz * AIMAP_SIZE + nx;

				if (sSeen[n])
					continue;

				if (AiMapBlocked(map, nx, nz))
					continue;

				if (dx != 0 && dz != 0 &&
					(AiMapBlocked(map, cx + dx, cz) || AiMapBlocked(map, cx, cz + dz)))
					continue;

				sSeen[n] = 1;
				sDist[n] = d + 1;
				sQueue[tail++] = n;
			}
		}
	}

	return tail;
}

int AiLocalGoal(const AIMAP* map, int fromX, int fromZ, AIGOAL* out)
{
	int six, siz, q, reached;
	int bestClear = -1, bestIx = -1, bestIz = -1, bestRing = 0;

	if (out == NULL)
		return 0;

	AiLocalFail(out);

	if (!AiMapValid(map))
		return 0;

	if (!AiMapSampleIndex(map, fromX, fromZ, &six, &siz))
		return 0;

	/* the car is very often inside a "blocked" sample - that is what a probe radius the
	 * width of a car does when the car is near a wall - so walk out before flooding */
	if (!AiMapNearestOpen(map, six, siz, &six, &siz))
		return 0;

	reached = AiLocalFlood(map, six, siz);

	for (q = 0; q < reached; q++)
	{
		int cur = sQueue[q];
		int cx = cur % AIMAP_SIZE;
		int cz = cur / AIMAP_SIZE;
		int d = sDist[cur];

		/* RUNG 1. Breadth-first, so the first road sample reached is the nearest
		 * reachable one - nearest along the ground the car can drive, not as the crow
		 * flies, which is the whole reason for flooding rather than looking around. */
		if (AiMapRoad(map, cx, cz))
		{
			AiMapWorld(map, cx, cz, &out->x, &out->z);
			out->kind = AIGOAL_ROAD;
			out->samples = d;
			out->clearance = AiMapClearance(map, cx, cz);

			return 1;
		}

		/* RUNG 2, gathered as we go: the most open ground, and (because BFS) the
		 * nearest of those when several are equally open. Strictly greater, so the
		 * first one seen wins a tie. */
		if (AiMapClearance(map, cx, cz) > bestClear)
		{
			bestClear = AiMapClearance(map, cx, cz);
			bestIx = cx;
			bestIz = cz;
			bestRing = d;
		}
	}

	/* No road is reachable from here. Room to move still beats standing still, so the
	 * most open ground the flood could reach is the answer - and if the road we can see
	 * is behind a wall, this is where we find that out rather than driving at the wall. */
	if (bestIx >= 0)
	{
		AiMapWorld(map, bestIx, bestIz, &out->x, &out->z);
		out->kind = AIGOAL_OPEN;
		out->samples = bestRing;
		out->clearance = bestClear;

		return 1;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* The other question the local search answers: where to RUN to        */
/* ------------------------------------------------------------------ */

/* How far out a flee goal is looked for, in samples. Far enough to be a real
 * destination, near enough to be reachable in one plan - and it scales with the grid, so
 * widening the window widens the running too. 16 x AIMAP_STEP is about 8,000 world units,
 * roughly four map cells: a cross-town destination rather than the next junction. */
#define AILOCAL_FLEE_RING	16

/* The road bonus has to stay COMPARABLE TO THE DISTANCE RANGE, or it is either drowned
 * (too small: the flee drifts back off the road) or absolute (too big: the flee will
 * drive straight AT the threat to reach a road). The distance term is away/4 over a ring
 * of AILOCAL_FLEE_RING samples, i.e. 0..~128, plus the clearance part - so 120 is most of
 * that range: a road wins unless it is at the very near edge of the ring, which is what
 * "stay on the road network" wants. */
#define AILOCAL_ROAD_BONUS	120

int AiLocalFleeGoal(const AIMAP* map, int fromX, int fromZ, int threatX, int threatZ, AIGOAL* out)
{
	int cix, ciz, tix, tiz, ix, iz, q, reached, best = -1, bestScore = 0, bestClear = 0;

	if (out == NULL)
		return 0;

	AiLocalFail(out);

	if (!AiMapValid(map))
		return 0;

	if (!AiMapSampleIndex(map, fromX, fromZ, &cix, &ciz))
		return 0;

	if (!AiMapNearestOpen(map, cix, ciz, &cix, &ciz))
		return 0;

	/* the threat is a point to run FROM and it may be off the window (the cat is further
	 * away than we can see): then treat it as here, which makes "away" mean "the far side
	 * of the window" and still gives the flee a direction */
	if (!AiMapSampleIndex(map, threatX, threatZ, &tix, &tiz))
	{
		tix = cix;
		tiz = ciz;
	}

	/* REACHABILITY, not line of sight. The gate here used to be AiStarLineClear, which
	 * asks "is the straight line to this point clear" - so a road reachable only AROUND
	 * A CORNER was rejected outright, and the mouse could not drive the road network it
	 * was standing on. That is the "it does not follow the road" symptom: the flee could
	 * only ever pick something it could see in a straight line. The flood answers "can
	 * the car actually drive there" for every candidate at once, and it is the SAME flood
	 * AiLocalGoal uses, so the road rung and the flee rung agree about the map. */
	reached = AiLocalFlood(map, cix, ciz);

	for (q = 0; q < reached; q++)
	{
		int score, dx, dz;
		int cur = sQueue[q];
		long away;

		ix = cur % AIMAP_SIZE;
		iz = cur / AIMAP_SIZE;

		dx = ix - cix;
		dz = iz - ciz;

		if (dx < 0) dx = -dx;
		if (dz < 0) dz = -dz;

		if (dx < AILOCAL_FLEE_RING / 2 && dz < AILOCAL_FLEE_RING / 2)
			continue;	/* it has to be somewhere worth going: near-here is not a flee */

		if (dx > AILOCAL_FLEE_RING || dz > AILOCAL_FLEE_RING)
			continue;	/* too far to be one plan's worth of driving */

		/* how far from the threat, in samples */
		{
			int tdx = ix - tix;
			int tdz = iz - tiz;

			away = (long)tdx * (long)tdx + (long)tdz * (long)tdz;
		}

		score = (int)(away / 4);

		if (AiMapRoad(map, ix, iz))
			score += AILOCAL_ROAD_BONUS;	/* see the define: it must not be drowned by distance */

		score += AiMapClearance(map, ix, iz) * 2;

		if (best < 0 || score > bestScore)
		{
			bestScore = score;
			best = cur;
			bestClear = AiMapClearance(map, ix, iz);
		}
	}

	if (best < 0)
		return 0;

	{
		int bx = best % AIMAP_SIZE;
		int bz = best / AIMAP_SIZE;

		AiMapWorld(map, bx, bz, &out->x, &out->z);
		out->kind = AiMapRoad(map, bx, bz) ? AIGOAL_ROAD : AIGOAL_OPEN;
		out->clearance = bestClear;
		out->samples = ((bx - cix) < 0 ? (cix - bx) : (bx - cix));
		{
			int dzz = ((bz - ciz) < 0 ? (ciz - bz) : (bz - ciz));

			if (dzz > out->samples)
				out->samples = dzz;
		}
	}

	return 1;
}
