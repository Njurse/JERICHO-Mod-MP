#ifndef MP_CARQUERY_H
#define MP_CARQUERY_H

/* ------------------------------------------------------------------
 * mp_carquery.h — the mp <-> carhacks LIVE CAR query contract.
 *
 * mp's "Change car" pause row asks two questions the JERICHO hook
 * surface has no place for (and reports one outcome, MP_CARQ_CHOSEN):
 *
 *   - which CITIES can this session offer? A city's car data is one
 *     level's own; letting a session mix them (so a Rio car can be
 *     driven on a Chicago map) is what carhacks' cross-city import
 *     exists for, and only carhacks knows whether it is doing it.
 *   - make THIS car available: load (city, model) here and make the
 *     session agree on it, if it is not already held.
 *
 * carhacks is a SEPARATE MODULE, so this is a JERICHO CUSTOM EVENT,
 * fired by mp and answered by whoever registers for the id. That is
 * also why it degrades cleanly: a module that does not answer is not an
 * error, and mp falls back to the session's own city and its frontend
 * roster -- the whole feature minus the cross-city half.
 *
 * Custom ids start at JER_EVENT_MODULE_CUSTOM (jericho.h), so they can
 * never collide with an engine event. THIS HEADER IS THE ONE PLACE THE
 * NUMBERS AND THE ARGUMENT STRUCTS ARE WRITTEN DOWN; both modules
 * include it, which is why it depends on nothing but the SDK.
 * ------------------------------------------------------------------ */

#include "jericho.h"

#define MP_CARQ_CITIES	(JER_EVENT_MODULE_CUSTOM + 40)	/* "which cities?" */
#define MP_CARQ_LOAD	(JER_EVENT_MODULE_CUSTOM + 41)	/* "make (city,model) available" */
#define MP_CARQ_CHOSEN	(JER_EVENT_MODULE_CUSTOM + 42)	/* "this is what we drive now" */

/* Handlers fill the result field of the struct and return
 * JER_RESULT_CONTINUE; the caller reads it. */

/* MP_CARQ_CITIES -> ARGS.count city indices written to ARGS.cities[].
 *
 * `city` is a 0..3 index (CHICAGO/HAVANA/VEGAS/RIO). A handler that has
 * nothing to add leaves count at 0 rather than repeating the session's own
 * city, because 0 is what tells mp "nobody knows better": mp then offers the
 * session's city alone. */
typedef struct MP_CARQ_CITIES_ARGS
{
	int* cities;	/* out: city indices, most preferred first */
	int  max;	/* in: capacity of cities[] */
	int  count;	/* out: how many were written; 0 = nobody knows */
} MP_CARQ_CITIES_ARGS;

/* MP_CARQ_LOAD -> ARGS.ok = 1 when this machine now holds (city, model) and the
 * session has been told, so the car can be driven here.
 *
 * This is a REQUEST, not a command: a handler that cannot place the car (no
 * spare resident slot, a palette block it cannot get) answers ok = 0, and mp
 * tells the player the car is not available rather than driving a model it
 * cannot render. */
typedef struct MP_CARQ_LOAD_ARGS
{
	int  city;	/* in: 0..3 */
	int  model;	/* in: model number */
	int  ok;	/* out: 1 = held (or being loaded) here */
} MP_CARQ_LOAD_ARGS;

/* MP_CARQ_CHOSEN -> nothing to fill in: a NOTICE, fired by mp AFTER a switch
 * attempt (the Change car row, MP_TEST_PAUSECAR) and when the local player gets
 * into a car again after being on foot.
 *
 * LOAD only makes a car available; whether the player ends up driving it is
 * decided afterwards (the swap can still refuse). So the session must not be
 * told about the car at load time - it is told here, once the car on the road
 * really is (city, model). This is also the moment the car the player LEFT can
 * be given back (carhacks' release routine), which LOAD cannot know.
 *
 *   changed = 1: the local player's car on the road is now (city, model), where
 *                city is the source city of the slot it is drawn from, or -1 for
 *                the level's own car. Also 1 when it already was that car.
 *   changed = 0: the switch to (city, model) did not happen (city as requested). */
typedef struct MP_CARQ_CHOSEN_ARGS
{
	int  city;	/* in: 0..3, or -1 = the level's own car */
	int  model;	/* in: model number */
	int  changed;	/* in: 1 = driving it now, 0 = the switch did not happen */
} MP_CARQ_CHOSEN_ARGS;

#endif /* MP_CARQUERY_H */
