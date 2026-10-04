#ifndef MP_AILOCAL_H
#define MP_AILOCAL_H

/* ------------------------------------------------------------------
 * ailocal.h -- the LOCAL search: "get me back to a road".
 *
 * This is deliberately a different KIND of search from aistar. A* answers
 * "how do I get to that specific place"; this answers "where is the
 * nearest place worth being", which is the question a car that has
 * wandered off the road, or been shunted into a garden, actually has.
 *
 * One flood of the window answers both rungs, in order:
 *
 *   1. ROAD  - the nearest sample on the road network that the car can
 *              actually REACH. Breadth-first, so the first road sample
 *              reached is the nearest one, and it is nearest in the
 *              graph rather than in a straight line. Reachability is the
 *              point: a road across a wall is not a way out, and a
 *              line-of-sight test would happily pick one;
 *   2. OPEN  - when no road is reachable, the most open ground that is
 *              (greatest distance to a wall, nearest on a tie). Turning
 *              towards room to move is still better than sitting still;
 *   3. NONE  - nothing at all, which leaves the caller to fall back on
 *              whatever it was doing before.
 *
 * The caller then plans to the goal with aistar, which is why this
 * returns a POINT and not a route: the local search picks the
 * destination, the pathfinder finds the way.
 *
 * Pure, like aimapgrid.c: no engine calls, so the host test covers it.
 * ------------------------------------------------------------------ */

#include "ai/aimap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIGOAL_NONE	0
#define AIGOAL_ROAD	1	/* on the road network, and reachable from here */
#define AIGOAL_OPEN	2	/* no road reachable: the most open ground instead */

typedef struct AIGOAL
{
	int	kind;		/* AIGOAL_* */
	int	x, z;		/* the destination, in world units */
	int	samples;	/* how far away it is, in samples (0 = here) */
	int	clearance;	/* wall clearance at the destination */
} AIGOAL;

/* Fill `out` with where this car should head for. Returns 1 when there is a
 * goal (ROAD or OPEN), 0 when the window offers nothing (kind NONE). */
int AiLocalGoal(const AIMAP* map, int fromX, int fromZ, AIGOAL* out);

/* The other question: where to RUN to, given something to run from. Scores every
 * reachable sample in a ring and takes the one furthest from the threat - on the road
 * and with room to move breaking ties. This answers "where should the mouse go", the
 * way AiLocalGoal answers "where is the road", and it returns a POINT because that
 * is what a plan needs. Returns 1 and fills `out`, or 0 when nothing in the ring is
 * both reachable and worth going to. */
int AiLocalFleeGoal(const AIMAP* map, int fromX, int fromZ, int threatX, int threatZ, AIGOAL* out);

#ifdef __cplusplus
}
#endif

#endif /* MP_AILOCAL_H */
