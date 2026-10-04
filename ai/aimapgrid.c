/*
 * aimapgrid.c -- the PURE half of the world model: everything that reads an
 * already-built grid and nothing that touches the engine.
 *
 * The split is deliberate and it follows carpinref.h/slotrelease.h: the
 * decisions worth testing must not need the engine to exist, so a host test can
 * hand-build a grid and exercise the real cost model and the real pathfinder
 * against it (see JERICHO/test/test_ai_path.c). aimap.c keeps the other half -
 * filling the grid, which is nothing but engine probes.
 *
 * Nothing here calls the engine. If a change ever makes that awkward, the change
 * is in the wrong file.
 */

#include "ai/aimap.h"

int AiMapValid(const AIMAP* map)
{
	return (map != NULL) && map->valid;
}

int AiMapBlocked(const AIMAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= AIMAP_SIZE || iz >= AIMAP_SIZE)
		return 1;	/* outside the window is not somewhere we drive */

	return (map->cell[iz][ix] & AIMAP_BLOCKED) ? 1 : 0;
}

int AiMapRoad(const AIMAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= AIMAP_SIZE || iz >= AIMAP_SIZE)
		return 0;

	return (map->cell[iz][ix] & AIMAP_ROAD) ? 1 : 0;
}

int AiMapClearance(const AIMAP* map, int ix, int iz)
{
	if (map == NULL || ix < 0 || iz < 0 || ix >= AIMAP_SIZE || iz >= AIMAP_SIZE)
		return 0;

	return map->clear[iz][ix];
}

int AiMapCost(const AIMAP* map, int ix, int iz)
{
	int cost;

	if (!AiMapValid(map) || AiMapBlocked(map, ix, iz))
		return AIMAP_COST_BLOCKED;

	cost = 100;	/* must stay AIMAP_COST_MIN in aistar.h: the heuristic is scaled to it */

	/* off the road is allowed and discouraged: the whole point of a road
	 * preference is that it must not make anywhere unreachable */
	if (!AiMapRoad(map, ix, iz))
		cost += 40;

	/* and hug the middle, so a route does not graze a wall all the way down a street */
	cost += (AIMAP_CLEAR_MAX - AiMapClearance(map, ix, iz)) * 6;

	return cost;
}

/* Nearest sample, FLOORING for negative offsets. C integer division truncates toward
 * zero, so the obvious (v + step/2) / step pulls a point a whole step BELOW the window
 * back onto its edge and reports it as inside - which the self-test caught, and which
 * would let a line "leave" the grid without the pathfinder noticing. */
static int AiMapSampleRound(int v, int step)
{
	if (v >= 0)
		return (v + step / 2) / step;

	return -(((-v) + step / 2) / step);
}

int AiMapSampleIndex(const AIMAP* map, int x, int z, int* ix, int* iz)
{
	int sx, sz;

	if (map == NULL)
		return 0;

	sx = AiMapSampleRound(x - map->originX, AIMAP_STEP);
	sz = AiMapSampleRound(z - map->originZ, AIMAP_STEP);

	if (sx < 0 || sz < 0 || sx >= AIMAP_SIZE || sz >= AIMAP_SIZE)
		return 0;

	if (ix != NULL) *ix = sx;
	if (iz != NULL) *iz = sz;

	return 1;
}

void AiMapWorld(const AIMAP* map, int ix, int iz, int* x, int* z)
{
	if (map == NULL)
	{
		if (x != NULL) *x = 0;
		if (z != NULL) *z = 0;
		return;
	}

	if (x != NULL) *x = map->originX + ix * AIMAP_STEP;
	if (z != NULL) *z = map->originZ + iz * AIMAP_STEP;
}

/* Ring by ring, so the first hit is the nearest one. Shared rather than duplicated: the
 * pathfinder and the local road search both need it, and two copies would eventually
 * disagree about what "nearest" means. */
int AiMapNearestOpen(const AIMAP* map, int ix, int iz, int* ox, int* oz)
{
	int r, dx, dz;

	if (!AiMapBlocked(map, ix, iz))
	{
		if (ox != NULL) *ox = ix;
		if (oz != NULL) *oz = iz;
		return 1;
	}

	for (r = 1; r <= AIMAP_OPEN_SNAP; r++)
	{
		for (dz = -r; dz <= r; dz++)
		{
			for (dx = -r; dx <= r; dx++)
			{
				if ((dx < 0 ? -dx : dx) != r && (dz < 0 ? -dz : dz) != r)
					continue;	/* the ring, not the disc */

				if (!AiMapBlocked(map, ix + dx, iz + dz))
				{
					if (ox != NULL) *ox = ix + dx;
					if (oz != NULL) *oz = iz + dz;
					return 1;
				}
			}
		}
	}

	return 0;
}
