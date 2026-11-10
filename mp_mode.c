/*
 * mp_mode.c -- the game-mode table and the answers to the session's questions.
 * See mp_mode.h for the contract and docs/GAMEMODES.md for how to add a mode.
 *
 * Kept free of engine and session headers so it builds on its own: the standalone
 * check (tools/mode_check/test_mp_mode.cpp) compiles this file with mp_round.c
 * (the round state, which is equally pure) and nothing else.
 */
#include "mp_mode.h"
#include "mp_round.h"	/* team membership: the round state */
#include "mp_proto.h"	/* MP_GAMEMODE_*: the ids are the wire's, not this file's */

#include <stddef.h>

/* THE TABLE. One row per mode. Row 0 must stay Take a Ride, and every field of it
 * must describe what the session did before modes existed -- the standalone check
 * fails if it does not. */
static const MP_MODE_DESC kModes[] =
{
	{
		MP_GAMEMODE_TAKEADRIDE,	/* id */
		"TAKE A RIDE",		/* name: the string browsers have always shown */
		1,			/* selectable */

		0,			/* teamCount: free roam, nobody is on a team */
		{ { "", 0, 0, 0 } },	/* teams */
		0,			/* teamColours: everyone keeps their own colour */

		MP_MODE_RESPAWN_INSTANT, 0,	/* a death puts you straight back on the map */
		MP_MODE_LATEJOIN_LIVE,	/* a late joiner drives at once */
		0,			/* countdown: none */
		0,			/* carChangeLocked: Vehicle Options always works */
		MP_MODE_CARPOOL_ALL,	/* every car the car mods offer */
		MP_MODE_SPAWN_SESSION,	/* the session's own placement */
		0,			/* mapCycle: not offered */
		MP_MODE_TIMER_NONE,	/* timer: none, nobody is marked */
		MP_MODE_MUSIC_AUTO	/* music: the engine's own (cop chases) */
	},
	{
		MP_GAMEMODE_FOXHOUNDS,	/* id */
		"FOX N HOUNDS",		/* name */
		1,			/* selectable */

		0,			/* teamCount: every player for themselves */
		{ { "", 0, 0, 0 } },	/* teams */
		0,			/* teamColours */

		MP_MODE_RESPAWN_INSTANT, 0,	/* as Take a Ride */
		MP_MODE_LATEJOIN_LIVE,	/* a late joiner drives at once, as a hound */
		0,			/* countdown: none (POC) */
		0,			/* carChangeLocked */
		MP_MODE_CARPOOL_ALL,
		MP_MODE_SPAWN_SESSION,
		0,			/* mapCycle */
		MP_MODE_TIMER_PERSONAL,	/* your clock runs while you are the fox */
		MP_MODE_MUSIC_PURSUIT	/* the chase is on */
	},
	{
		MP_GAMEMODE_BOMBTAG,	/* id */
		"CAR BOMB TAG",		/* name */
		1,			/* selectable */

		0,			/* teamCount */
		{ { "", 0, 0, 0 } },	/* teams */
		0,			/* teamColours */

		MP_MODE_RESPAWN_INSTANT, 0,
		MP_MODE_LATEJOIN_LIVE,	/* a late joiner drives at once, without the bomb */
		0,			/* countdown */
		0,			/* carChangeLocked */
		MP_MODE_CARPOOL_ALL,
		MP_MODE_SPAWN_SESSION,
		0,			/* mapCycle */
		MP_MODE_TIMER_SHARED,	/* one clock; the holder at 0:00 loses */
		MP_MODE_MUSIC_PURSUIT
	},
};

#define MP_MODE_ROWS	((int)(sizeof(kModes) / sizeof(kModes[0])))

int MpModeCount(void)
{
	return MP_MODE_ROWS;
}

const MP_MODE_DESC* MpModeAt(int index)
{
	if (index < 0 || index >= MP_MODE_ROWS)
		return NULL;

	return &kModes[index];
}

const MP_MODE_DESC* MpModeFind(int id)
{
	int i;

	for (i = 0; i < MP_MODE_ROWS; i++)
	{
		if (kModes[i].id == id)
			return &kModes[i];
	}

	return NULL;
}

/* An unknown id behaves as Take a Ride: before modes existed nothing read the id at
 * all, so that is the behaviour a newer server's id has always had here. */
const MP_MODE_DESC* MpModeBehaviour(int id)
{
	const MP_MODE_DESC* d = MpModeFind(id);

	return d != NULL ? d : &kModes[0];
}

int MpModeIsSelectable(int id)
{
	const MP_MODE_DESC* d = MpModeFind(id);

	return d != NULL && d->selectable;
}

int MpModeNextSelectable(int id, int dir)
{
	int i, start = -1;

	for (i = 0; i < MP_MODE_ROWS; i++)
	{
		if (kModes[i].id == id)
		{
			start = i;
			break;
		}
	}

	/* Not a selectable row (or unknown): the first selectable one. */
	if (start < 0 || !kModes[start].selectable || dir == 0)
	{
		if (start >= 0 && kModes[start].selectable)
			return id;

		for (i = 0; i < MP_MODE_ROWS; i++)
			if (kModes[i].selectable)
				return kModes[i].id;

		return MP_GAMEMODE_TAKEADRIDE;
	}

	dir = dir > 0 ? 1 : -1;

	for (i = 1; i <= MP_MODE_ROWS; i++)
	{
		int k = ((start + dir * i) % MP_MODE_ROWS + MP_MODE_ROWS) % MP_MODE_ROWS;

		if (kModes[k].selectable)
			return kModes[k].id;
	}

	return id;
}

const char* MpModeDisplayName(int id)
{
	const MP_MODE_DESC* d = MpModeFind(id);

	return d != NULL ? d->name : "-";
}

int MpModeRespawnPolicy(int mode)
{
	return MpModeBehaviour(mode)->respawn;
}

int MpModeLateJoinPolicy(int mode)
{
	return MpModeBehaviour(mode)->lateJoin;
}

int MpModeSpawnRule(int mode)
{
	return MpModeBehaviour(mode)->spawn;
}

int MpModeWantsCountdown(int mode)
{
	return MpModeBehaviour(mode)->countdown;
}

int MpModeAllowsMapCycle(int mode)
{
	return MpModeBehaviour(mode)->mapCycle;
}

int MpModeTimerKind(int mode)
{
	return MpModeBehaviour(mode)->timer;
}

int MpModeUsesTimer(int mode)
{
	return MpModeTimerKind(mode) != MP_MODE_TIMER_NONE;
}

int MpModeMusic(int mode)
{
	return MpModeBehaviour(mode)->music;
}

const char* MpModeCarChangeRefusal(int mode, int roundRunning)
{
	const MP_MODE_DESC* d = MpModeBehaviour(mode);

	if (d->carChangeLocked && roundRunning)
		return "this game mode locks the car while a round is running";

	return NULL;
}

int MpModeFilterCarPool(int mode, int city, int* slots, int* models, int count)
{
	(void)city;
	(void)slots;
	(void)models;

	switch (MpModeBehaviour(mode)->carPool)
	{
	case MP_MODE_CARPOOL_ALL:
	default:
		return count;
	}
}

int MpModePlayerTeam(int mode, int playerId)
{
	const MP_MODE_DESC* d = MpModeBehaviour(mode);
	int team;

	/* A mode without teams (Take a Ride) has nobody on one, whatever a snapshot
	 * says: the round state cannot put a free-for-all player on a team. */
	if (d->teamCount <= 0)
		return -1;

	/* Membership travels in the round state (MP_TAG_ROUND, mp_round.c). A team
	 * index the mode does not have reads as none. */
	team = MpRoundPlayerTeam(playerId);

	if (team < 0 || team >= d->teamCount || team >= MP_MODE_MAX_TEAMS)
		return -1;

	return team;
}

int MpModePlayerColour(int mode, int playerId, int on, int r, int g, int b,
	int* outR, int* outG, int* outB)
{
	const MP_MODE_DESC* d = MpModeBehaviour(mode);
	int team = MpModePlayerTeam(mode, playerId);

	if (d->teamColours && team >= 0 && team < d->teamCount && team < MP_MODE_MAX_TEAMS)
	{
		r = d->teams[team].r;
		g = d->teams[team].g;
		b = d->teams[team].b;
		on = 1;
	}

	if (outR != NULL) *outR = r;
	if (outG != NULL) *outG = g;
	if (outB != NULL) *outB = b;

	return on;
}
