/*
 * test_mp_mark.cpp -- the standalone check for the marked-player referee (mp_mark.c)
 * and its wire format (MP_MARK / MP_MARK_ENTRY, MP_TAG_MARK, proto 24).
 *
 * Two ways to run it:
 *   test_mp_mark            the rule checks below (exit 0 = every rule holds)
 *   test_mp_mark --script   read referee steps on stdin and print the snapshot
 *                           after each one, so run_mode_check.py can drive the
 *                           Python twin (dedi/markref.py) with the same steps and
 *                           compare the bytes. One step per line:
 *                             reset <mode> <minutes>
 *                             sync <rnd> <id>...      (the whole present-set)
 *                             ready <id>...           (the whole ready-set)
 *                             tick <ms> <rnd>
 *                             contact <a> <b>
 *                             hit <a> <b> <strength> / drive <ms> <speed> / escape
 *                             damage <from> <damage> <max>
 *                             out <id> <rnd>          (v24: no respawn this round)
 *                           Each prints "STEP <n> <events> <hex>".
 *
 * Builds against mp_mark.c, mp_mode.c and mp_round.c alone (.cpp so the mod
 * project's **.c glob leaves it out of the DLL).
 */
#include "mp_mark.h"
#include "mp_mode.h"
#include "mp_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFails = 0;
static int gChecks = 0;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static_assert(MP_PROTO_VERSION == 24, "round lifecycle v2 is proto 24");
static_assert(sizeof(MP_MARK) == 16, "MP_MARK (v24: endReason, flags, spare)");
static_assert(sizeof(MP_MARK_ENTRY) == 4, "MP_MARK_ENTRY");
static_assert(sizeof(MP_ROUND) == 8 && sizeof(MP_ROUND_ENTRY) == 8, "JPRD is untouched");
static_assert(sizeof(MP_ROSTER_ENTRY) == 32, "the roster entry is untouched");
static_assert(sizeof(MP_WELCOME) == 18, "the welcome is untouched");
static_assert(sizeof(MP_SESSION) == 12, "the session is untouched");
static_assert(sizeof(MP_BEACON) == 50, "the beacon is untouched");

#define SNAP_MAX ((int)(sizeof(MP_MARK) + MP_MAX_PLAYERS * sizeof(MP_MARK_ENTRY)))
#define MIN_MS 60000

static void Present(const int* ids, int n, unsigned rnd)
{
	int present[MP_MAX_PLAYERS];
	int i;

	memset(present, 0, sizeof(present));
	for (i = 0; i < n; i++)
		present[ids[i]] = 1;

	MpMarkSync(present, rnd);
}

/* Every id has picked a car: the round-start gate is only the player count. */
static void AllReady(void)
{
	int ready[MP_MAX_PLAYERS];
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		ready[i] = 1;

	MpMarkReady(ready);
}

static void Ready(const int* ids, int n)
{
	int ready[MP_MAX_PLAYERS];
	int i;

	memset(ready, 0, sizeof(ready));
	for (i = 0; i < n; i++)
		ready[ids[i]] = 1;

	MpMarkReady(ready);
}

static void CheckTag(void)
{
	static const char* const kUsed[] = {
		"JPHL", "JPWL", "JPRJ", "JPST", "JPIN", "JPCS", "JPPN", "JPPO", "JPCH", "JPLV",
		"JPKK", "JPRS", "JPHI", "JPPD", "JPCL", "JPCC", "JPTF", "JPCX", "JPSX", "JPRD",
		"JPSS", "JPSW"
	};
	size_t i;

	CHECK(strlen(MP_TAG_MARK) == 4 && memcmp(MP_TAG_MARK, "JP", 2) == 0);
	for (i = 0; i < sizeof(kUsed) / sizeof(kUsed[0]); i++)
		CHECK(memcmp(MP_TAG_MARK, kUsed[i], 4) != 0);
}

static void CheckMinutes(void)
{
	CHECK(MP_MARK_MINUTES_DEFAULT == 3);
	CHECK(MpMarkClampMinutes(0) == 1 && MpMarkClampMinutes(-7) == 1);
	CHECK(MpMarkClampMinutes(1) == 1 && MpMarkClampMinutes(5) == 5);
	CHECK(MpMarkClampMinutes(6) == 5 && MpMarkClampMinutes(99) == 5);

	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, MP_MARK_MINUTES_DEFAULT);
	CHECK(MpMarkMinutes() == 3);
	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, 9);
	CHECK(MpMarkMinutes() == 5);
}

/* Take a Ride: the referee is inert -- no state, nothing ticks, no tag. */
static void CheckTakeARide(void)
{
	int ids[] = { 0, 1, 2 };

	MpMarkReset(MP_GAMEMODE_TAKEADRIDE, 3);
	CHECK(!MpMarkHaveState());
	Present(ids, 3, 0);
	AllReady();
	CHECK(MpMarkTick(1000, 0) == 0);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 0) == 0);
	CHECK(MpMarkCountdownMs(0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1);
	CHECK(MpMarkContact(0, 1) == 0);
}

static void CheckFoxAndHounds(void)
{
	int one[] = { 2 };
	int two[] = { 2, 5 };
	int three[] = { 2, 5, 7 };
	unsigned char buf[SNAP_MAX];
	MP_MARK h;
	MP_MARK_ENTRY e;
	int len, fox, hound;

	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, 3);
	CHECK(MpMarkHaveState());
	AllReady();	/* the round-start gate has its own check: CheckRoundStart */

	/* one player: the wait is not satisfied, nothing starts, nothing ticks */
	Present(one, 1, 0);
	CHECK(MpMarkTick(5000, 0) == 0);
	CHECK(MpMarkTick(MP_MARK_LOBBY_MS, 0) == 0);
	CHECK(MpMarkTick(5000, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1);
	CHECK(MpMarkClockMs(2, 0) == 3 * MIN_MS);

	/* the second arrives: the first-load countdown (5 s), then live */
	Present(two, 2, 0);
	CHECK(MpMarkTick(33, 1) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);
	CHECK(MpMarkTick(5 * 1000, 1) == MP_MARK_EV_BEGIN);	/* rnd 1 of {2,5} -> 5 */
	CHECK(MpMarkPhase() == MP_MARK_PHASE_LIVE && MpMarkMarked() == 5);
	fox = 5; hound = 2;
	CHECK(MpMarkSeq() == 1);

	/* only the fox's clock ticks */
	MpMarkTick(1000, 0);
	CHECK(MpMarkClockMs(fox, 0) == 3 * MIN_MS - 1000);
	CHECK(MpMarkClockMs(hound, 0) == 3 * MIN_MS);

	/* a client's run-on between snapshots: the fox's clock, not the hound's */
	CHECK(MpMarkClockMs(fox, 500) == 3 * MIN_MS - 1500);
	CHECK(MpMarkClockMs(hound, 500) == 3 * MIN_MS);

	/* a tag inside the go-live cooldown is refused (the start-line bump) */
	CHECK(MpMarkContact(hound, fox) == 0);
	MpMarkTick(1000, 0);			/* 2.0 s after live */
	CHECK(MpMarkContact(hound, fox) == 1);	/* the hound is now the fox */
	CHECK(MpMarkMarked() == hound && MpMarkSeq() == 2);

	/* the same bump cannot bounce it straight back */
	CHECK(MpMarkContact(fox, hound) == 0);
	MpMarkTick(1999, 0);
	CHECK(MpMarkContact(fox, hound) == 0);
	CHECK(MpMarkMarked() == hound);

	/* the old fox's clock is frozen where it was; the new fox's runs */
	CHECK(MpMarkClockMs(fox, 0) == 3 * MIN_MS - 2000);
	CHECK(MpMarkClockMs(hound, 0) == 3 * MIN_MS - 1999);
	MpMarkTick(1, 0);
	CHECK(MpMarkContact(fox, hound) == 1);	/* 2 s later it may move again */
	CHECK(MpMarkMarked() == fox);

	/* two hounds bumping: nothing */
	Present(three, 3, 0);
	CHECK(MpMarkClockMs(7, 0) == 3 * MIN_MS);	/* a late joiner: full, frozen */
	MpMarkTick(2500, 0);
	CHECK(MpMarkContact(hound, 7) == 0);
	CHECK(MpMarkMarked() == fox);

	/* the snapshot */
	len = MpMarkBuild(buf, (int)sizeof(buf));
	CHECK(len == (int)(sizeof(MP_MARK) + 3 * sizeof(MP_MARK_ENTRY)));
	CHECK(MpMarkBuild(buf, len - 1) == 0);
	memcpy(&h, buf, sizeof(h));
	CHECK(h.gamemode == MP_GAMEMODE_FOXHOUNDS && h.phase == MP_MARK_PHASE_LIVE);
	CHECK(h.marked == fox && h.winner == MP_MARK_NONE && h.minutes == 3 && h.count == 3);
	CHECK(h.sharedDs == 0 && h.countdownDs == 0);
	memcpy(&e, buf + sizeof(h), sizeof(e));
	CHECK(e.flags == MP_MARK_ENTRY_F_READY);
	CHECK(e.playerId == 2 && e.remainDs == (3 * MIN_MS - 2000) / 100);	/* frozen since it passed back */

	/* first to 0:00 wins */
	MpMarkTick(3 * MIN_MS - 2000 - 2500 - 1, 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_LIVE);
	CHECK(MpMarkClockMs(fox, 0) == 1);
	CHECK(MpMarkTick(1, 0) == MP_MARK_EV_END);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_ENDED && MpMarkWinner() == fox);
	CHECK(MpMarkClockMs(fox, 0) == 0);
	CHECK(MpMarkContact(hound, fox) == 0);	/* over: no tags */

	/* the intermission, the countdown, then the next round with fresh clocks */
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS - 1, 0) == 0);
	CHECK(MpMarkTick(1, 2) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 2) == MP_MARK_EV_BEGIN);	/* rnd 2 of {2,5,7} -> 7 */
	CHECK(MpMarkMarked() == 7 && MpMarkWinner() == -1);
	CHECK(MpMarkClockMs(fox, 0) == 3 * MIN_MS && MpMarkClockMs(hound, 0) == 3 * MIN_MS);

	/* the fox leaves: the next one is picked */
	Present(two, 2, 1);
	CHECK(MpMarkMarked() == 5);

	/* down to one player: the round ends with no winner, then waits. The fox
	 * leaving with fewer than two left ends it at once and marks nobody (0.23.4) */
	Present(one, 1, 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_ENDED && MpMarkMarked() == -1);
	CHECK(MpMarkTick(33, 0) == 0);
	CHECK(MpMarkWinner() == -1);
	MpMarkTick(MP_MARK_INTERMISSION_MS, 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1);
}

static void CheckBombTag(void)
{
	int two[] = { 1, 4 };
	int three[] = { 1, 4, 9 };
	unsigned char buf[SNAP_MAX];
	MP_MARK h;
	MP_MARK_ENTRY e;
	int holder, other;

	MpMarkReset(MP_GAMEMODE_BOMBTAG, 1);
	AllReady();
	Present(two, 2, 0);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_COUNTDOWN);	/* first-load 5 s, cars already in */
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS);
	CHECK(MpMarkTick(1000, 0) == 0);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS);		/* the countdown is not the bomb's time */
	CHECK(MpMarkContact(1, 4) == 0);		/* nor a pass */
	CHECK(MpMarkTick(5 * 1000 - 1000, 0) == MP_MARK_EV_BEGIN);	/* rnd 0 -> 1 */
	holder = 1; other = 4;
	CHECK(MpMarkMarked() == holder);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS);

	/* one shared clock: it ticks whoever holds the bomb */
	MpMarkTick(2000, 0);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS - 2000);
	CHECK(MpMarkClockMs(-1, 300) == MIN_MS - 2300);
	CHECK(MpMarkClockMs(holder, 0) == 0);		/* no personal clocks */

	/* the holder passes it; it cannot come straight back */
	CHECK(MpMarkContact(holder, other) == 1);
	CHECK(MpMarkMarked() == other);
	CHECK(MpMarkContact(holder, other) == 0);
	MpMarkTick(1000, 0);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS - 3000);	/* the clock did not reset */

	/* a late joiner has no bomb */
	Present(three, 3, 0);
	CHECK(MpMarkMarked() == other);

	MpMarkBuild(buf, (int)sizeof(buf));
	memcpy(&h, buf, sizeof(h));
	CHECK(h.gamemode == MP_GAMEMODE_BOMBTAG && h.marked == other && h.minutes == 1);
	CHECK(h.sharedDs == (MIN_MS - 3000) / 100 && h.count == 3 && h.countdownDs == 0);
	memcpy(&e, buf + sizeof(h) + 2 * sizeof(e), sizeof(e));
	CHECK(e.playerId == 9 && e.remainDs == 0);

	/* the holder at 0:00 loses: phase ENDED, still marked, nobody "won" */
	CHECK(MpMarkTick(MIN_MS - 3000, 0) == MP_MARK_EV_END);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_ENDED);
	CHECK(MpMarkMarked() == other && MpMarkWinner() == -1);
	CHECK(MpMarkClockMs(-1, 999) == 0);
}

/* A client: the snapshot replaces the copy; refusals keep the last one. */
static void CheckApply(void)
{
	int two[] = { 0, 3 };
	unsigned char buf[SNAP_MAX];
	int len;

	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, 2);
	AllReady();
	Present(two, 2, 0);
	MpMarkTick(10, 1);			/* first-load countdown */
	MpMarkTick(5 * 1000, 1);
	MpMarkTick(1234, 0);
	len = MpMarkBuild(buf, (int)sizeof(buf));

	MpMarkReset(MP_GAMEMODE_TAKEADRIDE, 3);
	CHECK(MpMarkApply(buf, len - 1) == 0);		/* short */
	CHECK(!MpMarkHaveState());
	CHECK(MpMarkApply(buf, len) == 1);
	CHECK(MpMarkHaveState() && MpMarkMode() == MP_GAMEMODE_FOXHOUNDS);
	CHECK(MpMarkMarked() == 3 && MpMarkPhase() == MP_MARK_PHASE_LIVE && MpMarkMinutes() == 2);
	CHECK(MpMarkClockMs(3, 0) == (2 * MIN_MS - 1234 + 99) / 100 * 100);
	CHECK(MpMarkClockMs(0, 0) == 2 * MIN_MS);

	buf[5] = MP_MAX_PLAYERS + 1;			/* count out of range */
	CHECK(MpMarkApply(buf, len) == 0);
	CHECK(MpMarkMarked() == 3);
}

/* THE ROUND START (t508): wait for every car, COUNTDOWN (no clock, no tag), GO. */
static void CheckRoundStart(void)
{
	int two[] = { 3, 6 };
	int three[] = { 3, 6, 8 };
	int only3[] = { 3 };
	unsigned char buf[SNAP_MAX];
	MP_MARK h;
	MP_MARK_ENTRY e;
	int len, ready;

	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, 2);
	Present(two, 2, 0);

	/* two present, one still in the car select: nothing starts */
	Ready(only3, 1);
	CHECK(MpMarkTick(0, 0) == 0);
	CHECK(MpMarkTick(MP_MARK_LOBBY_MS, 0) == 0);
	CHECK(MpMarkTick(5000, 0) == 0);
	CHECK(MpMarkTick(60000, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1);
	CHECK(MpMarkClockMs(3, 0) == 2 * MIN_MS && MpMarkClockMs(6, 0) == 2 * MIN_MS);
	CHECK(MpMarkCountWaiting(&ready) == 2 && ready == 1);
	CHECK(MpMarkPlayerReady(3) && !MpMarkPlayerReady(6));
	len = MpMarkBuild(buf, (int)sizeof(buf));
	memcpy(&e, buf + sizeof(MP_MARK) + sizeof(e), sizeof(e));
	CHECK(len > 0 && e.playerId == 6 && e.flags == 0);

	/* the last car is picked: COUNTDOWN, with nobody marked yet */
	Ready(two, 2);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN && MpMarkMarked() == -1);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);
	CHECK(MpMarkCountdownMs(1500) == 5 * 1000 - 1500);
	len = MpMarkBuild(buf, (int)sizeof(buf));
	memcpy(&h, buf, sizeof(h));
	CHECK(h.phase == MP_MARK_PHASE_COUNTDOWN && h.countdownDs == (5 * 1000) / 100);
	memcpy(&e, buf + sizeof(h) + sizeof(e), sizeof(e));
	CHECK(e.flags == MP_MARK_ENTRY_F_READY);

	/* the countdown touches no clock and allows no tag */
	CHECK(MpMarkTick(1000, 0) == 0);
	CHECK(MpMarkContact(3, 6) == 0);
	CHECK(MpMarkClockMs(3, 0) == 2 * MIN_MS && MpMarkClockMs(6, 0) == 2 * MIN_MS);
	CHECK(MpMarkClockMs(3, 900) == 2 * MIN_MS);	/* a client does not run it on either */
	CHECK(MpMarkCountdownMs(0) == 5 * 1000 - 1000);

	/* a joiner arrives before GO without a car: back to waiting for them */
	Present(three, 3, 0);
	CHECK(MpMarkTick(33, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkCountdownMs(0) == 0);
	CHECK(MpMarkTick(30000, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);

	/* their car is in: a fresh first-load countdown (GO has not happened yet) */
	Ready(three, 3);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);
	CHECK(MpMarkTick(5 * 1000 - 1, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN);
	CHECK(MpMarkClockMs(3, 0) == 2 * MIN_MS && MpMarkClockMs(6, 0) == 2 * MIN_MS && MpMarkClockMs(8, 0) == 2 * MIN_MS);

	/* GO: live, a fox, the go-live cooldown as before, the fox's clock starts */
	CHECK(MpMarkTick(1, 1) == MP_MARK_EV_BEGIN);	/* rnd 1 of {3,6,8} -> 6 */
	CHECK(MpMarkPhase() == MP_MARK_PHASE_LIVE && MpMarkMarked() == 6 && MpMarkCountdownMs(0) == 0);
	CHECK(MpMarkContact(3, 6) == 0);		/* cooldown */
	MpMarkTick(2000, 0);
	CHECK(MpMarkClockMs(6, 0) == 2 * MIN_MS - 2000);
	CHECK(MpMarkClockMs(3, 0) == 2 * MIN_MS);
	CHECK(MpMarkContact(3, 6) == 1 && MpMarkMarked() == 3);

	/* a player leaving a countdown below two: waiting, no round */
	MpMarkReset(MP_GAMEMODE_BOMBTAG, 1);
	Present(two, 2, 0);
	Ready(two, 2);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);
	Present(only3, 1, 0);
	CHECK(MpMarkTick(5 * 1000, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS);

	/* still the first-load count (GO never happened), then GO, then intermission */
	Present(two, 2, 0);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst());
	CHECK(MpMarkTick(5 * 1000, 0) == MP_MARK_EV_BEGIN);
	CHECK(MpMarkTick(MIN_MS, 0) == MP_MARK_EV_END);
	CHECK(MpMarkClockMs(-1, 0) == 0);
	Ready(only3, 1);				/* (a test-only flip: cars stay picked) */
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1 && MpMarkWinner() == -1);
	/* ... and with every car in: COUNTDOWN with the clock full again, then GO */
	Ready(two, 2);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 0) == MP_MARK_EV_BEGIN);
	CHECK(MpMarkTick(1000, 0) == 0 && MpMarkClockMs(-1, 0) == MIN_MS - 1000);

	/* straight from the intermission into the countdown when everyone has a car */
	CHECK(MpMarkTick(MIN_MS, 0) == MP_MARK_EV_END);
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN && MpMarkMarked() == -1);

	/* the mirror: a client gets the countdown and the ready flags */
	len = MpMarkBuild(buf, (int)sizeof(buf));
	MpMarkReset(MP_GAMEMODE_TAKEADRIDE, 3);
	CHECK(MpMarkApply(buf, len) == 1);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN && MpMarkCountdownMs(0) == MP_MARK_COUNTDOWN_MS);
	CHECK(MpMarkCountdownMs(2500) == MP_MARK_COUNTDOWN_MS - 2500 && MpMarkCountdownMs(9999) == 0);
	CHECK(MpMarkPlayerReady(3) && MpMarkPlayerReady(6) && !MpMarkPlayerReady(8));
	CHECK(MpMarkClockMs(-1, 3000) == MIN_MS);	/* frozen on a client too */
}

/* THE PRE-GAME WAIT (t515/t516): after a map load, WAITING until the mode's
 * wait is satisfied, then one first-load N..1; later rounds on the same map
 * use the shorter per-round count. PHASE_LOBBY is unused. */
static void CheckPreGame(void)
{
	int two[] = { 1, 4 };
	int one[] = { 1 };
	unsigned char buf[SNAP_MAX];
	MP_MARK h;
	int len;

	/* Take a Ride: no wait, no countdown, ever (the referee is inert) */
	MpMarkReset(MP_GAMEMODE_TAKEADRIDE, 3);
	Present(two, 2, 0);
	CHECK(MpMarkTick(0, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkCountdownMs(0) == 0);
	CHECK(MpModeHasPreGame(MP_GAMEMODE_TAKEADRIDE) == 0);

	/* a map load with nobody in it yet: nothing, until the wait is satisfied */
	MpMarkReset(MP_GAMEMODE_BOMBTAG, 2);
	AllReady();
	CHECK(MpMarkTick(10000, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	Present(one, 1, 0);
	CHECK(MpMarkTick(33, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkCountdownMs(0) == 0);
	CHECK(MpMarkMarked() == -1);

	/* the second player (cars already in): first-load 5, 4, ... COUNTDOWN */
	Present(two, 2, 0);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownTotalMs() == 5 * 1000);
	CHECK(MpMarkTick(1000, 0) == 0);
	CHECK(MpMarkCountdownMs(0) == 5 * 1000 - 1000);
	CHECK(MpMarkCountdownMs(500) == 5 * 1000 - 1500);
	CHECK(MpMarkClockMs(-1, 0) == 2 * MIN_MS && MpMarkContact(1, 4) == 0);
	len = MpMarkBuild(buf, (int)sizeof(buf));
	memcpy(&h, buf, sizeof(h));
	CHECK(h.phase == MP_MARK_PHASE_COUNTDOWN && h.countdownDs == (5 * 1000 - 1000) / 100);

	/* everyone leaves mid-count: back to WAITING, and first-map stays armed */
	Present(one, 0, 0);
	CHECK(MpMarkTick(2000, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	CHECK(MpMarkCountdownMs(0) == 0);
	Present(two, 2, 0);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);
	CHECK(MpMarkTick(5 * 1000 - 1, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN);

	/* the first-load count ends: GO, then the round, then ENDED */
	CHECK(MpMarkTick(1, 0) == MP_MARK_EV_BEGIN);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_LIVE);
	CHECK(MpMarkTick(2 * MIN_MS, 0) == MP_MARK_EV_END);

	/* the SAME map, after the intermission: the shorter per-round count */
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(!MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == MP_MARK_COUNTDOWN_MS);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 0) == MP_MARK_EV_BEGIN);

	/* a new map: everyone is kicked to rejoin (a dedicated server's vote) ->
	 * the live round ends (the holder leaving ends it at once, nobody marked),
	 * and the next startable group gets the first-load count */
	Present(one, 0, 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_ENDED && MpMarkMarked() == -1);
	CHECK(MpMarkTick(33, 0) == 0);
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	Present(two, 2, 0);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);

	/* players coming back during an intermission after a new map: first-load again */
	CHECK(MpMarkTick(5 * 1000, 0) == MP_MARK_EV_BEGIN);
	CHECK(MpMarkTick(2 * MIN_MS, 0) == MP_MARK_EV_END);
	Present(one, 0, 0);
	Present(two, 2, 0);
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst());

	/* the mirror */
	len = MpMarkBuild(buf, (int)sizeof(buf));
	MpMarkReset(MP_GAMEMODE_TAKEADRIDE, 3);
	CHECK(MpMarkApply(buf, len) == 1);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN && MpMarkCountdownIsFirst());
	CHECK(MpMarkCountdownMs(1000) == 5 * 1000 - 1000);
}

/* A joiner who arrives during WAITING is counted in the wait, not mid-round. */
static void CheckJoinDuringWait(void)
{
	int one[] = { 3 };
	int two[] = { 3, 6 };
	int three[] = { 3, 6, 8 };

	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, 2);
	Present(one, 1, 0);
	Ready(one, 1);
	CHECK(MpMarkTick(1000, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);

	/* a second player joins without a car: still waiting */
	Present(two, 2, 0);
	CHECK(MpMarkTick(33, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	CHECK(!MpMarkPlayerReady(6));

	/* their car is in: the first-load countdown starts */
	Ready(two, 2);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst());

	/* a third joins during the wait after an abort, still counted */
	Present(three, 3, 0);
	CHECK(MpMarkTick(33, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	Ready(three, 3);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);
}

/* A joiner who arrives during COUNTDOWN without a car aborts it; after they
 * pick a car the same kind of countdown starts again. After GO, a later-round
 * countdown aborted the same way retries the shorter count, not the first-load. */
static void CheckJoinDuringCountdown(void)
{
	int two[] = { 3, 6 };
	int three[] = { 3, 6, 8 };

	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, 2);
	Present(two, 2, 0);
	Ready(two, 2);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkTick(2000, 0) == 0 && MpMarkCountdownIsFirst());

	Present(three, 3, 0);
	CHECK(MpMarkTick(33, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkCountdownMs(0) == 0);

	Ready(three, 3);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == 5 * 1000);
	CHECK(MpMarkTick(5 * 1000, 0) == MP_MARK_EV_BEGIN);

	/* later round: abort mid-count, retry is the per-round length */
	CHECK(MpMarkTick(2 * MIN_MS, 0) == MP_MARK_EV_END);
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(!MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == MP_MARK_COUNTDOWN_MS);
	Present(two, 2, 0);	/* 8 left -- still two, startable, count continues? 8 left means two remain, both ready */
	/* drop to a joiner-not-ready by adding 8 back without ready */
	Present(three, 3, 0);
	Ready(two, 2);
	CHECK(MpMarkTick(33, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	Ready(three, 3);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(!MpMarkCountdownIsFirst() && MpMarkCountdownMs(0) == MP_MARK_COUNTDOWN_MS);
}

/* 0.23.4: Car Bomb Tag leavers, and Cops n Robbers on the getaway's real damage. */
static void CheckLeaversAndDamage(void)
{
	int three[] = { 1, 4, 6 };
	int two46[] = { 4, 6 };
	int two14[] = { 1, 4 };
	int one4[] = { 4 };
	int cop = 0, ev = 0, holder;

	/* the holder leaves with two left: a random new holder, a 12 s grace (no
	 * pass, and the clock raised to 12 s when less was left) */
	MpMarkReset(MP_GAMEMODE_BOMBTAG, 1);
	AllReady();
	Present(three, 3, 0);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkTick(5 * 1000, 0) == MP_MARK_EV_BEGIN && MpMarkMarked() == 1);
	MpMarkTick(MIN_MS - 5000, 0);
	CHECK(MpMarkClockMs(-1, 0) == 5000);
	Present(two46, 2, 1);
	holder = MpMarkMarked();
	CHECK(holder == 6 && MpMarkPhase() == MP_MARK_PHASE_LIVE);
	CHECK(MpMarkClockMs(-1, 0) == MP_MARK_BOMB_GRACE_MS);
	MpMarkTick(MP_MARK_BOMB_GRACE_MS - 1, 0);
	CHECK(MpMarkContact(4, 6) == 0 && MpMarkMarked() == 6);	/* still the grace */
	CHECK(MpMarkPhase() == MP_MARK_PHASE_LIVE);
	CHECK(MpMarkTick(1, 0) == MP_MARK_EV_END && MpMarkMarked() == 6);	/* 0:00: the holder */

	/* the holder leaves with one left: void, nobody marked, no bomb */
	MpMarkReset(MP_GAMEMODE_BOMBTAG, 1);
	AllReady();
	Present(two14, 2, 0);
	MpMarkTick(0, 0);
	CHECK(MpMarkTick(5 * 1000, 0) == MP_MARK_EV_BEGIN && MpMarkMarked() == 1);
	Present(one4, 1, 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_ENDED && MpMarkMarked() == -1 && MpMarkWinner() == -1);
	CHECK(MpMarkClockMs(-1, 0) > 0);	/* not the clock: no 0:00 */

	/* the OTHER player leaves the holder alone: void too */
	MpMarkReset(MP_GAMEMODE_BOMBTAG, 1);
	AllReady();
	Present(two14, 2, 0);
	MpMarkTick(0, 0);
	MpMarkTick(5 * 1000, 0);
	CHECK(MpMarkMarked() == 1);
	{
		int only1[] = { 1 };
		Present(only1, 1, 0);
	}
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_END && MpMarkMarked() == -1);

	/* Cops n Robbers: the meter from damage, the cop in contact credited */
	MpMarkReset(MP_GAMEMODE_COPSROBBERS, 1);
	AllReady();
	Present(three, 3, 0);
	MpMarkTick(0, 0);
	CHECK(MpMarkTick(5 * 1000, 0) == MP_MARK_EV_BEGIN && MpMarkMarked() == 1);
	CHECK(MpMarkDamageLive() == 0);
	CHECK(MpMarkHit(1, 4, 10, &ev) == 50);		/* no report yet: the old contact score */
	CHECK(MpMarkDamage(4, 100, 30000, &cop, &ev) == 0);	/* only the getaway reports */
	CHECK(MpMarkDamage(1, 0, 30000, &cop, &ev) == 0 && MpMarkDamageLive() == 1);
	MpMarkTick(1000, 0);
	CHECK(MpMarkHit(1, 6, 20, &ev) == 0);		/* names the cop, scores nothing now */
	CHECK(MpMarkDamage(1, 3000, 30000, &cop, &ev) == 100 && cop == 6 && ev == 0);
	CHECK(MpMarkHeat() == 150);
	MpMarkTick(MP_MARK_CNR_CREDIT_MS + 1, 0);
	CHECK(MpMarkDamage(1, 6000, 30000, &cop, &ev) == 100 && cop == -1);	/* too long ago */
	CHECK(MpMarkDamage(1, 1000, 30000, &cop, &ev) == 0 && MpMarkHeat() == 250);	/* repaired */
	CHECK(MpMarkHit(4, 1, 5, &ev) == 0);
	CHECK(MpMarkDamage(1, 30000, 30000, &cop, &ev) == MP_MARK_CNR_BUST_POINTS - 250 && cop == 4);
	CHECK(ev == MP_MARK_EV_END && MpMarkPhase() == MP_MARK_PHASE_ENDED && MpMarkWinner() == -1);
	CHECK(MpMarkHeat() == MP_MARK_CNR_BUST_POINTS);
}

/* Proto 24: end reasons, late joiners benched until the next round, out. */
static void CheckRoundV2(void)
{
	int two25[] = { 2, 5 }, three257[] = { 2, 5, 7 }, two27[] = { 2, 7 };
	int three146[] = { 1, 4, 6 }, two14[] = { 1, 4 };
	unsigned char buf[SNAP_MAX];
	MP_MARK h;
	MP_MARK_ENTRY e;
	int len, ev = 0, cop = -1;

	/* Fox N Hounds: a join after GO waits; the fox leaving the waiter alone is void */
	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, 3);
	AllReady();
	Present(two25, 2, 0);
	MpMarkTick(0, 0);
	CHECK(MpMarkTick(5 * 1000, 1) == MP_MARK_EV_BEGIN && MpMarkMarked() == 5);
	CHECK(MpMarkEndReason() == MP_MARK_END_NONE);
	Present(three257, 3, 0);
	CHECK(MpMarkPresent(7) && !MpMarkInRound(7) && MpMarkBench(7) == MP_MARK_ENTRY_F_WAITING);
	MpMarkTick(3000, 0);
	CHECK(MpMarkContact(5, 7) == 0 && MpMarkMarked() == 5);	/* not in this round */
	len = MpMarkBuild(buf, (int)sizeof(buf));
	CHECK(len == (int)(sizeof(MP_MARK) + 3 * sizeof(MP_MARK_ENTRY)));
	memcpy(&e, buf + sizeof(MP_MARK) + 2 * sizeof(e), sizeof(e));
	CHECK(e.playerId == 7 && (e.flags & MP_MARK_ENTRY_F_WAITING) && (e.flags & MP_MARK_ENTRY_F_READY));
	Present(two27, 2, 0);	/* the fox leaves: one in the round, one waiting */
	CHECK(MpMarkPhase() == MP_MARK_PHASE_ENDED && MpMarkMarked() == -1 && MpMarkWinner() == -1);
	CHECK(MpMarkEndReason() == MP_MARK_END_VOID);
	len = MpMarkBuild(buf, (int)sizeof(buf));
	memcpy(&h, buf, sizeof(h));
	CHECK(h.endReason == MP_MARK_END_VOID && h.flags == 0 && h.spare == 0);
	/* the client mirror */
	CHECK(MpMarkApply(buf, len) == 1 && MpMarkEndReason() == MP_MARK_END_VOID && MpMarkBench(7) == MP_MARK_ENTRY_F_WAITING);

	/* ...the next countdown puts the waiter in */
	MpMarkReset(MP_GAMEMODE_FOXHOUNDS, 1);
	AllReady();
	Present(two25, 2, 0);
	MpMarkTick(0, 0);
	MpMarkTick(5 * 1000, 0);
	Present(three257, 3, 0);
	CHECK(MpMarkBench(7) == MP_MARK_ENTRY_F_WAITING);
	CHECK(MpMarkTick(60 * 1000, 0) == MP_MARK_EV_END && MpMarkEndReason() == MP_MARK_END_CLOCK);
	CHECK(MpMarkWinner() >= 0);
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkInRound(7) && MpMarkBench(7) == 0 && MpMarkEndReason() == MP_MARK_END_NONE);

	/* Take a Ride never benches (no referee); a join during COUNTDOWN is in */
	MpMarkReset(MP_GAMEMODE_BOMBTAG, 1);
	AllReady();
	Present(two14, 2, 0);
	MpMarkTick(0, 0);
	Present(three146, 3, 0);
	CHECK(MpMarkInRound(6));
	MpMarkTick(5 * 1000, 0);
	CHECK(MpMarkTick(60 * 1000, 0) == MP_MARK_EV_END && MpMarkEndReason() == MP_MARK_END_CLOCK);

	/* Cops n Robbers: escape, bust (damage flag on the wire), leave, out */
	MpMarkReset(MP_GAMEMODE_COPSROBBERS, 1);
	AllReady();
	Present(three146, 3, 0);
	MpMarkTick(0, 0);
	CHECK(MpMarkTick(5 * 1000, 0) == MP_MARK_EV_BEGIN && MpMarkMarked() == 1);
	CHECK(MpMarkEscape() == MP_MARK_EV_END && MpMarkEndReason() == MP_MARK_END_ESCAPE && MpMarkWinner() == 1);

	MpMarkReset(MP_GAMEMODE_COPSROBBERS, 1);
	AllReady();
	Present(three146, 3, 0);
	MpMarkTick(0, 0);
	MpMarkTick(5 * 1000, 0);
	CHECK(MpMarkDamage(1, 100, 30000, &cop, &ev) > 0 && (MpMarkFlags() & MP_MARK_F_DAMAGE));
	len = MpMarkBuild(buf, (int)sizeof(buf));
	memcpy(&h, buf, sizeof(h));
	CHECK(h.flags == MP_MARK_F_DAMAGE && h.endReason == MP_MARK_END_NONE);
	CHECK(MpMarkDamage(1, 30000, 30000, &cop, &ev) > 0 && ev == MP_MARK_EV_END);
	CHECK(MpMarkEndReason() == MP_MARK_END_BUST);

	MpMarkReset(MP_GAMEMODE_COPSROBBERS, 1);
	AllReady();
	Present(three146, 3, 0);
	MpMarkTick(0, 0);
	MpMarkTick(5 * 1000, 0);
	{
		int cops46[] = { 4, 6 };
		Present(cops46, 2, 0);	/* the getaway leaves */
	}
	CHECK(MpMarkPhase() == MP_MARK_PHASE_ENDED && MpMarkEndReason() == MP_MARK_END_LEAVE);

	/* MpMarkOut: a cop out of three stays benched, the round goes on; the
	 * getaway out is caught; out outside LIVE is nothing */
	MpMarkReset(MP_GAMEMODE_COPSROBBERS, 1);
	AllReady();
	Present(three146, 3, 0);
	MpMarkTick(0, 0);
	CHECK(MpMarkOut(4, 0) == 0 && MpMarkInRound(4));	/* COUNTDOWN: nothing */
	MpMarkTick(5 * 1000, 0);
	CHECK(MpMarkOut(4, 0) == 0 && MpMarkBench(4) == MP_MARK_ENTRY_F_OUT && !MpMarkInRound(4));
	CHECK(MpMarkOut(4, 0) == 0);	/* already out */
	CHECK(MpMarkHit(1, 4, 20, &ev) == 0);	/* out of the round: no hit */
	CHECK(MpMarkTick(33, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_LIVE);
	CHECK(MpMarkOut(1, 0) == MP_MARK_EV_END && MpMarkEndReason() == MP_MARK_END_BUST);

	/* Car Bomb Tag: the holder out with two left in -> a new holder with the grace */
	MpMarkReset(MP_GAMEMODE_BOMBTAG, 1);
	AllReady();
	Present(three146, 3, 0);
	MpMarkTick(0, 0);
	MpMarkTick(5 * 1000, 0);
	CHECK(MpMarkMarked() == 1);
	CHECK(MpMarkOut(1, 0) == MP_MARK_EV_MOVED && MpMarkMarked() == 4);
	CHECK(MpMarkOut(6, 0) == 0);	/* one left in the round: void on the tick */
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_END && MpMarkEndReason() == MP_MARK_END_VOID && MpMarkMarked() == -1);
}

static int RunScript(void)
{
	char line[256];
	int step = 0;

	while (fgets(line, sizeof(line), stdin) != NULL)
	{
		char* tok = strtok(line, " \t\r\n");
		unsigned char buf[SNAP_MAX];
		int ev = 0, len, i;

		if (tok == NULL)
			continue;

		if (strcmp(tok, "reset") == 0)
		{
			int mode = atoi(strtok(NULL, " \t\r\n"));
			int minutes = atoi(strtok(NULL, " \t\r\n"));
			MpMarkReset(mode, minutes);
		}
		else if (strcmp(tok, "sync") == 0)
		{
			int present[MP_MAX_PLAYERS];
			unsigned rnd = (unsigned)strtoul(strtok(NULL, " \t\r\n"), NULL, 10);
			char* id;

			memset(present, 0, sizeof(present));
			while ((id = strtok(NULL, " \t\r\n")) != NULL)
				present[atoi(id)] = 1;
			ev = MpMarkSync(present, rnd);
		}
		else if (strcmp(tok, "ready") == 0)
		{
			int ready[MP_MAX_PLAYERS];
			char* id;

			memset(ready, 0, sizeof(ready));
			while ((id = strtok(NULL, " \t\r\n")) != NULL)
				ready[atoi(id)] = 1;
			MpMarkReady(ready);
		}
		else if (strcmp(tok, "tick") == 0)
		{
			int ms = atoi(strtok(NULL, " \t\r\n"));
			unsigned rnd = (unsigned)strtoul(strtok(NULL, " \t\r\n"), NULL, 10);
			ev = MpMarkTick(ms, rnd);
		}
		else if (strcmp(tok, "contact") == 0)
		{
			int a = atoi(strtok(NULL, " \t\r\n"));
			int b = atoi(strtok(NULL, " \t\r\n"));
			ev = MpMarkContact(a, b);
		}
		else if (strcmp(tok, "hit") == 0)
		{
			int a = atoi(strtok(NULL, " \t\r\n"));
			int b = atoi(strtok(NULL, " \t\r\n"));
			int strength = atoi(strtok(NULL, " \t\r\n"));
			int hev = 0;
			int pts = MpMarkHit(a, b, strength, &hev);
			ev = pts * 256 + hev;	/* points and the event, one number */
		}
		else if (strcmp(tok, "drive") == 0)
		{
			int ms = atoi(strtok(NULL, " \t\r\n"));
			int speed = atoi(strtok(NULL, " \t\r\n"));
			ev = MpMarkDrive(ms, speed);
		}
		else if (strcmp(tok, "escape") == 0)
			ev = MpMarkEscape();
		else if (strcmp(tok, "out") == 0)
		{
			int id = atoi(strtok(NULL, " \t\r\n"));
			unsigned rnd = (unsigned)strtoul(strtok(NULL, " \t\r\n"), NULL, 10);
			ev = MpMarkOut(id, rnd);
		}
		else if (strcmp(tok, "damage") == 0)
		{
			int from = atoi(strtok(NULL, " \t\r\n"));
			int dmg = atoi(strtok(NULL, " \t\r\n"));
			int maxd = atoi(strtok(NULL, " \t\r\n"));
			int cop = -1, dev = 0;
			int pts = MpMarkDamage(from, dmg, maxd, &cop, &dev);
			ev = pts * 65536 + (cop + 1) * 256 + dev;	/* points, credited cop, event */
		}
		else
			continue;

		len = MpMarkBuild(buf, (int)sizeof(buf));
		printf("STEP %d %d ", step++, ev);
		for (i = 0; i < len; i++)
			printf("%02x", buf[i]);
		printf("\n");
	}

	return 0;
}

int main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--script") == 0)
		return RunScript();

	CheckTag();
	CheckMinutes();
	CheckTakeARide();
	CheckFoxAndHounds();
	CheckBombTag();
	CheckApply();
	CheckRoundStart();
	CheckPreGame();
	CheckJoinDuringWait();
	CheckJoinDuringCountdown();
	CheckLeaversAndDamage();
	CheckRoundV2();

	printf("mp_mark: %d check(s), %d failure(s)\n", gChecks, gFails);
	return gFails == 0 ? 0 : 1;
}
