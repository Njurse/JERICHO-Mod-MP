/*
 * mp_mark.c -- the marked-player referee and its mirror. See mp_mark.h, and
 * docs/GAMEMODES.md for the rules and the wire. Pure, like mp_round.c; the Python
 * twin is tools/servers/dediserver/dedi/markref.py and the two are run step by
 * step against each other by tools/mode_check.
 */
#include "mp_mark.h"
#include "mp_mode.h"
#include "mp_proto.h"

#include <string.h>

static struct
{
	int have;
	int mode;
	int timer;		/* MP_MODE_TIMER_* of mode */
	int phase;		/* MP_MARK_PHASE_* */
	int minutes;
	int marked;		/* -1 = nobody */
	int winner;		/* -1 = nobody */
	unsigned seq;
	unsigned revision;
	int sharedMs;
	long clockMs;		/* host: time refereed this session */
	long cooldownUntil;	/* host: no tag before this */
	int intermissionMs;	/* host: ENDED -> next round */
	int present[MP_MAX_PLAYERS];
	int remainMs[MP_MAX_PLAYERS];
} gMark;

static int MpMarkIdOk(int id)
{
	return id >= 0 && id < MP_MAX_PLAYERS;
}

int MpMarkClampMinutes(int minutes)
{
	if (minutes < MP_MARK_MINUTES_MIN)
		return MP_MARK_MINUTES_MIN;
	if (minutes > MP_MARK_MINUTES_MAX)
		return MP_MARK_MINUTES_MAX;
	return minutes;
}

static int MpMarkFullMs(void)
{
	return gMark.minutes * 60 * 1000;
}

static int MpMarkCount(void)
{
	int i, n = 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		if (gMark.present[i])
			n++;

	return n;
}

/* The `rnd`-th present player, in ascending id. -1 when nobody is. */
static int MpMarkPick(unsigned rnd)
{
	int i, n = MpMarkCount(), k;

	if (n <= 0)
		return -1;

	k = (int)(rnd % (unsigned)n);

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (!gMark.present[i])
			continue;
		if (k-- == 0)
			return i;
	}

	return -1;
}

void MpMarkReset(int mode, int minutes)
{
	memset(&gMark, 0, sizeof(gMark));
	gMark.mode = mode;
	gMark.timer = MpModeTimerKind(mode);
	gMark.phase = MP_MARK_PHASE_WAITING;
	gMark.minutes = MpMarkClampMinutes(minutes);
	gMark.marked = -1;
	gMark.winner = -1;
	gMark.have = gMark.timer != MP_MODE_TIMER_NONE;
}

static void MpMarkBegin(unsigned rnd)
{
	int i;

	gMark.phase = MP_MARK_PHASE_LIVE;
	gMark.winner = -1;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		gMark.remainMs[i] = (gMark.present[i] && gMark.timer == MP_MODE_TIMER_PERSONAL) ? MpMarkFullMs() : 0;

	gMark.sharedMs = gMark.timer == MP_MODE_TIMER_SHARED ? MpMarkFullMs() : 0;
	gMark.marked = MpMarkPick(rnd);
	gMark.cooldownUntil = gMark.clockMs + MP_MARK_TAG_COOLDOWN_MS;
	gMark.seq = (gMark.seq + 1) & 0xFFFF;
	gMark.revision++;
}

static void MpMarkEnd(int winner)
{
	gMark.phase = MP_MARK_PHASE_ENDED;
	gMark.winner = winner;
	gMark.intermissionMs = MP_MARK_INTERMISSION_MS;
	gMark.revision++;
}

int MpMarkSync(const int* present, unsigned rnd)
{
	int i, ev = 0, changed = 0, markedLeft = 0;

	if (present == NULL || gMark.timer == MP_MODE_TIMER_NONE)
		return 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		int want = present[i] ? 1 : 0;

		if (want == gMark.present[i])
			continue;

		gMark.present[i] = want;
		changed = 1;

		/* An arrival is a hound with a full clock that does not tick (Fox N
		 * Hounds), or a player without the bomb (Car Bomb Tag). */
		gMark.remainMs[i] = (want && gMark.timer == MP_MODE_TIMER_PERSONAL) ? MpMarkFullMs() : 0;

		if (!want && i == gMark.marked)
			markedLeft = 1;
	}

	if (markedLeft && gMark.phase == MP_MARK_PHASE_LIVE)
	{
		gMark.marked = MpMarkPick(rnd);
		gMark.cooldownUntil = gMark.clockMs + MP_MARK_TAG_COOLDOWN_MS;
		gMark.seq = (gMark.seq + 1) & 0xFFFF;
		ev |= MP_MARK_EV_MOVED;
	}
	else if (markedLeft)
		gMark.marked = -1;

	if (changed)
		gMark.revision++;

	return ev;
}

int MpMarkTick(int ms, unsigned rnd)
{
	int n;

	if (gMark.timer == MP_MODE_TIMER_NONE || ms < 0)
		return 0;

	gMark.clockMs += ms;
	n = MpMarkCount();

	switch (gMark.phase)
	{
	case MP_MARK_PHASE_WAITING:
		if (n >= 2)
		{
			MpMarkBegin(rnd);
			return MP_MARK_EV_BEGIN;
		}
		return 0;

	case MP_MARK_PHASE_LIVE:
		if (n < 2)
		{
			MpMarkEnd(-1);	/* nobody left to play against: no winner */
			return MP_MARK_EV_END;
		}

		if (gMark.timer == MP_MODE_TIMER_PERSONAL)
		{
			if (!MpMarkIdOk(gMark.marked))
				return 0;

			gMark.remainMs[gMark.marked] -= ms;

			if (gMark.remainMs[gMark.marked] <= 0)
			{
				gMark.remainMs[gMark.marked] = 0;
				MpMarkEnd(gMark.marked);	/* first to 0:00 wins */
				return MP_MARK_EV_END;
			}
		}
		else
		{
			gMark.sharedMs -= ms;

			if (gMark.sharedMs <= 0)
			{
				gMark.sharedMs = 0;
				MpMarkEnd(-1);		/* the holder (still `marked`) loses */
				return MP_MARK_EV_END;
			}
		}
		return 0;

	case MP_MARK_PHASE_ENDED:
	default:
		gMark.intermissionMs -= ms;

		if (gMark.intermissionMs > 0)
			return 0;

		if (n >= 2)
		{
			MpMarkBegin(rnd);
			return MP_MARK_EV_BEGIN;
		}

		gMark.phase = MP_MARK_PHASE_WAITING;
		gMark.marked = -1;
		gMark.winner = -1;
		gMark.revision++;
		return 0;
	}
}

int MpMarkContact(int a, int b)
{
	int to;

	if (gMark.phase != MP_MARK_PHASE_LIVE || gMark.timer == MP_MODE_TIMER_NONE)
		return 0;
	if (!MpMarkIdOk(a) || !MpMarkIdOk(b) || a == b)
		return 0;
	if (!gMark.present[a] || !gMark.present[b])
		return 0;

	if (gMark.marked == a)
		to = b;
	else if (gMark.marked == b)
		to = a;
	else
		return 0;	/* two hounds / two bomb-less players bumping: nothing */

	if (gMark.clockMs < gMark.cooldownUntil)
		return 0;

	gMark.marked = to;
	gMark.cooldownUntil = gMark.clockMs + MP_MARK_TAG_COOLDOWN_MS;
	gMark.seq = (gMark.seq + 1) & 0xFFFF;
	gMark.revision++;
	return 1;
}

static unsigned MpMarkDs(int ms)
{
	unsigned ds;

	if (ms <= 0)
		return 0;

	ds = (unsigned)((ms + 99) / 100);	/* round UP: 0:00 only when it really is */
	return ds > 0xFFFF ? 0xFFFF : ds;
}

int MpMarkBuild(unsigned char* out, int cap)
{
	MP_MARK h;
	int i, n = MpMarkCount(), len;

	len = (int)(sizeof(MP_MARK) + (size_t)n * sizeof(MP_MARK_ENTRY));

	if (out == NULL || cap < len)
		return 0;

	memset(&h, 0, sizeof(h));
	h.gamemode = (uint8_t)gMark.mode;
	h.phase = (uint8_t)gMark.phase;
	h.marked = (uint8_t)(MpMarkIdOk(gMark.marked) ? gMark.marked : MP_MARK_NONE);
	h.winner = (uint8_t)(MpMarkIdOk(gMark.winner) ? gMark.winner : MP_MARK_NONE);
	h.minutes = (uint8_t)gMark.minutes;
	h.count = (uint8_t)n;
	h.seq = (uint16_t)gMark.seq;
	h.sharedDs = (uint16_t)MpMarkDs(gMark.sharedMs);
	memcpy(out, &h, sizeof(h));

	n = 0;
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_MARK_ENTRY e;

		if (!gMark.present[i])
			continue;

		memset(&e, 0, sizeof(e));
		e.playerId = (uint8_t)i;
		e.remainDs = (uint16_t)MpMarkDs(gMark.remainMs[i]);
		memcpy(out + sizeof(h) + (size_t)n * sizeof(e), &e, sizeof(e));
		n++;
	}

	return len;
}

unsigned MpMarkRevision(void)
{
	return gMark.revision;
}

int MpMarkApply(const unsigned char* p, int len)
{
	MP_MARK h;
	int i;

	if (p == NULL || len < (int)sizeof(MP_MARK))
		return 0;

	memcpy(&h, p, sizeof(h));

	if (h.count > MP_MAX_PLAYERS ||
		len < (int)(sizeof(MP_MARK) + (size_t)h.count * sizeof(MP_MARK_ENTRY)))
		return 0;

	gMark.have = 1;
	gMark.mode = h.gamemode;
	gMark.timer = MpModeTimerKind(h.gamemode);
	gMark.phase = h.phase;
	gMark.marked = h.marked == MP_MARK_NONE || !MpMarkIdOk(h.marked) ? -1 : h.marked;
	gMark.winner = h.winner == MP_MARK_NONE || !MpMarkIdOk(h.winner) ? -1 : h.winner;
	gMark.minutes = MpMarkClampMinutes(h.minutes);
	gMark.seq = h.seq;
	gMark.sharedMs = (int)h.sharedDs * 100;

	memset(gMark.present, 0, sizeof(gMark.present));
	memset(gMark.remainMs, 0, sizeof(gMark.remainMs));

	for (i = 0; i < h.count; i++)
	{
		MP_MARK_ENTRY e;

		memcpy(&e, p + sizeof(h) + (size_t)i * sizeof(e), sizeof(e));

		if (!MpMarkIdOk(e.playerId))
			continue;

		gMark.present[e.playerId] = 1;
		gMark.remainMs[e.playerId] = (int)e.remainDs * 100;
	}

	gMark.revision++;
	return 1;
}

int MpMarkHaveState(void)	{ return gMark.have; }
int MpMarkMode(void)		{ return gMark.mode; }
int MpMarkPhase(void)		{ return gMark.phase; }
int MpMarkMarked(void)		{ return gMark.marked; }
int MpMarkWinner(void)		{ return gMark.winner; }
int MpMarkMinutes(void)		{ return gMark.minutes; }
unsigned MpMarkSeq(void)	{ return gMark.seq; }

int MpMarkPresent(int playerId)
{
	return MpMarkIdOk(playerId) && gMark.present[playerId];
}

int MpMarkClockMs(int playerId, int sinceMs)
{
	int ms, ticking;

	if (sinceMs < 0)
		sinceMs = 0;

	if (playerId < 0)
	{
		ms = gMark.sharedMs;
		ticking = gMark.timer == MP_MODE_TIMER_SHARED;
	}
	else
	{
		if (!MpMarkPresent(playerId))
			return 0;
		ms = gMark.remainMs[playerId];
		ticking = gMark.timer == MP_MODE_TIMER_PERSONAL && playerId == gMark.marked;
	}

	if (ticking && gMark.phase == MP_MARK_PHASE_LIVE)
		ms -= sinceMs;

	return ms < 0 ? 0 : ms;
}
