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
	CHECK(MpMarkTick(1000, 0) == 0);
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

	/* one player: nothing starts, nothing ticks */
	Present(one, 1, 0);
	CHECK(MpMarkTick(5000, 0) == 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_WAITING && MpMarkMarked() == -1);
	CHECK(MpMarkClockMs(2, 0) == 3 * MIN_MS);

	/* the second arrives: live, the random pick is the fox */
	Present(two, 2, 0);
	CHECK(MpMarkTick(33, 1) == MP_MARK_EV_BEGIN);	/* rnd 1 of {2,5} -> 5 */
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
	CHECK(h.sharedDs == 0 && h.reserved == 0);
	memcpy(&e, buf + sizeof(h), sizeof(e));
	CHECK(e.playerId == 2 && e.remainDs == (3 * MIN_MS - 2000) / 100);	/* frozen since it passed back */

	/* first to 0:00 wins */
	MpMarkTick(3 * MIN_MS - 2000 - 2500 - 1, 0);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_LIVE);
	CHECK(MpMarkClockMs(fox, 0) == 1);
	CHECK(MpMarkTick(1, 0) == MP_MARK_EV_END);
	CHECK(MpMarkPhase() == MP_MARK_PHASE_ENDED && MpMarkWinner() == fox);
	CHECK(MpMarkClockMs(fox, 0) == 0);
	CHECK(MpMarkContact(hound, fox) == 0);	/* over: no tags */

	/* the intermission, then the next round with fresh clocks */
	CHECK(MpMarkTick(MP_MARK_INTERMISSION_MS - 1, 0) == 0);
	CHECK(MpMarkTick(1, 2) == MP_MARK_EV_BEGIN);	/* rnd 2 of {2,5,7} -> 7 */
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
	Present(two, 2, 0);
	CHECK(MpMarkTick(0, 0) == MP_MARK_EV_BEGIN);	/* rnd 0 -> 1 */
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
	CHECK(h.sharedDs == (MIN_MS - 3000) / 100 && h.count == 3);
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
	Present(two, 2, 0);
	MpMarkTick(10, 1);
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

	printf("mp_mark: %d check(s), %d failure(s)\n", gChecks, gFails);
	return gFails == 0 ? 0 : 1;
}
