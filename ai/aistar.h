#ifndef MP_AISTAR_H
#define MP_AISTAR_H

/* ------------------------------------------------------------------
 * aistar.h -- the tiny pathfinder: A* over the aimap grid.
 *
 * Why A* and not something cleverer: the grid is small and static for
 * the life of a plan, the costs are already in aimap (road preference,
 * wall clearance, blocked), and a bounded number of expansions gives a
 * hard ceiling on the work per re-plan. A distance field would be
 * better if we planned every frame from a fixed source - this plans
 * from a moving source, to a moving goal, at a low rate.
 *
 * Properties that matter to the caller:
 *
 *   - NO ALLOCATION. The node pool is a fixed array sized to the
 *     biggest grid aimap can produce, so a plan cannot fail on memory
 *     and cannot fragment anything mid-frame;
 *   - DETERMINISTIC. Ties break on node index, so the same grid and the
 *     same request give the same path. Two instances comparing notes
 *     should not be able to disagree about a tie;
 *   - BOUNDED. AISTAR_EXPANSIONS caps the work; hitting the cap is NOT
 *     an error - the path comes back INCOMPLETE with the best partial
 *     route to the closest reachable point, which is what a stuck car
 *     actually wants (head that way), and it is what the local road
 *     search consumes;
 *   - it reads the grid and nothing else. No engine calls, which is
 *     what lets the self-test build a grid by hand and check the
 *     pathfinder in isolation.
 *
 * The path that comes out is SMOOTHED: the cell-by-cell stair is
 * collapsed into straight runs by line of sight, so a driver steers to a
 * handful of real waypoints rather than every grid step. Without this a
 * car weaves down a straight road. Waypoint 0 is always where the plan
 * STARTED, so a path reads from here to the goal, and every leg after it
 * is a straight run that was checked clear.
 * ------------------------------------------------------------------ */

#include "ai/aimap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Big enough for any grid aimap produces, so the pool is never a limit. */
#define AISTAR_MAX_NODES	(AIMAP_SIZE * AIMAP_SIZE)

/* Work ceiling per plan: the grid itself. Every sample is expanded at most once, so this
 * is a hard bound of AISTAR_MAX_NODES node expansions, and hitting it is still normal
 * (it means the goal is walled off, not that the search gave up early). It was 900, which
 * a winding route through a real maze exhausted - the pathfinder then reported "cannot
 * reach" for a goal that was merely round the corner. */
#define AISTAR_EXPANSIONS	AISTAR_MAX_NODES

/* Straight and diagonal step costs, in the 10/14 integer scheme, scaled
 * by the sample costs at each end. */
#define AISTAR_STEP_STRAIGHT	10
#define AISTAR_STEP_DIAGONAL	14

/* Waypoints a plan may return. A smoothed route is a few dozen turns at
 * the very most; beyond this the tail is dropped rather than the path
 * being rejected (the head is what matters). */
#define AISTAR_MAX_WAYPOINTS	24

typedef struct AIPATH
{
	int	waypoints;			/* how many are in wx[]/wz[] */
	int	wx[AISTAR_MAX_WAYPOINTS];	/* world units */
	int	wz[AISTAR_MAX_WAYPOINTS];
	int	complete;			/* 1 = the goal was reached */
	int	expanded;			/* nodes expanded, for the log */
	int	cost;				/* total path cost, 0 when there is none */
	int	onRoad;				/* waypoints that sit on the road network */
	int	goalX, goalZ;			/* what was actually reached */
} AIPATH;

/* Plan from a world position to another over `map`.
 *
 * Returns 1 when the goal was reached, 0 when it was not - and in BOTH cases
 * path->waypoints / wx[] / wz[] hold the route (the best partial one when the
 * goal could not be reached). Returns 0 with waypoints == 0 only when there is
 * nowhere to go from here at all: no valid grid, or the start is walled in.
 *
 * The start sample being BLOCKED is handled rather than refused: our probe
 * radius is about a car's width, so a car parked against a wall is inside a
 * "blocked" sample by its own definition. The planner walks out to the nearest
 * open sample instead of declaring the car trapped. */
int AiStarPlan(const AIMAP* map, int fromX, int fromZ, int toX, int toZ, AIPATH* path);

/* Would a car get from one world point to another in a straight line? Exposed
 * because the bots also want it: "can I just drive at it from here" is a much
 * cheaper question than a plan, and it is what the smoothing uses. */
int AiStarLineClear(const AIMAP* map, int fromX, int fromZ, int toX, int toZ);

#ifdef __cplusplus
}
#endif

#endif /* MP_AISTAR_H */
