/*
 * ailocal.c -- one flood of the window, two rungs of the answer. See ailocal.h.
 *
 * Static scratch rather than stack: this runs from the game loop, the arrays are sized
 * to the grid, and the module is single-threaded (the same reasoning as aistar's node
 * pool). Nothing is allocated and nothing grows.
 */

#include <string.h>

#include "ai/aimap.h"
#include "ai/aistar.h"	/* AiStarLineClear: reachability of a flee goal */
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

int AiLocalGoal(const AIMAP* map, int fromX, int fromZ, AIGOAL* out)
{
	int six, siz, head, tail;
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

	memset(sSeen, 0, sizeof(sSeen));

	head = 0;
	tail = 0;

	{
		int start = siz * AIMAP_SIZE + six;

		sSeen[start] = 1;
		sDist[start] = 0;
		sQueue[tail++] = start;
	}

	while (head < tail)
	{
		int cur = sQueue[head++];
		int cx = cur % AIMAP_SIZE;
		int cz = cur / AIMAP_SIZE;
		int d = sDist[cur];
		int dx, dz;

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

				sSeen[n] = 1;
				sDist[n] = d + 1;
				sQueue[tail++] = n;
			}
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

/* The road bonus has to stay COMPARABLE TO THE DISTANCE RANGE, or widening the ring
 * drowns it: the distance term grows with the square of the ring (about 0..250 over a
 * ring of 16 at this step), so a fixed small bonus quietly stops mattering and the
 * flee drifts back off the road. 120 is about half that range - the mouse will take a
 * road up to about half the ring closer to the cat, and no more. */
#define AILOCAL_ROAD_BONUS	120

int AiLocalFleeGoal(const AIMAP* map, int fromX, int fromZ, int threatX, int threatZ, AIGOAL* out)
{
	int cix, ciz, ix, iz, best = -1, bestScore = 0, bestClear = 0;

	if (out == NULL)
		return 0;

	AiLocalFail(out);

	if (!AiMapValid(map))
		return 0;

	if (!AiMapSampleIndex(map, fromX, fromZ, &cix, &ciz))
		return 0;

	if (!AiMapNearestOpen(map, cix, ciz, &cix, &ciz))
		return 0;

	/* Score every sample in the window and take the best, rather than walking a fan of
	 * headings: the question is a POINT to drive to, and a point is what a plan wants.
	 * The score is "furthest from the threat, on the road, with room to move" - in that
	 * order of weight, so running away wins over tidiness, and a road only breaks a tie
	 * between places equally far away. Reachability is required by the line-of-sight
	 * test, because a goal across a wall is not a goal. */
	for (iz = 0; iz < AIMAP_SIZE; iz++)
	{
		for (ix = 0; ix < AIMAP_SIZE; ix++)
		{
			int dx, dz, score, wx, wz, tix, tiz;
			long away;

			if (AiMapBlocked(map, ix, iz))
				continue;

			/* it has to be somewhere worth going: near-here is not a flee */
			dx = ix - cix;
			dz = iz - ciz;

			if (dx < 0) dx = -dx;
			if (dz < 0) dz = -dz;

			if (dx < AILOCAL_FLEE_RING / 2 && dz < AILOCAL_FLEE_RING / 2)
				continue;

			if (dx > AILOCAL_FLEE_RING || dz > AILOCAL_FLEE_RING)
				continue;	/* too far to be one plan's worth of driving */

			AiMapWorld(map, ix, iz, &wx, &wz);

			if (!AiStarLineClear(map, fromX, fromZ, wx, wz))
				continue;

			/* how far from the threat, in samples */
			if (!AiMapSampleIndex(map, threatX, threatZ, &tix, &tiz))
			{
				tix = cix;
				tiz = ciz;
			}

			{
				int tdx = ix - tix;
				int tdz = iz - tiz;

				away = (long)tdx * (long)tdx + (long)tdz * (long)tdz;
			}

			score = (int)(away / 4);

			if (AiMapRoad(map, ix, iz))
				score += AILOCAL_ROAD_BONUS;	/* see the define: it must not be drowned by distance */

			score += AiMapClearance(map, ix, iz) * 2;

			if (score > bestScore)
			{
				bestScore = score;
				best = iz * AIMAP_SIZE + ix;
				bestClear = AiMapClearance(map, ix, iz);
			}
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
