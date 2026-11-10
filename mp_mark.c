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
	int waitMin;		/* MpModeWaitMinPlayers */
	int waitCars;		/* MpModeWaitForCars */
	int firstSec;		/* MpModeFirstCountdownSec */
	int roundSec;		/* MpModeRoundCountdownSec */
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
	int countdownMs;	/* COUNTDOWN -> LIVE (on the wire as countdownDs) */
	int countdownTotalMs;	/* length of the current count */
	int countdownFirst;	/* 1 = fresh-map N..1 (not GET READY + N..1) */
	int firstMap;		/* host: GO has not happened since the last map load */
	int present[MP_MAX_PLAYERS];
	int ready[MP_MAX_PLAYERS];	/* has picked a car (MpMarkReady) */
	int remainMs[MP_MAX_PLAYERS];
	/* MP_MODE_TIMER_GETAWAY (Cops n Robbers), host-side except `heat` */
	int lastGetaway;	/* the getaway of the last round that went live, -1 = none yet */
	int firstPick;		/* the random first getaway, kept across an aborted countdown */
	int heat;		/* the cops' damage meter, 0..MP_MARK_CNR_BUST_POINTS (on the
				 * wire as the getaway's MP_MARK_ENTRY.remainDs) */
	long driveAcc;		/* the getaway's drive points, in 1/(1000*TOPSPEED) */
	long hitUntil[MP_MAX_PLAYERS];	/* per cop: no scored hit before this */
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
	gMark.waitMin = MpModeWaitMinPlayers(mode);
	gMark.waitCars = MpModeWaitForCars(mode);
	gMark.firstSec = MpModeFirstCountdownSec(mode);
	gMark.roundSec = MpModeRoundCountdownSec(mode);
	gMark.firstMap = MpModeHasPreGame(mode);	/* a reset is a map load (the host's START) */
	gMark.phase = MP_MARK_PHASE_WAITING;
	gMark.minutes = MpMarkClampMinutes(minutes);
	gMark.marked = -1;
	gMark.winner = -1;
	gMark.lastGetaway = -1;
	gMark.firstPick = -1;
	gMark.have = gMark.timer != MP_MODE_TIMER_NONE;
}

static int MpMarkOneClock(void)
{
	return gMark.timer == MP_MODE_TIMER_SHARED || gMark.timer == MP_MODE_TIMER_GETAWAY;
}

/* Cops n Robbers: who drives the getaway next. The first is random (kept if a
 * countdown is aborted, so a retry does not re-roll); then the next present id
 * after the last getaway, wrapping, so every player gets a turn. */
static int MpMarkNextGetaway(unsigned rnd)
{
	int i;

	if (gMark.lastGetaway < 0)
	{
		if (!MpMarkIdOk(gMark.firstPick) || !gMark.present[gMark.firstPick])
			gMark.firstPick = MpMarkPick(rnd);
		return gMark.firstPick;
	}

	for (i = 1; i <= MP_MAX_PLAYERS; i++)
	{
		int id = (gMark.lastGetaway + i) % MP_MAX_PLAYERS;

		if (gMark.present[id])
			return id;
	}

	return -1;
}

/* Every present player's clock full again (Fox N Hounds), or the one shared clock. */
static void MpMarkFreshClocks(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		gMark.remainMs[i] = (gMark.present[i] && gMark.timer == MP_MODE_TIMER_PERSONAL) ? MpMarkFullMs() : 0;

	gMark.sharedMs = MpMarkOneClock() ? MpMarkFullMs() : 0;
}

/* May a round start now? The mode's wait: enough players, and if waitForCars
 * every present player has picked a car. A mode with no wait is startable as
 * soon as someone is here (this referee is inactive when the mode has no timer). */
static int MpMarkStartable(void)
{
	int i, n = MpMarkCount();
	int need = gMark.waitMin;

	if (need < 1)
		need = (gMark.waitCars || gMark.firstSec > 0 || gMark.roundSec > 0) ? 1 : 2;

	if (n < need)
		return 0;

	if (!gMark.waitCars)
		return 1;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
		if (gMark.present[i] && !gMark.ready[i])
			return 0;

	return 1;
}

static void MpMarkBegin(unsigned rnd)
{
	gMark.phase = MP_MARK_PHASE_LIVE;
	gMark.winner = -1;
	gMark.countdownMs = 0;
	gMark.countdownTotalMs = 0;
	gMark.countdownFirst = 0;
	gMark.firstMap = 0;	/* GO: later rounds on this map use the per-round count */
	MpMarkFreshClocks();

	if (gMark.timer == MP_MODE_TIMER_GETAWAY)
	{
		/* the getaway named at the countdown, unless they left since */
		if (!MpMarkIdOk(gMark.marked) || !gMark.present[gMark.marked])
			gMark.marked = MpMarkNextGetaway(rnd);
		gMark.lastGetaway = gMark.marked;
		gMark.heat = 0;
		gMark.driveAcc = 0;
		memset(gMark.hitUntil, 0, sizeof(gMark.hitUntil));
	}
	else
		gMark.marked = MpMarkPick(rnd);

	gMark.cooldownUntil = gMark.clockMs + MP_MARK_TAG_COOLDOWN_MS;
	gMark.seq = (gMark.seq + 1) & 0xFFFF;
	gMark.revision++;
}

/* The wait is satisfied: one COUNTDOWN. A fresh map uses firstCountdownSec as
 * N..1; later rounds use GET READY + roundCountdownSec. firstMap is only
 * cleared at GO, so an aborted first-load count is retried, not replaced by
 * the shorter per-round count. */
static void MpMarkCountdown(int first, unsigned rnd)
{
	int ms;

	gMark.phase = MP_MARK_PHASE_COUNTDOWN;
	/* Cops n Robbers names the getaway NOW: every machine needs it for the
	 * chase spawn and the role car before GO. Nobody else is marked here. */
	gMark.marked = gMark.timer == MP_MODE_TIMER_GETAWAY ? MpMarkNextGetaway(rnd) : -1;
	gMark.heat = 0;
	gMark.winner = -1;
	gMark.countdownFirst = first ? 1 : 0;

	if (first && gMark.firstSec > 0)
		ms = gMark.firstSec * 1000;
	else if (gMark.roundSec > 0)
		ms = MP_MARK_COUNTDOWN_SPAWN_MS + gMark.roundSec * 1000;
	else
		ms = 0;

	gMark.countdownMs = ms;
	gMark.countdownTotalMs = ms;
	MpMarkFreshClocks();
	gMark.revision++;
}

/* WAITING -> (COUNTDOWN | LIVE) when the gate opens; the events that start says. */
static int MpMarkStart(unsigned rnd)
{
	int first = gMark.firstMap && gMark.firstSec > 0;

	if (first || gMark.roundSec > 0)
	{
		MpMarkCountdown(first, rnd);
		if (gMark.countdownMs > 0)
			return MP_MARK_EV_COUNTDOWN;
	}

	MpMarkBegin(rnd);
	return MP_MARK_EV_BEGIN;
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

	if (markedLeft && gMark.phase == MP_MARK_PHASE_LIVE && gMark.timer == MP_MODE_TIMER_GETAWAY)
	{
		MpMarkEnd(-1);		/* the getaway left: the round is over, nobody won it */
		ev |= MP_MARK_EV_END;
	}
	else if (markedLeft && gMark.phase == MP_MARK_PHASE_LIVE)
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

	/* Everyone gone: whoever comes in next is loading the map (a dedicated
	 * server's vote kicks everyone to rejoin on the new one), so the next
	 * countdown is the first-load count again. */
	if (changed && MpModeHasPreGame(gMark.mode) && MpMarkCount() == 0)
		gMark.firstMap = 1;

	return ev;
}

void MpMarkReady(const int* ready)
{
	int i, changed = 0;

	if (ready == NULL || gMark.timer == MP_MODE_TIMER_NONE)
		return;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		int want = ready[i] ? 1 : 0;

		if (want == gMark.ready[i])
			continue;

		gMark.ready[i] = want;

		if (gMark.present[i])
			changed = 1;	/* only a present player's flag is on the wire */
	}

	if (changed)
		gMark.revision++;
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
		if (MpMarkStartable())
			return MpMarkStart(rnd);
		return 0;

	case MP_MARK_PHASE_LOBBY:
		/* retired: treat a stale snapshot as WAITING */
		gMark.phase = MP_MARK_PHASE_WAITING;
		gMark.countdownMs = 0;
		gMark.countdownTotalMs = 0;
		gMark.countdownFirst = 0;
		gMark.revision++;
		if (MpMarkStartable())
			return MpMarkStart(rnd);
		return 0;

	case MP_MARK_PHASE_COUNTDOWN:
		/* someone left, or a joiner without a car arrived: wait for them */
		if (!MpMarkStartable())
		{
			gMark.phase = MP_MARK_PHASE_WAITING;
			gMark.marked = -1;	/* Cops n Robbers' named getaway waits too */
			gMark.countdownMs = 0;
			gMark.countdownTotalMs = 0;
			gMark.countdownFirst = 0;
			gMark.revision++;
			return 0;
		}

		gMark.countdownMs -= ms;

		if (gMark.countdownMs > 0)
			return 0;

		MpMarkBegin(rnd);	/* GO */
		return MP_MARK_EV_BEGIN;

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
				/* Car Bomb Tag: the holder (still `marked`) loses.
				 * Cops n Robbers: the getaway lasted the clock and wins. */
				MpMarkEnd(gMark.timer == MP_MODE_TIMER_GETAWAY ? gMark.marked : -1);
				return MP_MARK_EV_END;
			}
		}
		return 0;

	case MP_MARK_PHASE_ENDED:
	default:
		gMark.intermissionMs -= ms;

		if (gMark.intermissionMs > 0)
			return 0;

		/* the same wait-then-countdown gate (first-load if the map was emptied) */
		if (MpMarkStartable())
			return MpMarkStart(rnd);

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

	if (gMark.phase != MP_MARK_PHASE_LIVE || gMark.timer == MP_MODE_TIMER_NONE ||
		gMark.timer == MP_MODE_TIMER_GETAWAY)
		return 0;	/* Cops n Robbers never moves the getaway: see MpMarkHit */
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

int MpMarkHit(int a, int b, int strength, int* outEv)
{
	int cop, pts;

	if (outEv != NULL)
		*outEv = 0;

	if (gMark.phase != MP_MARK_PHASE_LIVE || gMark.timer != MP_MODE_TIMER_GETAWAY)
		return 0;
	if (!MpMarkIdOk(a) || !MpMarkIdOk(b) || a == b || !gMark.present[a] || !gMark.present[b])
		return 0;

	if (gMark.marked == a)
		cop = b;
	else if (gMark.marked == b)
		cop = a;
	else
		return 0;	/* two cops bumping: nothing */

	if (strength <= 0 || gMark.clockMs < gMark.hitUntil[cop])
		return 0;

	if (strength > MP_MARK_CNR_HIT_MAX)
		strength = MP_MARK_CNR_HIT_MAX;

	pts = strength * MP_MARK_CNR_HIT_POINTS;
	gMark.hitUntil[cop] = gMark.clockMs + MP_MARK_CNR_HIT_COOLDOWN_MS;
	gMark.heat += pts;
	gMark.revision++;

	if (gMark.heat >= MP_MARK_CNR_BUST_POINTS)
	{
		gMark.heat = MP_MARK_CNR_BUST_POINTS;
		MpMarkEnd(-1);		/* busted: the cops win the round */
		if (outEv != NULL)
			*outEv = MP_MARK_EV_END;
	}

	return pts;
}

int MpMarkDrive(int ms, int speed)
{
	long den = 1000L * MP_MARK_CNR_DRIVE_TOPSPEED;
	int pts;

	if (gMark.phase != MP_MARK_PHASE_LIVE || gMark.timer != MP_MODE_TIMER_GETAWAY ||
		!MpMarkIdOk(gMark.marked) || ms <= 0)
		return 0;

	if (speed < 0)
		speed = 0;
	if (speed > MP_MARK_CNR_DRIVE_TOPSPEED)
		speed = MP_MARK_CNR_DRIVE_TOPSPEED;

	gMark.driveAcc += (long)ms * (MP_MARK_CNR_DRIVE_BASE * MP_MARK_CNR_DRIVE_TOPSPEED +
		MP_MARK_CNR_DRIVE_BONUS * speed);
	pts = (int)(gMark.driveAcc / den);
	gMark.driveAcc %= den;
	return pts;
}

int MpMarkEscape(void)
{
	if (gMark.phase != MP_MARK_PHASE_LIVE || gMark.timer != MP_MODE_TIMER_GETAWAY ||
		!MpMarkIdOk(gMark.marked))
		return 0;

	MpMarkEnd(gMark.marked);	/* clear of every cop: the getaway wins now */
	return MP_MARK_EV_END;
}

int MpMarkHeat(void)
{
	return gMark.timer == MP_MODE_TIMER_GETAWAY ? gMark.heat : 0;
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
	h.countdownDs = (uint16_t)MpMarkDs(gMark.countdownMs);
	memcpy(out, &h, sizeof(h));

	n = 0;
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_MARK_ENTRY e;

		if (!gMark.present[i])
			continue;

		memset(&e, 0, sizeof(e));
		e.playerId = (uint8_t)i;
		e.flags = (uint8_t)(gMark.ready[i] ? MP_MARK_ENTRY_F_READY : 0);
		e.remainDs = (uint16_t)MpMarkDs(gMark.remainMs[i]);
		if (gMark.timer == MP_MODE_TIMER_GETAWAY)
			e.remainDs = (uint16_t)(i == gMark.marked ? gMark.heat : 0);	/* the heat meter */
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
	gMark.waitMin = MpModeWaitMinPlayers(h.gamemode);
	gMark.waitCars = MpModeWaitForCars(h.gamemode);
	gMark.firstSec = MpModeFirstCountdownSec(h.gamemode);
	gMark.roundSec = MpModeRoundCountdownSec(h.gamemode);
	gMark.phase = h.phase;
	gMark.marked = h.marked == MP_MARK_NONE || !MpMarkIdOk(h.marked) ? -1 : h.marked;
	gMark.winner = h.winner == MP_MARK_NONE || !MpMarkIdOk(h.winner) ? -1 : h.winner;
	gMark.minutes = MpMarkClampMinutes(h.minutes);
	gMark.seq = h.seq;
	gMark.sharedMs = (int)h.sharedDs * 100;
	gMark.countdownMs = (int)h.countdownDs * 100;

	/* first vs per-round is host-only; a client infers from remaining vs the
	 * later total (firstSec*1000 is 5000, later is 4000 for Fox/Bomb). Once
	 * a COUNTDOWN is already in progress, keep the kind locked in. */
	if (gMark.phase != MP_MARK_PHASE_COUNTDOWN)
	{
		gMark.countdownFirst = 0;
		gMark.countdownTotalMs = 0;
	}
	else if (gMark.countdownTotalMs <= 0)
	{
		int later = gMark.roundSec > 0
			? MP_MARK_COUNTDOWN_SPAWN_MS + gMark.roundSec * 1000 : 0;
		int first = gMark.firstSec * 1000;

		if (first > 0 && (later <= 0 || gMark.countdownMs > later))
		{
			gMark.countdownFirst = 1;
			gMark.countdownTotalMs = first;
		}
		else
		{
			gMark.countdownFirst = 0;
			gMark.countdownTotalMs = later > 0 ? later : first;
		}
	}

	memset(gMark.present, 0, sizeof(gMark.present));
	memset(gMark.ready, 0, sizeof(gMark.ready));
	memset(gMark.remainMs, 0, sizeof(gMark.remainMs));
	gMark.heat = 0;

	for (i = 0; i < h.count; i++)
	{
		MP_MARK_ENTRY e;

		memcpy(&e, p + sizeof(h) + (size_t)i * sizeof(e), sizeof(e));

		if (!MpMarkIdOk(e.playerId))
			continue;

		gMark.present[e.playerId] = 1;
		gMark.ready[e.playerId] = (e.flags & MP_MARK_ENTRY_F_READY) ? 1 : 0;
		gMark.remainMs[e.playerId] = (int)e.remainDs * 100;

		if (gMark.timer == MP_MODE_TIMER_GETAWAY)
		{
			gMark.remainMs[e.playerId] = 0;
			if (e.playerId == gMark.marked)
				gMark.heat = e.remainDs;
		}
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

int MpMarkPlayerReady(int playerId)
{
	return MpMarkPresent(playerId) && gMark.ready[playerId];
}

int MpMarkCountWaiting(int* outReady)
{
	int i, n = 0, r = 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (!gMark.present[i])
			continue;
		n++;
		if (gMark.ready[i])
			r++;
	}

	if (outReady != NULL)
		*outReady = r;

	return n;
}

int MpMarkCountdownMs(int sinceMs)
{
	int ms;

	if (gMark.phase != MP_MARK_PHASE_COUNTDOWN)
		return 0;

	ms = gMark.countdownMs - (sinceMs > 0 ? sinceMs : 0);
	return ms < 0 ? 0 : ms;
}

int MpMarkCountdownTotalMs(void)
{
	return gMark.phase == MP_MARK_PHASE_COUNTDOWN ? gMark.countdownTotalMs : 0;
}

int MpMarkCountdownIsFirst(void)
{
	return gMark.phase == MP_MARK_PHASE_COUNTDOWN && gMark.countdownFirst;
}

int MpMarkClockMs(int playerId, int sinceMs)
{
	int ms, ticking;

	if (sinceMs < 0)
		sinceMs = 0;

	if (playerId < 0)
	{
		ms = gMark.sharedMs;
		ticking = MpMarkOneClock();
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
