/*
 * aimap.c -- build and query the AI's occupancy grid. See aimap.h for why
 * the world model looks like this.
 *
 * Everything here is a THIN wrapper over the engine's own queries:
 *
 *   CellEmpty(p, radius)   the scenery test the whole game uses, which
 *                          deliberately ignores smashables and chairs;
 *   JerRoadAt(x, y, z)     JERICHO's road-network hook (dr2roads.c), a
 *                          wrapper over the engine's RoadInCell;
 *   MapHeight(p)           the ground height, so a sample is probed at the
 *                          height of the GROUND THERE rather than at the
 *                          car's height - CellEmpty compares heights, so
 *                          probing a distant sample at our own height
 *                          would invent walls and lose real ones.
 */

#include <string.h>

#include "jericho.h"
#include "driver2.h"
#include "objcoll.h"	/* CellEmpty: the engine's own scenery test */
#include "map.h"	/* cells_across: the "is a level loaded" test */
#include "dr2roads.h"	/* JerRoadAt, MapHeight */

#include "mp.h"
#include "ai/aimap.h"

/* ------------------------------------------------------------------ */
/* The world                                                           */
/* ------------------------------------------------------------------ */

int AiMapWorldLoaded(void)
{
	/* cells_across is filled in from the level header when a level loads, and is 0
	 * before that. It is the same field MAP_CELL_SIZE is a macro for (map.h), i.e.
	 * the value this module has divided by and crashed on, so asking it here is
	 * exactly right: no level, no world to sample. */
	return cells_across > 0;
}

static void AiMapFill(AIMAP* map, unsigned char flags)
{
	int ix, iz;

	for (iz = 0; iz < AIMAP_SIZE; iz++)
	{
		for (ix = 0; ix < AIMAP_SIZE; ix++)
		{
			map->cell[iz][ix] = flags;
			map->clear[iz][ix] = 0;
		}
	}
}

/* How many samples from the nearest blocked one, up to AIMAP_CLEAR_MAX. A
 * bounded neighbourhood scan, done once per build rather than per query
 * because the pathfinder asks this a great many times. */
static void AiMapMeasureClearance(AIMAP* map)
{
	int ix, iz, dx, dz;

	for (iz = 0; iz < AIMAP_SIZE; iz++)
	{
		for (ix = 0; ix < AIMAP_SIZE; ix++)
		{
			int best = AIMAP_CLEAR_MAX + 1;

			if (map->cell[iz][ix] & AIMAP_BLOCKED)
			{
				map->clear[iz][ix] = 0;
				continue;
			}

			for (dz = -AIMAP_CLEAR_MAX; dz <= AIMAP_CLEAR_MAX && best > 1; dz++)
			{
				for (dx = -AIMAP_CLEAR_MAX; dx <= AIMAP_CLEAR_MAX; dx++)
				{
					int sx = ix + dx;
					int sz = iz + dz;
					int d;

					if (sx < 0 || sz < 0 || sx >= AIMAP_SIZE || sz >= AIMAP_SIZE)
						continue;	/* outside the window: unknown, not blocked */

					if ((map->cell[sz][sx] & AIMAP_BLOCKED) == 0)
						continue;

					d = (dx < 0 ? -dx : dx);
					if ((dz < 0 ? -dz : dz) > d)
						d = (dz < 0 ? -dz : dz);

					if (d < best)
						best = d;
				}
			}

			map->clear[iz][ix] = (best > AIMAP_CLEAR_MAX) ? AIMAP_CLEAR_MAX : best;
		}
	}
}

int AiMapBuild(AIMAP* map, int x, int y, int z)
{
	int ix, iz;

	if (map == NULL)
		return 0;

	map->valid = 0;
	map->sampled = 0;
	map->roadSamples = 0;
	map->centreX = x;
	map->centreY = y;
	map->centreZ = z;

	/* an odd grid, so sample AIMAP_SIZE/2 IS the centre we were given */
	map->originX = x - (AIMAP_SIZE / 2) * AIMAP_STEP;
	map->originZ = z - (AIMAP_SIZE / 2) * AIMAP_STEP;

	if (!AiMapWorldLoaded())
	{
		/* No level: say so, and leave a grid that reports "blocked" everywhere so a
		 * caller who ignores the return value cannot plan across a world that is not
		 * there. */
		AiMapFill(map, AIMAP_BLOCKED);
		return 0;
	}

	for (iz = 0; iz < AIMAP_SIZE; iz++)
	{
		for (ix = 0; ix < AIMAP_SIZE; ix++)
		{
			int wx = map->originX + ix * AIMAP_STEP;
			int wz = map->originZ + iz * AIMAP_STEP;
			unsigned char f = 0;
			VECTOR v;

			v.vx = wx;
			v.vz = wz;
			v.vy = MapHeight(&v);	/* probe at the ground there, not at our height */

			if (!CellEmpty(&v, AIMAP_RADIUS))
				f |= AIMAP_BLOCKED;

			if (!(f & AIMAP_BLOCKED) && JerRoadAt(wx, v.vy, wz))
				f |= AIMAP_ROAD;

			map->cell[iz][ix] = f;
			map->clear[iz][ix] = 0;

			if (f & AIMAP_ROAD)
				map->roadSamples++;

			map->sampled++;
		}
	}

	AiMapMeasureClearance(map);

	map->valid = 1;

	return 1;
}

/* The queries and the cost model live in aimapgrid.c: they are PURE, so a host test
 * can exercise them (and the pathfinder) against a hand-built grid with no engine
 * present. Only the filling of the grid - which is nothing but engine probes - is
 * in this file. */
