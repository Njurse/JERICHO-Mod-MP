/*
 * test_mp_mode.cpp -- the standalone check for the game-mode registry (mp_mode.c).
 *
 * Asserts that mode 0, Take a Ride, answers every decision point exactly as the
 * session behaved before modes existed, and that an unknown id behaves as mode 0.
 * Builds against mp_mode.c and mp_round.c alone (no engine, no session) -- run it with
 * tools/mode_check/run_mode_check.py, which finds a C++ compiler.
 *
 * Named .cpp, not .c, on purpose: the mod project globs **.c into the DLL and this
 * file has a main().
 */
#include "mp_mode.h"
#include "mp_round.h"
#include "mp_proto.h"

#include <stdio.h>
#include <string.h>

static int gFails = 0;
static int gChecks = 0;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void CheckTakeARideDescriptor(void)
{
	const MP_MODE_DESC* d = MpModeFind(MP_GAMEMODE_TAKEADRIDE);

	CHECK(MP_GAMEMODE_TAKEADRIDE == 0);
	CHECK(MpModeCount() >= 1);
	CHECK(MpModeAt(0) != NULL && MpModeAt(0)->id == MP_GAMEMODE_TAKEADRIDE);	/* row 0 */
	CHECK(MpModeAt(MpModeCount()) == NULL);
	CHECK(MpModeAt(-1) == NULL);
	CHECK(d != NULL);

	if (d == NULL)
		return;

	CHECK(strcmp(d->name, "TAKE A RIDE") == 0);	/* the string browsers always showed */
	CHECK(d->selectable == 1);
	CHECK(d->teamCount == 0);
	CHECK(d->teamColours == 0);
	CHECK(d->respawn == MP_MODE_RESPAWN_INSTANT);
	CHECK(d->respawnDelayFrames == 0);
	CHECK(d->lateJoin == MP_MODE_LATEJOIN_LIVE);
	CHECK(d->waitMinPlayers == 0 && d->waitForCars == 0);
	CHECK(d->firstCountdownSec == 0 && d->roundCountdownSec == 0);
	CHECK(MpModeHasPreGame(MP_GAMEMODE_TAKEADRIDE) == 0);
	CHECK(MpModeWaitMinPlayers(MP_GAMEMODE_TAKEADRIDE) == 0);
	CHECK(d->carChangeLocked == 0);
	CHECK(d->carPool == MP_MODE_CARPOOL_ALL);
	CHECK(d->spawn == MP_MODE_SPAWN_SESSION);
	CHECK(d->mapCycle == 0);
	CHECK(d->timer == MP_MODE_TIMER_NONE);		/* no clock, nobody marked */
	CHECK(d->music == MP_MODE_MUSIC_AUTO);		/* the engine's own music */
}

/* The proof-of-concept rows (v23): Fox N Hounds and Car Bomb Tag. Everything but
 * the clock and the music is Take a Ride's answer. */
static void CheckMarkModes(void)
{
	static const struct { int id; const char* name; int timer; } kRows[] = {
		{ MP_GAMEMODE_FOXHOUNDS, "FOX N HOUNDS", MP_MODE_TIMER_PERSONAL },
		{ MP_GAMEMODE_BOMBTAG,   "CAR BOMB TAG", MP_MODE_TIMER_SHARED },
	};
	size_t i;

	CHECK(MP_GAMEMODE_FOXHOUNDS == 1 && MP_GAMEMODE_BOMBTAG == 2);
	CHECK(MP_MODE_MUSIC_AUTO == 0 && MP_MODE_MUSIC_CALM == 1 && MP_MODE_MUSIC_PURSUIT == 2);	/* = JER_MUSIC_* */

	for (i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++)
	{
		const MP_MODE_DESC* d = MpModeFind(kRows[i].id);

		CHECK(d != NULL);
		if (d == NULL)
			continue;

		CHECK(strcmp(d->name, kRows[i].name) == 0);
		CHECK(strcmp(MpModeDisplayName(kRows[i].id), kRows[i].name) == 0);
		CHECK(d->selectable == 1);
		CHECK(d->timer == kRows[i].timer);
		CHECK(MpModeTimerKind(kRows[i].id) == kRows[i].timer);
		CHECK(MpModeUsesTimer(kRows[i].id) == 1);
		CHECK(d->music == MP_MODE_MUSIC_PURSUIT && MpModeMusic(kRows[i].id) == MP_MODE_MUSIC_PURSUIT);
		CHECK(d->teamCount == 0 && d->teamColours == 0);
		CHECK(d->respawn == MP_MODE_RESPAWN_INSTANT);	/* instant respawn, as Take a Ride */
		CHECK(d->lateJoin == MP_MODE_LATEJOIN_LIVE);
		/* the round start: wait for 2 and every car, 5-4-3-2-1 first map, 3-2-1 later, GO */
		CHECK(d->spawn == MP_MODE_SPAWN_CLUSTER && MpModeSpawnRule(kRows[i].id) == MP_MODE_SPAWN_CLUSTER);
		CHECK(d->waitMinPlayers == 2 && MpModeWaitMinPlayers(kRows[i].id) == 2);
		CHECK(d->waitForCars == 1 && MpModeWaitForCars(kRows[i].id) == 1);
		CHECK(d->firstCountdownSec == 5 && MpModeFirstCountdownSec(kRows[i].id) == 5);
		CHECK(d->roundCountdownSec == 3 && MpModeRoundCountdownSec(kRows[i].id) == 3);
		CHECK(MpModeHasPreGame(kRows[i].id) == 1 && MpModeWantsCountdown(kRows[i].id) == 1);
		CHECK(d->carChangeLocked == 0 && d->mapCycle == 0);
		CHECK(MpModeCarChangeRefusal(kRows[i].id, 1) == NULL);
	}

	CHECK(MpModeUsesTimer(MP_GAMEMODE_TAKEADRIDE) == 0);
	CHECK(MpModeMusic(MP_GAMEMODE_TAKEADRIDE) == MP_MODE_MUSIC_AUTO);
	CHECK(MpModeWantsCountdown(MP_GAMEMODE_TAKEADRIDE) == 0);
	CHECK(MpModeSpawnRule(MP_GAMEMODE_TAKEADRIDE) == MP_MODE_SPAWN_SESSION);
	CHECK(MpModeWantsCountdown(77) == 0 && MpModeSpawnRule(77) == MP_MODE_SPAWN_SESSION);	/* unknown -> mode 0 */
}

/* Every id the registry knows, plus ids it does not, keep the table well formed. */
static void CheckTable(void)
{
	int i, j;

	for (i = 0; i < MpModeCount(); i++)
	{
		const MP_MODE_DESC* d = MpModeAt(i);

		CHECK(d->id >= 0 && d->id < 256);		/* fits the wire byte */
		CHECK(d->name != NULL && d->name[0] != '\0');
		CHECK(d->teamCount >= 0 && d->teamCount <= MP_MODE_MAX_TEAMS);
		CHECK(!d->teamColours || d->teamCount > 0);

		for (j = i + 1; j < MpModeCount(); j++)
			CHECK(MpModeAt(j)->id != d->id);	/* ids unique */
	}

	/* Every row in the table is selectable: Take a Ride and the v23 POC modes. */
	CHECK(MpModeCount() == 3);
	for (i = 0; i < MpModeCount(); i++)
		CHECK(MpModeAt(i)->selectable == 1);
}

/* The answers each decision point in the session gets for a given id. */
static void CheckAnswersAreToday(int mode, const char* expectName)
{
	int slots[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	int models[8] = { 0, 1, 2, 3, 4, 8, 9, 12 };
	int slots0[8], models0[8];
	int i, on, r, g, b;

	CHECK(strcmp(MpModeDisplayName(mode), expectName) == 0);

	/* MpRespawnAfterDeath: an instant respawn (and, separately, never a game over) */
	CHECK(MpModeRespawnPolicy(mode) == MP_MODE_RESPAWN_INSTANT);

	/* MpSpawnLateJoiners / MpOnNetSpawn: seat at once, at the session's placement */
	CHECK(MpModeLateJoinPolicy(mode) == MP_MODE_LATEJOIN_LIVE);
	CHECK(MpModeSpawnRule(mode) == MP_MODE_SPAWN_SESSION);

	/* not wired yet, but the answers must already be "off" */
	CHECK(MpModeWantsCountdown(mode) == 0);
	CHECK(MpModeAllowsMapCycle(mode) == 0);
	CHECK(MpModeUsesTimer(mode) == 0);
	CHECK(MpModeMusic(mode) == MP_MODE_MUSIC_AUTO);

	/* MpChangeCar: never vetoed, in or out of a running session */
	CHECK(MpModeCarChangeRefusal(mode, 0) == NULL);
	CHECK(MpModeCarChangeRefusal(mode, 1) == NULL);

	/* MpCarListForCity: the list comes back untouched, every city */
	memcpy(slots0, slots, sizeof(slots));
	memcpy(models0, models, sizeof(models));

	for (i = 0; i < 4; i++)
	{
		CHECK(MpModeFilterCarPool(mode, i, slots, models, 8) == 8);
		CHECK(MpModeFilterCarPool(mode, i, slots, models, 3) == 3);
		CHECK(MpModeFilterCarPool(mode, i, slots, models, 0) == 0);
	}

	CHECK(memcmp(slots, slots0, sizeof(slots)) == 0);
	CHECK(memcmp(models, models0, sizeof(models)) == 0);

	/* MpNameTagColour / MpOnPedDraw / chat ink: the player's own colour, on/off kept */
	for (i = -1; i < MP_MAX_PLAYERS; i++)
	{
		CHECK(MpModePlayerTeam(mode, i) == -1);

		r = g = b = -7;
		on = MpModePlayerColour(mode, i, 0, 10, 20, 30, &r, &g, &b);
		CHECK(on == 0 && r == 10 && g == 20 && b == 30);

		on = MpModePlayerColour(mode, i, 1, 255, 0, 128, &r, &g, &b);
		CHECK(on == 1 && r == 255 && g == 0 && b == 128);
	}

	/* in-place use, as MpOnPedDraw calls it */
	r = 1; g = 2; b = 3;
	on = MpModePlayerColour(mode, 0, 1, r, g, b, &r, &g, &b);
	CHECK(on == 1 && r == 1 && g == 2 && b == 3);
}

static void CheckLobby(void)
{
	/* the Game mode row cycles 0 -> 1 -> 2 -> 0, and back */
	CHECK(MpModeIsSelectable(MP_GAMEMODE_TAKEADRIDE));
	CHECK(MpModeIsSelectable(MP_GAMEMODE_FOXHOUNDS));
	CHECK(MpModeIsSelectable(MP_GAMEMODE_BOMBTAG));
	CHECK(!MpModeIsSelectable(MP_GAMEMODE_COUNT));
	CHECK(!MpModeIsSelectable(-1));
	CHECK(MpModeNextSelectable(MP_GAMEMODE_TAKEADRIDE, 1) == MP_GAMEMODE_FOXHOUNDS);
	CHECK(MpModeNextSelectable(MP_GAMEMODE_FOXHOUNDS, 1) == MP_GAMEMODE_BOMBTAG);
	CHECK(MpModeNextSelectable(MP_GAMEMODE_BOMBTAG, 1) == MP_GAMEMODE_TAKEADRIDE);
	CHECK(MpModeNextSelectable(MP_GAMEMODE_TAKEADRIDE, -1) == MP_GAMEMODE_BOMBTAG);
	CHECK(MpModeNextSelectable(MP_GAMEMODE_TAKEADRIDE, 0) == MP_GAMEMODE_TAKEADRIDE);
	CHECK(MpModeNextSelectable(200, 1) == MP_GAMEMODE_TAKEADRIDE);	/* unknown -> a selectable one */
}

int main(void)
{
	CheckTakeARideDescriptor();
	CheckTable();
	CheckAnswersAreToday(MP_GAMEMODE_TAKEADRIDE, "TAKE A RIDE");

	/* an id this build does not know: named "-", plays as Take a Ride (as before) */
	CHECK(MpModeFind(MP_GAMEMODE_COUNT) == NULL);
	CHECK(MpModeBehaviour(MP_GAMEMODE_COUNT) == MpModeFind(MP_GAMEMODE_TAKEADRIDE));
	CheckAnswersAreToday(MP_GAMEMODE_COUNT, "-");
	CheckAnswersAreToday(255, "-");

	CheckLobby();
	CheckMarkModes();

	/* MpModePlayerTeam reads the round state now. Even a snapshot that names a team
	 * cannot put a Take a Ride player on one: the mode has no teams. */
	{
		unsigned char buf[sizeof(MP_ROUND) + 2 * sizeof(MP_ROUND_ENTRY)];
		MP_ROUND h;
		MP_ROUND_ENTRY e;
		int i;

		memset(&h, 0, sizeof(h));
		h.phase = MP_ROUND_PHASE_LIVE;
		h.round = 1;
		h.count = 2;
		memcpy(buf, &h, sizeof(h));
		memset(&e, 0, sizeof(e));
		e.playerId = 0; e.team = 1;
		memcpy(buf + sizeof(h), &e, sizeof(e));
		e.playerId = 1; e.team = MP_ROUND_TEAM_NONE;
		memcpy(buf + sizeof(h) + sizeof(e), &e, sizeof(e));

		MpRoundReset();
		CHECK(MpRoundApply(buf, (int)sizeof(buf)) == 1);
		CHECK(MpRoundPlayerTeam(0) == 1);		/* the carrier has it... */

		for (i = -1; i < MP_MAX_PLAYERS; i++)
		{
			CHECK(MpModePlayerTeam(MP_GAMEMODE_TAKEADRIDE, i) == -1);	/* ...TaR ignores it */
			CHECK(MpModePlayerTeam(MP_GAMEMODE_COUNT, i) == -1);	/* unknown plays as TaR */
		}

		MpRoundReset();
	}

	/* the table's clock and music per id, for run_mode_check.py to compare with the
	 * Python servers' mirror (mp_test.GAMEMODE_TIMER / GAMEMODE_MUSIC) */
	{
		int i;

		for (i = 0; i < MpModeCount(); i++)
			printf("MODEROW %d %d %d %d\n", MpModeAt(i)->id, MpModeAt(i)->timer, MpModeAt(i)->music,
				MpModeWantsCountdown(MpModeAt(i)->id));
	}

	printf("mp_mode: %d check(s), %d failure(s)\n", gChecks, gFails);
	return gFails == 0 ? 0 : 1;
}
