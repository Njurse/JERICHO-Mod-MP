/*
 * test_mp_round.cpp -- the standalone check for the round state (mp_round.c) and its
 * wire format (MP_ROUND / MP_ROUND_ENTRY, MP_TAG_ROUND, proto 22).
 *
 * Asserts the Take a Ride lifecycle -- idle until START, then one live round with
 * every player playing, no team, score 0; late joiners playing; leavers removed --
 * the snapshot bytes, the client's apply (and its refusals), and the defaults a
 * client with no snapshot answers. Prints the canonical Take a Ride snapshot as
 * "SNAPSHOT <hex>" so run_mode_check.py can compare it with tools/mp_test.py
 * build_round byte for byte.
 *
 * Builds against mp_round.c and mp_mode.c alone. Named .cpp for the same reason as
 * test_mp_mode.cpp: the mod project globs **.c into the DLL.
 */
#include "mp_round.h"
#include "mp_mode.h"
#include "mp_proto.h"

#include <stdio.h>
#include <string.h>

static int gFails = 0;
static int gChecks = 0;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* The wire, pinned. */
static_assert(MP_PROTO_VERSION == 23, "the round state came with proto 22; 23 added JPMK beside it");
static_assert(sizeof(MP_ROUND) == 8, "MP_ROUND");
static_assert(sizeof(MP_ROUND_ENTRY) == 8, "MP_ROUND_ENTRY");
static_assert(sizeof(MP_ROSTER_ENTRY) == 32, "the roster entry is untouched");
static_assert(sizeof(MP_WELCOME) == 18, "the welcome is untouched");
static_assert(sizeof(MP_SESSION) == 12, "the session is untouched");
static_assert(sizeof(MP_BEACON) == 50, "the beacon is untouched");
static_assert(MP_ROUND_PLAYER_PLAYING == 0, "a zeroed row reads as playing");

#define SNAP_MAX ((int)(sizeof(MP_ROUND) + MP_MAX_PLAYERS * sizeof(MP_ROUND_ENTRY)))

static void SetPresent(int* present, const int* ids, int n)
{
	int i;

	memset(present, 0, sizeof(int) * MP_MAX_PLAYERS);
	for (i = 0; i < n; i++)
		present[ids[i]] = 1;
}

static void CheckTag(void)
{
	static const char* const kOld[] = {
		"JPHL", "JPWL", "JPRJ", "JPST", "JPIN", "JPCS", "JPPN", "JPPO", "JPCH", "JPLV",
		"JPKK", "JPRS", "JPHI", "JPPD", "JPCL", "JPCC", "JPTF", "JPCX", "JPSX", "JPSS", "JPSW"
	};
	size_t i;

	CHECK(strlen(MP_TAG_ROUND) == 4 && memcmp(MP_TAG_ROUND, "JP", 2) == 0);
	for (i = 0; i < sizeof(kOld) / sizeof(kOld[0]); i++)
		CHECK(memcmp(MP_TAG_ROUND, kOld[i], 4) != 0);
}

/* A client that has had no snapshot: Take a Ride. */
static void CheckDefaults(void)
{
	int i;

	MpRoundReset();
	CHECK(!MpRoundHaveState());
	CHECK(MpRoundNumber() == 0);
	CHECK(MpRoundPhase() == MP_ROUND_PHASE_IDLE);
	CHECK(!MpRoundIsLive());

	for (i = -1; i <= MP_MAX_PLAYERS; i++)
	{
		CHECK(!MpRoundPlayerPresent(i));
		CHECK(MpRoundPlayerState(i) == MP_ROUND_PLAYER_PLAYING);
		CHECK(MpRoundPlayerScore(i) == 0);
		CHECK(MpRoundPlayerTeam(i) == -1);
	}
}

static void CheckTakeARideLifecycle(void)
{
	int present[MP_MAX_PLAYERS];
	unsigned char buf[SNAP_MAX];
	MP_ROUND h;
	MP_ROUND_ENTRY e;
	unsigned rev;
	int ids3[] = { 0, 1, 3 };
	int ids4[] = { 0, 1, 3, 5 };
	int ids3b[] = { 0, 3, 5 };
	int i, len;

	MpRoundReset();

	/* the lobby: present, waiting, no round */
	SetPresent(present, ids3, 3);
	CHECK(MpRoundSyncPresent(present, MP_GAMEMODE_TAKEADRIDE) == 1);
	CHECK(MpRoundSyncPresent(present, MP_GAMEMODE_TAKEADRIDE) == 0);	/* idempotent */
	CHECK(MpRoundHaveState());
	CHECK(MpRoundNumber() == 0 && MpRoundPhase() == MP_ROUND_PHASE_IDLE);
	CHECK(MpRoundPlayerState(1) == MP_ROUND_PLAYER_WAITING);

	/* START: round 1, live, everyone playing, no team, score 0 */
	rev = MpRoundRevision();
	MpRoundBegin(MP_GAMEMODE_TAKEADRIDE);
	CHECK(MpRoundRevision() != rev);
	CHECK(MpRoundNumber() == 1 && MpRoundIsLive() && MpRoundMode() == MP_GAMEMODE_TAKEADRIDE);

	for (i = 0; i < 3; i++)
	{
		CHECK(MpRoundPlayerPresent(ids3[i]));
		CHECK(MpRoundPlayerState(ids3[i]) == MP_ROUND_PLAYER_PLAYING);
		CHECK(MpRoundPlayerScore(ids3[i]) == 0);
		CHECK(MpRoundPlayerTeam(ids3[i]) == -1);
		CHECK(MpModePlayerTeam(MP_GAMEMODE_TAKEADRIDE, ids3[i]) == -1);
	}
	CHECK(!MpRoundPlayerPresent(2));

	/* the snapshot bytes */
	len = MpRoundBuild(buf, (int)sizeof(buf));
	CHECK(len == (int)(sizeof(MP_ROUND) + 3 * sizeof(MP_ROUND_ENTRY)));
	CHECK(MpRoundBuild(buf, len - 1) == 0);		/* too small: nothing */
	len = MpRoundBuild(buf, (int)sizeof(buf));
	memcpy(&h, buf, sizeof(h));
	CHECK(h.gamemode == MP_GAMEMODE_TAKEADRIDE && h.phase == MP_ROUND_PHASE_LIVE &&
		h.round == 1 && h.count == 3);
	CHECK(h.reserved[0] == 0 && h.reserved[1] == 0 && h.reserved[2] == 0);

	for (i = 0; i < 3; i++)
	{
		memcpy(&e, buf + sizeof(h) + i * sizeof(e), sizeof(e));
		CHECK(e.playerId == ids3[i]);		/* ascending id */
		CHECK(e.state == MP_ROUND_PLAYER_PLAYING);
		CHECK(e.team == MP_ROUND_TEAM_NONE);
		CHECK(e.reserved == 0);
		CHECK(e.score == 0);
	}

	/* a late joiner is live (Take a Ride), and only that is a change */
	SetPresent(present, ids4, 4);
	rev = MpRoundRevision();
	CHECK(MpRoundSyncPresent(present, MP_GAMEMODE_TAKEADRIDE) == 1);
	CHECK(MpRoundRevision() != rev);
	CHECK(MpRoundPlayerState(5) == MP_ROUND_PLAYER_PLAYING);
	CHECK(MpRoundPlayerTeam(5) == -1 && MpRoundPlayerScore(5) == 0);
	CHECK(MpRoundNumber() == 1 && MpRoundIsLive());	/* the round goes on */

	/* a leaver loses their row */
	SetPresent(present, ids3b, 3);
	CHECK(MpRoundSyncPresent(present, MP_GAMEMODE_TAKEADRIDE) == 1);
	CHECK(!MpRoundPlayerPresent(1));
	CHECK(MpRoundPlayerState(1) == MP_ROUND_PLAYER_PLAYING);	/* the default again */
	CHECK(MpRoundNumber() == 1 && MpRoundIsLive());

	/* end + begin: the next round, scores reset (no Take a Ride path does this yet) */
	MpRoundEnd();
	CHECK(MpRoundPhase() == MP_ROUND_PHASE_ENDED);
	rev = MpRoundRevision();
	MpRoundEnd();
	CHECK(MpRoundRevision() == rev);		/* ending twice is no change */
	MpRoundBegin(MP_GAMEMODE_TAKEADRIDE);
	CHECK(MpRoundNumber() == 2 && MpRoundIsLive());
	CHECK(MpRoundPlayerState(5) == MP_ROUND_PLAYER_PLAYING);

	/* NULL present-set: nothing */
	CHECK(MpRoundSyncPresent(NULL, 0) == 0);
}

static void CheckApply(void)
{
	int present[MP_MAX_PLAYERS];
	unsigned char buf[SNAP_MAX], again[SNAP_MAX];
	int ids[] = { 0, 2, 15 };
	MP_ROUND h;
	MP_ROUND_ENTRY e;
	int len, len2;

	/* host -> bytes -> a fresh client -> the same bytes */
	MpRoundReset();
	SetPresent(present, ids, 3);
	MpRoundSyncPresent(present, 0);
	MpRoundBegin(0);
	len = MpRoundBuild(buf, (int)sizeof(buf));

	MpRoundReset();
	CHECK(MpRoundApply(buf, len) == 1);
	CHECK(MpRoundHaveState() && MpRoundNumber() == 1 && MpRoundIsLive());
	CHECK(MpRoundPlayerPresent(15) && MpRoundPlayerState(15) == MP_ROUND_PLAYER_PLAYING);
	CHECK(MpRoundPlayerTeam(2) == -1 && MpRoundPlayerScore(0) == 0);
	len2 = MpRoundBuild(again, (int)sizeof(again));
	CHECK(len2 == len && memcmp(buf, again, len) == 0);

	/* refused: short header, short rows, a count past MP_MAX_PLAYERS -- copy kept */
	CHECK(MpRoundApply(buf, (int)sizeof(MP_ROUND) - 1) == 0);
	CHECK(MpRoundApply(buf, len - 1) == 0);
	CHECK(MpRoundApply(NULL, 8) == 0);
	memcpy(&h, buf, sizeof(h));
	h.count = MP_MAX_PLAYERS + 1;
	{
		unsigned char big[sizeof(MP_ROUND) + (MP_MAX_PLAYERS + 1) * sizeof(MP_ROUND_ENTRY)];

		memset(big, 0, sizeof(big));
		memcpy(big, &h, sizeof(h));
		CHECK(MpRoundApply(big, (int)sizeof(big)) == 0);
	}
	CHECK(MpRoundPlayerPresent(15) && MpRoundNumber() == 1);

	/* a whole replacement: rows not named are gone; bad ids and teams are ignored */
	memset(&h, 0, sizeof(h));
	h.phase = MP_ROUND_PHASE_ENDED;
	h.round = 7;
	h.count = 2;
	memcpy(buf, &h, sizeof(h));
	memset(&e, 0, sizeof(e));
	e.playerId = 3; e.state = MP_ROUND_PLAYER_SPECTATING; e.team = 9; e.score = -42;
	memcpy(buf + sizeof(h), &e, sizeof(e));
	e.playerId = 200; e.state = MP_ROUND_PLAYER_WAITING; e.team = 0; e.score = 5;
	memcpy(buf + sizeof(h) + sizeof(e), &e, sizeof(e));
	CHECK(MpRoundApply(buf, (int)(sizeof(h) + 2 * sizeof(e))) == 1);
	CHECK(MpRoundNumber() == 7 && MpRoundPhase() == MP_ROUND_PHASE_ENDED);
	CHECK(!MpRoundPlayerPresent(15) && !MpRoundPlayerPresent(0));
	CHECK(MpRoundPlayerState(3) == MP_ROUND_PLAYER_SPECTATING);
	CHECK(MpRoundPlayerScore(3) == -42);		/* signed */
	CHECK(MpRoundPlayerTeam(3) == -1);		/* team 9 is past MP_MODE_MAX_TEAMS */
}

/* The bytes tools/mp_test.py build_round must produce for the same state. */
static void PrintCanonical(void)
{
	int present[MP_MAX_PLAYERS];
	unsigned char buf[SNAP_MAX];
	int ids[] = { 0, 1 };
	int i, len;

	MpRoundReset();
	SetPresent(present, ids, 2);
	MpRoundSyncPresent(present, MP_GAMEMODE_TAKEADRIDE);
	MpRoundBegin(MP_GAMEMODE_TAKEADRIDE);
	len = MpRoundBuild(buf, (int)sizeof(buf));

	printf("SNAPSHOT ");
	for (i = 0; i < len; i++)
		printf("%02x", buf[i]);
	printf("\n");
}

int main(void)
{
	CheckTag();
	CheckDefaults();
	CheckTakeARideLifecycle();
	CheckApply();
	CheckDefaults();	/* a reset forgets everything */
	PrintCanonical();

	printf("mp_round: %d check(s), %d failure(s)\n", gChecks, gFails);
	return gFails == 0 ? 0 : 1;
}
