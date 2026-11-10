/*
 * mp_round.c -- the round state: the host's authority and the client's mirror.
 * See mp_round.h for the contract and docs/GAMEMODES.md for the wire.
 *
 * Kept free of engine and session headers so it builds on its own: the standalone
 * check (tools/mode_check/test_mp_round.cpp) compiles this file with mp_mode.c.
 */
#include "mp_round.h"
#include "mp_mode.h"
#include "mp_proto.h"

#include <string.h>

typedef struct MP_ROUND_PLAYER
{
	int present;
	int state;	/* MP_ROUND_PLAYER_* */
	int score;
	int team;	/* -1 = none */
} MP_ROUND_PLAYER;

static struct
{
	int have;	/* a state was built (host) or applied (client) this session */
	int mode;
	int number;
	int phase;
	unsigned revision;
	MP_ROUND_PLAYER players[MP_MAX_PLAYERS];
} gRound;

static int MpRoundIdOk(int id)
{
	return id >= 0 && id < MP_MAX_PLAYERS;
}

/* The team a mode puts a player on when a round starts. No mode has teams yet
 * (Phase 3 assigns them), so this is -1 for everybody -- the only right answer
 * for a mode with teamCount 0, such as Take a Ride. */
static int MpRoundTeamFor(int mode, int playerId)
{
	(void)mode;
	(void)playerId;
	return -1;
}

/* The state a player arriving NOW gets. */
static int MpRoundArrivalState(int mode)
{
	if (gRound.phase != MP_ROUND_PHASE_LIVE)
		return MP_ROUND_PLAYER_WAITING;

	/* Take a Ride (MP_MODE_LATEJOIN_LIVE): straight in. A SPECTATE policy is
	 * still played live until Phase 2 implements the wait -- the joiner IS spawned
	 * live (MpSpawnLateJoiners logs that fallback), so the state says so. */
	(void)mode;
	return MP_ROUND_PLAYER_PLAYING;
}

void MpRoundReset(void)
{
	int i;

	memset(&gRound, 0, sizeof(gRound));
	gRound.phase = MP_ROUND_PHASE_IDLE;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		gRound.players[i].team = -1;
}

void MpRoundBegin(int mode)
{
	int i;

	gRound.have = 1;
	gRound.mode = mode;
	gRound.number = (gRound.number >= 0xFFFF) ? 1 : gRound.number + 1;
	gRound.phase = MP_ROUND_PHASE_LIVE;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_ROUND_PLAYER* p = &gRound.players[i];

		if (!p->present)
			continue;

		p->state = MP_ROUND_PLAYER_PLAYING;
		if (!MpModeKeepsScore(mode))
			p->score = 0;	/* Cops n Robbers keeps the match total */
		p->team = MpRoundTeamFor(mode, i);
	}

	gRound.revision++;
}

void MpRoundEnd(void)
{
	if (gRound.phase == MP_ROUND_PHASE_ENDED)
		return;

	gRound.have = 1;
	gRound.phase = MP_ROUND_PHASE_ENDED;
	gRound.revision++;
}

int MpRoundAddScore(int playerId, int points)
{
	MP_ROUND_PLAYER* p;
	long long v;

	if (playerId < 0 || playerId >= MP_MAX_PLAYERS || points == 0)
		return 0;

	p = &gRound.players[playerId];

	if (!p->present)
		return 0;

	v = (long long)p->score + points;
	if (v > 0x7FFFFFFF)
		v = 0x7FFFFFFF;
	if (v < -0x7FFFFFFF)
		v = -0x7FFFFFFF;

	p->score = (int)v;
	gRound.have = 1;
	gRound.revision++;
	return 1;
}

int MpRoundSyncPresent(const int* present, int mode)
{
	int i, changed = 0;

	if (present == NULL)
		return 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_ROUND_PLAYER* p = &gRound.players[i];
		int want = present[i] ? 1 : 0;

		if (want == p->present)
			continue;

		p->present = want;
		p->score = 0;
		p->team = want ? MpRoundTeamFor(mode, i) : -1;
		p->state = want ? MpRoundArrivalState(mode) : MP_ROUND_PLAYER_PLAYING;
		changed = 1;
	}

	if (changed)
	{
		gRound.have = 1;
		gRound.revision++;
	}

	return changed;
}

int MpRoundBuild(unsigned char* out, int cap)
{
	MP_ROUND h;
	int i, n = 0, len;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		if (gRound.players[i].present)
			n++;

	len = (int)(sizeof(MP_ROUND) + (size_t)n * sizeof(MP_ROUND_ENTRY));

	if (out == NULL || cap < len)
		return 0;

	memset(&h, 0, sizeof(h));
	h.gamemode = (uint8_t)gRound.mode;
	h.phase = (uint8_t)gRound.phase;
	h.round = (uint16_t)gRound.number;
	h.count = (uint8_t)n;
	memcpy(out, &h, sizeof(h));

	n = 0;
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		const MP_ROUND_PLAYER* p = &gRound.players[i];
		MP_ROUND_ENTRY e;

		if (!p->present)
			continue;

		memset(&e, 0, sizeof(e));
		e.playerId = (uint8_t)i;
		e.state = (uint8_t)p->state;
		e.team = (uint8_t)(p->team < 0 ? MP_ROUND_TEAM_NONE : p->team);
		e.score = (int32_t)p->score;
		memcpy(out + sizeof(h) + (size_t)n * sizeof(e), &e, sizeof(e));
		n++;
	}

	return len;
}

unsigned MpRoundRevision(void)
{
	return gRound.revision;
}

int MpRoundApply(const unsigned char* p, int len)
{
	MP_ROUND h;
	int i;

	if (p == NULL || len < (int)sizeof(MP_ROUND))
		return 0;

	memcpy(&h, p, sizeof(h));

	if (h.count > MP_MAX_PLAYERS ||
		len < (int)(sizeof(MP_ROUND) + (size_t)h.count * sizeof(MP_ROUND_ENTRY)))
		return 0;

	gRound.have = 1;
	gRound.mode = h.gamemode;
	gRound.number = h.round;
	gRound.phase = h.phase;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		gRound.players[i].present = 0;
		gRound.players[i].state = MP_ROUND_PLAYER_PLAYING;
		gRound.players[i].score = 0;
		gRound.players[i].team = -1;
	}

	for (i = 0; i < h.count; i++)
	{
		MP_ROUND_ENTRY e;
		MP_ROUND_PLAYER* r;

		memcpy(&e, p + sizeof(h) + (size_t)i * sizeof(e), sizeof(e));

		if (!MpRoundIdOk(e.playerId))
			continue;

		r = &gRound.players[e.playerId];
		r->present = 1;
		r->state = e.state;
		r->score = (int)e.score;
		r->team = (e.team == MP_ROUND_TEAM_NONE || e.team >= MP_MODE_MAX_TEAMS) ? -1 : (int)e.team;
	}

	gRound.revision++;
	return 1;
}

int MpRoundHaveState(void)
{
	return gRound.have;
}

int MpRoundMode(void)
{
	return gRound.mode;
}

int MpRoundNumber(void)
{
	return gRound.number;
}

int MpRoundPhase(void)
{
	return gRound.phase;
}

int MpRoundIsLive(void)
{
	return gRound.phase == MP_ROUND_PHASE_LIVE;
}

int MpRoundPlayerPresent(int playerId)
{
	return MpRoundIdOk(playerId) && gRound.players[playerId].present;
}

int MpRoundPlayerState(int playerId)
{
	if (!MpRoundPlayerPresent(playerId))
		return MP_ROUND_PLAYER_PLAYING;

	return gRound.players[playerId].state;
}

int MpRoundPlayerScore(int playerId)
{
	if (!MpRoundPlayerPresent(playerId))
		return 0;

	return gRound.players[playerId].score;
}

int MpRoundPlayerTeam(int playerId)
{
	if (!MpRoundPlayerPresent(playerId))
		return -1;

	return gRound.players[playerId].team;
}
