/*
 * test_mp_mark.cpp -- the standalone check for the marked-player referee (mp_mark.c)
 * and its wire format (MP_MARK / MP_MARK_ENTRY, MP_TAG_MARK, proto 23).
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

static_assert(MP_PROTO_VERSION == 23, "the marked-player state is proto 23");
static_assert(sizeof(MP_MARK) == 12, "MP_MARK");
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

	/* one player: the map's pre-game lobby, then nothing starts, nothing ticks */
	Present(one, 1, 0);
	CHECK(MpMarkTick(5000, 0) == MP_MARK_EV_LOBBY);
	CHECK(MpMarkTick(MP_MARK_LOBBY_MS, 0) == 0);
	CHECK(MpMarkTick(5000, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1);
	CHECK(MpMarkClockMs(2, 0) == 3 * MIN_MS);

	/* the second arrives: the countdown, then live, the random pick is the fox */
	Present(two, 2, 0);
	CHECK(MpMarkTick(33, 1) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 1) == MP_MARK_EV_BEGIN);	/* rnd 1 of {2,5} -> 5 */
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

	/* down to one player: the round ends with no winner, then waits */
	Present(one, 1, 0);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_END);
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
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_LOBBY);
	CHECK(MpMarkTick(MP_MARK_LOBBY_MS, 0) == MP_MARK_EV_COUNTDOWN);	/* lobby over, cars in */
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS);
	CHECK(MpMarkTick(1000, 0) == 0);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS);		/* the countdown is not the bomb's time */
	CHECK(MpMarkContact(1, 4) == 0);		/* nor a pass */
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS - 1000, 0) == MP_MARK_EV_BEGIN);	/* rnd 0 -> 1 */
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
	MpMarkTick(10, 1);
	MpMarkTick(MP_MARK_LOBBY_MS, 1);
	MpMarkTick(MP_MARK_COUNTDOWN_MS, 1);
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

	/* two present, one still in the car select: the lobby, then nothing starts */
	Ready(only3, 1);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_LOBBY);
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
	CHECK(MpMarkCountdownMs(0) == MP_MARK_COUNTDOWN_MS);
	CHECK(MpMarkCountdownMs(1500) == MP_MARK_COUNTDOWN_MS - 1500);
	len = MpMarkBuild(buf, (int)sizeof(buf));
	memcpy(&h, buf, sizeof(h));
	CHECK(h.phase == MP_MARK_PHASE_COUNTDOWN && h.countdownDs == MP_MARK_COUNTDOWN_MS / 100);
	memcpy(&e, buf + sizeof(h) + sizeof(e), sizeof(e));
	CHECK(e.flags == MP_MARK_ENTRY_F_READY);

	/* the countdown touches no clock and allows no tag */
	CHECK(MpMarkTick(1000, 0) == 0);
	CHECK(MpMarkContact(3, 6) == 0);
	CHECK(MpMarkClockMs(3, 0) == 2 * MIN_MS && MpMarkClockMs(6, 0) == 2 * MIN_MS);
	CHECK(MpMarkClockMs(3, 900) == 2 * MIN_MS);	/* a client does not run it on either */
	CHECK(MpMarkCountdownMs(0) == MP_MARK_COUNTDOWN_MS - 1000);

	/* a joiner arrives before GO without a car: back to waiting for them */
	Present(three, 3, 0);
	CHECK(MpMarkTick(33, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkCountdownMs(0) == 0);
	CHECK(MpMarkTick(30000, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);

	/* their car is in: a fresh countdown, the full length */
	Ready(three, 3);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkCountdownMs(0) == MP_MARK_COUNTDOWN_MS);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS - 1, 0) == 0);
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
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_LOBBY);
	CHECK(MpMarkTick(MP_MARK_LOBBY_MS, 0) == MP_MARK_EV_COUNTDOWN);
	Present(only3, 1, 0);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1);
	CHECK(MpMarkClockMs(-1, 0) == MIN_MS);

	/* the intermission leads into the same gate: a car missing -> WAITING ... */
	Present(two, 2, 0);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 0) == MP_MARK_EV_BEGIN);
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

/* THE PRE-GAME LOBBY (t509): after a map load, a 5-4-3-2-1 before the first round's
 * wait-for-cars / 3-2-1 / GO, once per map. */
static void CheckLobby(void)
{
	int two[] = { 1, 4 };
	int one[] = { 1 };
	unsigned char buf[SNAP_MAX];
	MP_MARK h;
	int len;

	/* Take a Ride: no lobby, ever */
	MpMarkReset(MP_GAMEMODE_TAKEADRIDE, 3);
	Present(two, 2, 0);
	CHECK(MpMarkTick(0, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkCountdownMs(0) == 0);

	/* a map load with nobody in it yet: nothing, until someone is there */
	MpMarkReset(MP_GAMEMODE_BOMBTAG, 2);
	AllReady();
	CHECK(MpMarkTick(10000, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	Present(one, 1, 0);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_LOBBY);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_LOBBY && MpMarkCountdownMs(0) == MP_MARK_LOBBY_MS);
	CHECK(MpMarkMarked() == -1);

	/* 5, 4, ...: no clock, no tag, even with two players in */
	Present(two, 2, 0);
	CHECK(MpMarkTick(1000, 0) == 0);
	CHECK(MpMarkCountdownMs(0) == MP_MARK_LOBBY_MS - 1000);
	CHECK(MpMarkCountdownMs(500) == MP_MARK_LOBBY_MS - 1500);
	CHECK(MpMarkClockMs(-1, 0) == 0 && MpMarkContact(1, 4) == 0);
	len = MpMarkBuild(buf, (int)sizeof(buf));
	memcpy(&h, buf, sizeof(h));
	CHECK(h.phase == MP_MARK_PHASE_LOBBY && h.countdownDs == (MP_MARK_LOBBY_MS - 1000) / 100);

	/* everyone leaves mid-lobby: it holds, full, until someone is back */
	Present(one, 0, 0);
	CHECK(MpMarkTick(2000, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_LOBBY);
	CHECK(MpMarkCountdownMs(0) == MP_MARK_LOBBY_MS);
	Present(two, 2, 0);
	CHECK(MpMarkTick(MP_MARK_LOBBY_MS - 1, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_LOBBY);

	/* the lobby ends: straight into the round start (every car is in) */
	CHECK(MpMarkTick(1, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN && MpMarkCountdownMs(0) == MP_MARK_COUNTDOWN_MS);
	CHECK(MpMarkClockMs(-1, 0) == 2 * MIN_MS);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 0) == MP_MARK_EV_BEGIN);
	CHECK(MpMarkTick(2 * MIN_MS, 0) == MP_MARK_EV_END);

	/* the SAME map, after the intermission: no lobby, the 3-2-1 again */
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == MP_MARK_EV_COUNTDOWN);
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 0) == MP_MARK_EV_BEGIN);

	/* a new map: everyone is kicked to rejoin (a dedicated server's vote) -> the
	 * live round ends, and the first one back gets the lobby again */
	Present(one, 0, 0);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_END);
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	Present(two, 2, 0);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_LOBBY);
	CHECK(MpMarkTick(MP_MARK_LOBBY_MS, 0) == MP_MARK_EV_COUNTDOWN);

	/* players coming back during an intermission after a new map: lobby first */
	CHECK(MpMarkTick(MP_MARK_COUNTDOWN_MS, 0) == MP_MARK_EV_BEGIN);
	CHECK(MpMarkTick(2 * MIN_MS, 0) == MP_MARK_EV_END);
	Present(one, 0, 0);
	Present(two, 2, 0);
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS, 0) == 0 && MpMarkPhase() == MP_MARK_PHASE_WAITING);
	CHECK(MpMarkTick(33, 0) == MP_MARK_EV_LOBBY);

	/* the mirror */
	len = MpMarkBuild(buf, (int)sizeof(buf));
	MpMarkReset(MP_GAMEMODE_TAKEADRIDE, 3);
	CHECK(MpMarkApply(buf, len) == 1);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_LOBBY && MpMarkCountdownMs(1000) == MP_MARK_LOBBY_MS - 1000);
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
	CheckLobby();

	printf("mp_mark: %d check(s), %d failure(s)\n", gChecks, gFails);
	return gFails == 0 ? 0 : 1;
}
