/*
 * aistar.c -- A* over the aimap grid. See aistar.h for the contract.
 *
 * Deliberately plain: a fixed node pool indexed by sample, a linear scan for
 * the best open node, and an octile heuristic scaled to the cheapest possible
 * step. No allocation, no recursion, no engine calls - which is what lets the
 * self-test drive it against a hand-built grid.
 */

#include <string.h>

#include "ai/aimap.h"
#include "ai/aistar.h"

#define AISTAR_OPEN		1
#define AISTAR_CLOSED		2

typedef struct AISTAR_NODE
{
	int	g;
	int	f;
	int	parent;
	unsigned char state;
} AISTAR_NODE;

/* The pool is the whole grid: no node is ever visited twice, so nothing has to
 * grow. A local static rather than a struct field because a plan is a
 * short-lived, single-threaded operation and the game loop is single-threaded. */
static AISTAR_NODE sNodes[AISTAR_MAX_NODES];

static int AiStarIndex(int ix, int iz)
{
	return iz * AIMAP_SIZE + ix;
}

/* Octile distance between two samples, in 10/14 units, times the cheapest
 * sample cost. Admissible: no move can cost less than AIMAP_COST_MIN per step,
 * so this can never overestimate and A* stays optimal within its budget. */
#define AISTAR_COST_MIN	100	/* must match the base cost in AiMapCost */

static int AiStarHeuristic(int ax, int az, int bx, int bz)
{
	int dx = (ax > bx) ? ax - bx : bx - ax;
	int dz = (az > bz) ? az - bz : bz - az;
	int diag = (dx < dz) ? dx : dz;
	int straight = ((dx > dz) ? dx : dz) - diag;

	return (straight * AISTAR_STEP_STRAIGHT + diag * AISTAR_STEP_DIAGONAL) * AISTAR_COST_MIN / 10;
}

/* Is the midpoint of a diagonal step blocked? This is the corner-cut rule: a
 * car that may not clip a wall corner must not be routed through one, and
 * without it A* happily shaves every corner on the map. */
static int AiStarCornerBlocked(const AIMAP* map, int ix, int iz, int dx, int dz)
{
	if (dx == 0 || dz == 0)
		return 0;

	return AiMapBlocked(map, ix + dx, iz) || AiMapBlocked(map, ix, iz + dz);
}

int AiStarLineClear(const AIMAP* map, int fromX, int fromZ, int toX, int toZ)
{
	int dx, dz, span, k;
	int ix, iz;

	if (!AiMapValid(map))
		return 0;

	/* both ends have to be inside the window: off it we know nothing, and "I do
	 * not know" must never be reported as clear */
	if (!AiMapSampleIndex(map, fromX, fromZ, &ix, &iz))
		return 0;

	if (!AiMapSampleIndex(map, toX, toZ, &ix, &iz))
		return 0;

	dx = toX - fromX;
	dz = toZ - fromZ;

	if (dx == 0 && dz == 0)
		return !AiMapBlocked(map, ix, iz);

	/* sampled at half a grid step, so a thin obstacle lying across the line cannot
	 * slip between two tests */
	{
		int adx = (dx < 0) ? -dx : dx;
		int adz = (dz < 0) ? -dz : dz;

		span = ((adx > adz) ? adx : adz) / (AIMAP_STEP / 2);
	}

	if (span < 1)
		span = 1;

	for (k = 0; k <= span; k++)
	{
		int x = fromX + (int)(((long)dx * k) / span);
		int z = fromZ + (int)(((long)dz * k) / span);

		if (!AiMapSampleIndex(map, x, z, &ix, &iz))
			return 0;

		if (AiMapBlocked(map, ix, iz))
			return 0;
	}

	return 1;
}

/* Reconstruct from a node back to the start, then smooth: keep a waypoint only
 * where the straight run to the following kept one stops being clear. */
static int AiStarBuildPath(const AIMAP* map, int goalNode, AIPATH* path)
{
	int chain[AISTAR_MAX_NODES];
	int count = 0;
	int node = goalNode;
	int i, kept;

	/* walk the parents to the start */
	while (node >= 0 && count < AISTAR_MAX_NODES)
	{
		chain[count++] = node;

		if (sNodes[node].parent == node)
			break;

		node = sNodes[node].parent;
	}

	if (count == 0)
		return 0;

	/* Waypoint 0 is where we ARE, so a route reads from here to the goal. After that the
	 * chain is walked with a jump as far as line of sight allows - that IS the smoothing:
	 * a car steers at the next turning point, not at every grid step, and each leg it is
	 * given is known to be drivable. */
	{
		int wx, wz;

		AiMapWorld(map, chain[count - 1] % AIMAP_SIZE, chain[count - 1] / AIMAP_SIZE, &wx, &wz);

		path->wx[0] = wx;
		path->wz[0] = wz;

		/* count it too: onRoad describes the waypoints that came back, and the start
		 * is one of them. (It was counted only inside the loop below, so a route whose
		 * ends were both on the road reported one less than it had.) */
		if (AiMapRoad(map, chain[count - 1] % AIMAP_SIZE, chain[count - 1] / AIMAP_SIZE))
			path->onRoad++;

		kept = 1;
	}

	i = count - 1;	/* the start is already emitted */

	while (i > 0 && kept < AISTAR_MAX_WAYPOINTS)
	{
		int ix = chain[i] % AIMAP_SIZE;
		int iz = chain[i] / AIMAP_SIZE;
		int j, best = i - 1;	/* at worst the next node along: always a legal move */

		for (j = i - 1; j >= 0; j--)
		{
			int nx = chain[j] % AIMAP_SIZE;
			int nz = chain[j] / AIMAP_SIZE;
			int ax, az, bx, bz;

			AiMapWorld(map, ix, iz, &ax, &az);
			AiMapWorld(map, nx, nz, &bx, &bz);

			if (!AiStarLineClear(map, ax, az, bx, bz))
				break;

			best = j;
		}

		{
			int wx, wz;

			AiMapWorld(map, chain[best] % AIMAP_SIZE, chain[best] / AIMAP_SIZE, &wx, &wz);

			path->wx[kept] = wx;
			path->wz[kept] = wz;

			if (AiMapRoad(map, chain[best] % AIMAP_SIZE, chain[best] / AIMAP_SIZE))
				path->onRoad++;

			kept++;
		}

		i = best;
	}

	path->waypoints = kept;
	path->goalX = path->wx[kept > 0 ? kept - 1 : 0];
	path->goalZ = path->wz[kept > 0 ? kept - 1 : 0];

	return kept > 0;
}

int AiStarPlan(const AIMAP* map, int fromX, int fromZ, int toX, int toZ, AIPATH* path)
{
	int six, siz, gix, giz;
	int startNode, goalNode;
	int expanded = 0;
	int bestPartial = -1, bestPartialH = 0;
	int found = 0;
	int i;

	if (path == NULL)
		return 0;

	path->waypoints = 0;
	path->complete = 0;
	path->expanded = 0;
	path->cost = 0;
	path->onRoad = 0;
	path->goalX = fromX;
	path->goalZ = fromZ;

	if (!AiMapValid(map))
		return 0;

	if (!AiMapSampleIndex(map, fromX, fromZ, &six, &siz))
		return 0;

	if (!AiMapSampleIndex(map, toX, toZ, &gix, &giz))
		return 0;

	if (!AiMapNearestOpen(map, six, siz, &six, &siz))
		return 0;	/* walled in: genuinely nowhere to go */

	if (AiMapBlocked(map, gix, giz))
		AiMapNearestOpen(map, gix, giz, &gix, &giz);

	startNode = AiStarIndex(six, siz);
	goalNode = AiStarIndex(gix, giz);

	for (i = 0; i < AISTAR_MAX_NODES; i++)
	{
		sNodes[i].g = 0;
		sNodes[i].f = 0;
		sNodes[i].state = 0;
		sNodes[i].parent = -1;
	}

	sNodes[startNode].parent = startNode;	/* its own parent marks the start */
	sNodes[startNode].g = 0;
	sNodes[startNode].f = AiStarHeuristic(six, siz, gix, giz);
	sNodes[startNode].state = AISTAR_OPEN;

	bestPartial = startNode;
	bestPartialH = sNodes[startNode].f;

	while (expanded < AISTAR_EXPANSIONS)
	{
		int current = -1;
		int bestF = 0;
		int cx, cz, dx, dz;

		/* the best open node. A linear scan: the pool is ~1000 entries and the
		 * budget is ~900 expansions, so a heap would be more code for no
		 * measurable gain, and a scan on the node index breaks ties the same way
		 * every time. */
		for (i = 0; i < AISTAR_MAX_NODES; i++)
		{
			if (sNodes[i].state != AISTAR_OPEN)
				continue;

			if (current < 0 || sNodes[i].f < bestF)
			{
				current = i;
				bestF = sNodes[i].f;
			}
		}

		if (current < 0)
			break;	/* nothing left to expand */

		if (current == goalNode)
		{
			found = 1;
			break;
		}

		sNodes[current].state = AISTAR_CLOSED;
		expanded++;

		cx = current % AIMAP_SIZE;
		cz = current / AIMAP_SIZE;

		/* remember the closest we ever got, for the incomplete case */
		{
			int h = AiStarHeuristic(cx, cz, gix, giz);

			if (h < bestPartialH)
			{
				bestPartialH = h;
				bestPartial = current;
			}
		}

		for (dz = -1; dz <= 1; dz++)
		{
			for (dx = -1; dx <= 1; dx++)
			{
				int nx, nz, n, step, g;

				if (dx == 0 && dz == 0)
					continue;

				nx = cx + dx;
				nz = cz + dz;

				if (nx < 0 || nz < 0 || nx >= AIMAP_SIZE || nz >= AIMAP_SIZE)
					continue;

				n = AiStarIndex(nx, nz);

				if (sNodes[n].state == AISTAR_CLOSED)
					continue;

				if (AiMapBlocked(map, nx, nz))
					continue;

				if (AiStarCornerBlocked(map, cx, cz, dx, dz))
					continue;

				step = (dx != 0 && dz != 0) ? AISTAR_STEP_DIAGONAL : AISTAR_STEP_STRAIGHT;

				/* the step costs both ends, so a route pays for entering a bad
				 * sample as well as leaving one */
				g = sNodes[current].g +
					step * (AiMapCost(map, cx, cz) + AiMapCost(map, nx, nz)) / 20;

				if (sNodes[n].state == AISTAR_OPEN && g >= sNodes[n].g)
					continue;

				sNodes[n].parent = current;
				sNodes[n].g = g;
				sNodes[n].f = g + AiStarHeuristic(nx, nz, gix, giz);
				sNodes[n].state = AISTAR_OPEN;
			}
		}
	}

	path->expanded = expanded;
	path->complete = found;

	if (found)
	{
		if (!AiStarBuildPath(map, goalNode, path))
			return 0;

		path->cost = sNodes[goalNode].g;
		return 1;
	}

	/* Out of budget, or the goal is walled off. Either way the useful answer is
	 * the closest point we did reach: a car that needs to be somewhere else
	 * drives there and re-plans, which is exactly what the local search wants. */
	if (bestPartial >= 0 && bestPartial != startNode)
	{
		if (AiStarBuildPath(map, bestPartial, path))
		{
			path->cost = sNodes[bestPartial].g;
			return 0;
		}
	}

	return 0;
}
