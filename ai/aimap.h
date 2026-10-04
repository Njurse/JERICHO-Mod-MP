#ifndef MP_AIMAP_H
#define MP_AIMAP_H

/* ------------------------------------------------------------------
 * aimap.h -- the AI's view of the world: a small occupancy grid.
 *
 * The bots used to decide by probing a few points along a heading and
 * hoping. That cannot tell a street from the map edge, cannot see a lamp
 * post that is not exactly on the probe line, and has no notion of a
 * ROUTE at all - which is why a pair shuffles, corners itself, or drives
 * into scenery it believed was clear. This is the world model that the
 * pathfinder (aistar.h) and the "get back to the road" search
 * (ailocal.h) both walk over.
 *
 * It is deliberately small and dumb:
 *
 *   - a FIXED AIMAP_SIZE x AIMAP_SIZE window of samples AIMAP_STEP world
 *     units apart, re-centred on whoever asks. Fixed, because the
 *     pathfinder's node pools have to be fixed too, and because the size
 *     of the map is not ours to choose;
 *   - every sample is one CellEmpty() call: the engine's own scenery
 *     test. That is why FENCES AND BARRELS ARE NOT WALLS - CellEmpty
 *     skips MODEL_FLAG_SMASHABLE and chairs by design (objcoll.c:49) -
 *     so the grid already sees through exactly what a player drives
 *     through. Nothing here special-cases that; it falls out of using
 *     the engine's own test;
 *   - the road network is a PREFERENCE, never a wall. A sample is marked
 *     road/off-road from JerRoadAt and the cost model penalises off-road
 *     rather than forbidding it, because a car that lands in a garden
 *     has to be able to get out;
 *   - wall proximity is measured, so a route prefers the middle of a
 *     road instead of grazing its edge.
 *
 * NOTHING HERE DIVIDES BY MAP_CELL_SIZE. That is a level-header field
 * (map.h) which is ZERO in the frontend, and this module has crashed on
 * it before: the window is positioned in world units with a fixed step,
 * and the world is only sampled at all once cells_across says a level is
 * loaded. When it is not, the build reports so and every query answers
 * "blocked", so a caller that ignores the return value cannot act on a
 * half-built grid.
 * ------------------------------------------------------------------ */

#include <stddef.h>	/* NULL: the pure half of this library has no engine headers to lean on */

#ifdef __cplusplus
extern "C" {
#endif

#define AIMAP_SIZE	49	/* samples per side; odd, so one sits on the centre */
#define AIMAP_STEP	512	/* world units between samples (a quarter of a MAP_CELL_SIZE) */
#define AIMAP_RADIUS	350	/* the probe's radius: about a car's width */

/* The window is 49 x 512 = +/- 12,288 world units, i.e. about +/- 6 map cells and a fifth of
 * a region. It was 33 x 384 = +/- 6,144, which at a car's speed is a couple of seconds of
 * driving - so every route was short and the car re-planned almost as often as it steered.
 * The cost of the bigger window is real but small: 2401 samples instead of 1089 per build
 * (three cheap engine calls each) and a worst case of 2401-squared comparisons in A*'s
 * open-list scan, at a re-plan rate of about 1.4 Hz. */

/* Sample flags. */
#define AIMAP_BLOCKED	0x01	/* CellEmpty found something a car cannot pass */
#define AIMAP_ROAD	0x02	/* JerRoadAt: on the driveable road network */

/* How far clearance is measured, in samples. Beyond this everything reads
 * "wide open", which is all the cost model needs. */
#define AIMAP_CLEAR_MAX	6

/* The cost of a sample a route may not use. Big, but not INT_MAX, so
 * "cheaper" comparisons stay sane. */
#define AIMAP_COST_BLOCKED	1000000

typedef struct AIMAP
{
	int	originX, originZ;	/* world position of sample (0, 0) */
	int	centreX, centreZ;	/* the world position the window was centred on */
	int	centreY;		/* the height the probes used, for reference */
	int	valid;			/* 0 until a build has succeeded */
	int	sampled;		/* samples the engine answered for */
	int	roadSamples;		/* ...of which were on the road network */

	unsigned char cell[AIMAP_SIZE][AIMAP_SIZE];
	int	clear[AIMAP_SIZE][AIMAP_SIZE];	/* samples to the nearest blocked one, capped */
} AIMAP;

/* Build the window centred on (x, y, z). Returns 1 when the world was there to
 * sample, 0 when it was not (frontend, loading screen, between levels). */
int  AiMapBuild(AIMAP* map, int x, int y, int z);

int  AiMapValid(const AIMAP* map);
int  AiMapBlocked(const AIMAP* map, int ix, int iz);
int  AiMapRoad(const AIMAP* map, int ix, int iz);
int  AiMapClearance(const AIMAP* map, int ix, int iz);

/* The nearest sample a car could actually be: used when a probe radius leaves the car
 * inside a "blocked" sample, which is what happens whenever it is parked against a wall.
 * Returns 1 and writes the sample; 0 when there is nothing open within a few samples. */
#define AIMAP_OPEN_SNAP	4	/* how far out to look, in samples */
int  AiMapNearestOpen(const AIMAP* map, int ix, int iz, int* ox, int* oz);

/* Traversal cost. Blocked samples cost AIMAP_COST_BLOCKED. */
int  AiMapCost(const AIMAP* map, int ix, int iz);

/* World <-> sample conversion. AiMapSampleIndex returns 1 and writes the
 * nearest sample when (x, z) is inside the window, 0 when it is not. */
int  AiMapSampleIndex(const AIMAP* map, int x, int z, int* ix, int* iz);
void AiMapWorld(const AIMAP* map, int ix, int iz, int* x, int* z);

/* 1 when a level is loaded, so the world is worth sampling. Cheap; exposed
 * because both the search and the bots want to know before doing anything. */
int  AiMapWorldLoaded(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_AIMAP_H */
