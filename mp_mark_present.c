/*
 * mp_mark_present.c -- split out of mp_session.c
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"

/* ---- THE MARKED PLAYER (MP_TAG_MARK, mp_mark.c): Fox N Hounds, Car Bomb Tag ----
 *
 * The host referees: it keeps the referee's players equal to the registry, ticks it
 * on the sim frame (30 Hz, so 1000 ms per 30 frames exactly), turns accepted player
 * contacts into tags, drives the round state from its events, and sends 'JPMK'
 * after a WELCOME, on START, on every change, and every 30 frames while a round is
 * live (120 otherwise) so the clocks stay close. Clients apply it and never send.
 *
 * Every machine then PRESENTS it the same way (MpMarkPresentTick): the golden
 * notice on a change of fox / bomb holder or a round end, derived locally from the
 * snapshot and the roster's names; the clock as a top-centre panel; the stock
 * arrow over the marked car (never over our own); and the mode's music while live.
 * Take a Ride never reaches any of it: MpModeUsesTimer is 0 for mode 0.
 *
 * PRE-GAME (modes that opt in: waitMinPlayers, waitForCars, firstCountdownSec,
 * roundCountdownSec in mp_mode.h): one WAITING stage until the wait is satisfied,
 * then one COUNTDOWN. A fresh map uses firstCountdownSec as N..1 then GO (the
 * start cluster is placed from the beginning of that count). Later rounds on the
 * same map use GET READY (the cluster) plus roundCountdownSec as N..1 then GO.
 * A joiner who arrives before GO is not a mid-round (live) join: they pick a car
 * and are counted in the wait. Only a join after GO uses mid-round placement.
 * Take a Ride leaves the four fields at 0 and never waits. No clock, tag, arrow
 * or forced music before GO. */

#define MP_CHASE_EXTRA		1000	/* <= MP_CHASE_GAP - MP_JOIN_CLEAR: never into the next cop's band */
#define MP_CHASE_FIRST		4000
#define MP_CHASE_GAP		3000
#define MP_CHASE_GIVEUP_FRAMES	105	/* 3.5 s: inside the 4 s round countdown */
#define MP_CHASE_JITTER		0	/* none: every machine computes the same line */
#define MP_CHASE_MOVED		2000
#define MP_CHASE_SNAP		1800	/* a step off the road snaps back this far, at most */
#define MP_CHASE_STEP		1000
#define MP_CHASE_WAIT_FRAMES	60	/* 2 s at 30 Hz */
#define MP_CLUSTER_DRIFT	1000	/* a slot's road spot stays this close to it, so two slots never snap together */
#define MP_CLUSTER_GIVEUP_FRAMES	(MP_MARK_COUNTDOWN_SPAWN_MS * 30 / 1000 + 30)	/* 2 s */
#define MP_CLUSTER_PER_RING	6	/* slots on one ring: 2*4800*sin(pi/6) = 4800 apart */
#define MP_CLUSTER_RADIUS	(3 * MP_CLUSTER_STEP)	/* 14400: furthest a car lands from the anchor */
#define MP_CLUSTER_STEP		(MP_JOIN_CLEAR + 2800)	/* 4800 (was 3200, the join spawn's retry step):
							 * Fox N Hounds / Car Bomb Tag rounds start 1.5x
							 * further apart. Only MP_MODE_SPAWN_CLUSTER uses it. */
#define MP_CLUSTER_TRIES_PER_FRAME	12	/* was 4: the cluster should be placed in the GET READY
					 * second, not trickle into the digits */
#define MP_CHASE_FRESH_FRAMES	12	/* a cop lines up once the getaway's state is this many frames
					 * newer than our countdown start (was: wait for it to move 2000,
					 * else 2 s -- measured: try 61, 60 frames into the count) */
#define MP_CHASE_REPLACE_MAX	2	/* re-line-ups when the getaway lands after we did */
#define MP_FOX_FADE_FRAMES	12	/* a colour fade, ~0.4 s at 30 fps */
#define MP_FOX_OWN_Y		18	/* the own timer: under the title, over nothing */
#define MP_FOX_SIDE_INSET	6	/* the side column's right edge: in from the SCREEN's
					 * edge (jer_screen_edges), so widescreen moves it out */
#define MP_FOX_SIDE_ROWS	5	/* side timers before a "+N" */
#define MP_FOX_SIDE_SCALE	10	/* 10/16 of the mission timer */
#define MP_FOX_SIDE_STEP	20
#define MP_FOX_SIDE_Y		60	/* under the Felony bar's row (overlay.c: y 46) */
#define MP_FOX_TITLE_Y		4
#define MP_JOIN_WIDE_TRIES	8	/* centers probed inside the chosen region */
#define MP_MARK_ARROW_SLOT	0	/* jer_arrow slot for the marked car */
#define MP_MARK_PANEL_SLOT	5	/* jer_hud_panel slot for the clock */
#define MP_MARK_RELAY_RADIUS	1200	/* host view of a reported contact, centre to centre:
					 * a car length plus room for two links' lag (mp 0.25;
					 * was 1800, which passed the mark on near misses) */

static unsigned gMpMarkRng;		/* host: the referee's random picks */

static unsigned gMpMarkSentRev;		/* host: the revision last broadcast */

static unsigned gMpMarkMsAcc;		/* host: 30 Hz frames -> ms, without drift */

static unsigned gMpMarkApplyFrame;	/* client: the frame the last snapshot landed */

int gMpSawPreLive;		/* 1 = we already saw a pre-GO phase this join */
static int gMpMarkRefusedLogged;

/* what this machine last PRESENTED, so a notice fires once per change */
static int gMpMarkShownSeq = -1;

static int gMpMarkShownPhase = -1;

static int gMpMarkPanelOn;

static int gMpMarkArrowOn;

static int gMpMarkMusicOn;

static int gMpMarkShownCount = -1;	/* the countdown notice last shown: 4 = GET READY, 3..1 */

static int gMpIntermissionShown = -1;	/* ENDED: last 'Next round in N' shown, or -1 */

static int gMpMarkTickFrame = -1;	/* last bomb-tick frame, or -1 */

static int gMpHudR = 128, gMpHudG = 128, gMpHudB = 64;	/* eased panel colour */

static char gMpFoxLine1[64];

static char gMpFoxLine2[16];

static int gMpFoxLinesOn;

/* The timer modes' HUD in the mission-timer digits (core PrintTimer, the JERICHO
 * HUD timer convention: JERICHO/docs/HOOKS.md "HUD timers"). Built each frame by
 * MpMarkPresentTick, drawn from DRAW_OVERLAY. Fox N Hounds: every player's own
 * clock (ours top centre, the rest down the right edge). Car Bomb Tag and Cops n
 * Robbers: the one round clock, top centre. (mp 0.23.4; all modes 0.24.0) */
#define MP_FOX_FADE_FRAMES	12	/* a colour fade, ~0.4 s at 30 fps */
#define MP_FOX_SIDE_ROWS	5	/* side timers before a "+N" */
static struct
{
	int on;
	int kind;			/* MP_MODE_TIMER_* */
	int me;				/* our id, -1 = no own timer (not in the round) */
	int title;			/* draw the "... IS THE FOX!" title */
	int marked;
	int ms[MP_MAX_PLAYERS];		/* -1 = not in the round */
	int shared;			/* the round clock (bomb, CnR) */
	char titleText[48];
	char sub[24];			/* one line under the clock: the CnR damage meter */
} gMpFoxHud;

static int gMpFoxFade[MP_MAX_PLAYERS];	/* 0 = grey (frozen) .. MP_FOX_FADE_FRAMES = cyan */

static int gMpBombFade;			/* 0 = white .. MP_FOX_FADE_FRAMES = red: we hold the bomb */

/* A3: the full repair still owed after the round's role-car swap */
static int gMpRoundRepairPending;

/* the round-start cluster on THIS machine (MpMarkClusterTick, defined with the
 * other placement code below) */
static int gMpClusterState;		/* 0 = not in a countdown, 1 = placing, 2 = done / gave up */

static unsigned gMpClusterFrom;		/* the frame this machine saw the countdown start */

static int gMpClusterTries;

/* where the getaway was when this machine's cop lined up behind it: if the
 * getaway lands somewhere else after that (its placement took longer than its
 * state took to get here), the cop lines up again, still before GO */
static int gMpChaseLineX, gMpChaseLineZ, gMpChaseLineSet, gMpChaseReplaces;

static unsigned MpMarkRand(void);
static void MpMarkHostSync(void);
void MpHostSendMark(int connIndex);
static const char* MpMarkName(int id);
void MpMarkPresentClear(void);
static void MpMarkNotices(int me, int since);
static int MpHudLerp(int cur, int tgt);
static void MpMarkBeepBomb(CAR_DATA* cp, int remMs, int fullMs, int localHolder);
static void MpMarkGlowFor(int marked, int myId, int bomb);
void MpMarkHostDamage(int from, int damage, int maxDamage, const char* how);
void MpHostBenchOut(int id, const char* how);
static void MpRoundRepairTick(void);
static void MpSpectateTick(int phase, int myId);
static void MpFoxTimerColour(int id);
static int MpFoxDrawTimer(int id, int ms, int right, int y, int scale);
static void MpFoxHudBuild(int phase, int marked, int myId, int since);
static void MpMarkResetCarState(CAR_DATA* cp);
static void MpMarkRoundReset(void);
static int MpClusterPlace(MP_PLAYER* me, CAR_DATA* cp, int x, int z, int dir, int anchorId, const char* how);
static void MpMarkClusterTick(void);
static int MpChaseHeading(int x, int z, int want);
static void MpMarkChaseTick(void);

/* Called from MpSessionReset: this module's state is per session, so
 * the next match starts from the same values the module loaded with. */
void MpMarkPresentReset(void)
{
	gMpMarkApplyFrame = 0;
	gMpMarkRefusedLogged = 0;
	gMpMarkSentRev = 0;
	gMpWideRegion = -1;
	gMpWideTries = 0;
}

static unsigned MpMarkRand(void)
{
	gMpMarkRng = gMpMarkRng * 1103515245u + 12345u;
	return (gMpMarkRng >> 16) & 0x7FFF;
}

static void MpMarkHostSync(void)
{
	int present[MP_MAX_PLAYERS];
	int ready[MP_MAX_PLAYERS];
	int id;

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);

		present[id] = p != NULL;
		/* the round-start gate: the same "has chosen a car" the spawn and the
		 * roster's MP_ROSTER_FLAG_CAR_READY use (the host is always chosen) */
		ready[id] = p != NULL && MpPlayerCarReady(p);
	}

	{
		int ev = MpMarkSync(present, MpMarkRand());

		if ((ev & MP_MARK_EV_MOVED) && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark: the marked player left - player %d is marked now\n", MpMarkMarked());

		/* the getaway left mid-round (Cops n Robbers), or the fox / the bomb
		 * holder left with fewer than two still here: over, nobody won it */
		if (ev & MP_MARK_EV_END)
		{
			gMpCnrPending = 0;
			MpRoundEnd();

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] mark: the marked player left - round %d over, nobody won (marked %d)\n",
					MpRoundNumber(), MpMarkMarked());
		}
	}

	MpMarkReady(ready);
}

void MpHostSendMark(int connIndex)
{
	unsigned char buf[sizeof(MP_MARK) + MP_MAX_PLAYERS * sizeof(MP_MARK_ENTRY)];
	int len;

	if (!MpIsHost() || !MpModeUsesTimer(gMp.gamemode))
		return;

	len = MpMarkBuild(buf, (int)sizeof(buf));

	if (len <= 0)
		return;

	if (connIndex >= 0)
	{
		MpSendConn(connIndex, MP_TAG_MARK, 0, buf, len);
		return;
	}

	MpHostBroadcast(MP_TAG_MARK, 0, buf, len);
	gMpMarkSentRev = MpMarkRevision();
}

/* Host, at START in a timer mode: a fresh referee, waiting for two players. */
void MpHostMarkBegin(void)
{
	gMpMarkRng ^= (unsigned)MpNowMs() ^ (gMp.seed * 2654435761u) ^ 0x5EEDu;
	gMpMarkMsAcc = 0;

	MpMarkSetIntermissionMs(gMp.config.intermissionSec * 1000);	/* the host's lobby setting */
	MpMarkResetSeconds(gMp.gamemode, MpSessionModeSeconds());
	MpRoundHostSync();
	MpMarkHostSync();

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] mark: %s, %d s - waiting for two players%s\n",
			MpModeName(gMp.gamemode), MpMarkSeconds(),
			MpModeWantsCountdown(gMp.gamemode) ? " with a car each, then a countdown" : "");

	MpHostSendMark(-1);
}

/* Host, once a frame in a match. */
void MpHostMarkTick(void)
{
	int ev, ms;

	if (!MpIsHost() || !MpModeUsesTimer(gMp.gamemode))
		return;

	MpMarkHostSync();

	gMpMarkMsAcc += 1000;
	ms = (int)(gMpMarkMsAcc / 30);
	gMpMarkMsAcc %= 30;

	ev = MpMarkTick(ms, MpMarkRand());

	if (ev & MP_MARK_EV_END)
		MpHostCnrFlush();	/* the clock ran out on the getaway: bank its last points first */

	ev |= MpHostCnrTick((ev & MP_MARK_EV_BEGIN) ? 0 : ms);

	if ((ev & MP_MARK_EV_COUNTDOWN) && gMpCtx != NULL)
	{
		int ready = 0, n = MpMarkCountWaiting(&ready);

		gMpCtx->jer_log(gMpCtx, "[mp] mark: wait satisfied (%d/%d cars) - %s countdown %d ms to round %d\n",
			ready, n,
			MpMarkCountdownIsFirst() ? "first-load" : "round",
			MpMarkCountdownTotalMs(), MpRoundNumber() + 1);
	}

	if (ev & MP_MARK_EV_BEGIN)
	{
		MpRoundHostSync();
		MpRoundBegin(gMp.gamemode);
		gMpCnrPending = 0;
		gMpCnrEscapeMs = 0;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark: round %d live, player %d is marked (%d min)\n",
				MpRoundNumber(), MpMarkMarked(), MpMarkMinutes());
	}

	if (ev & MP_MARK_EV_END)
	{
		MpRoundEnd();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark: round %d over - winner %d, marked %d\n",
				MpRoundNumber(), MpMarkWinner(), MpMarkMarked());
	}

	if (MpMarkRevision() != gMpMarkSentRev ||
		(gMp.frame % ((MpMarkPhase() == MP_MARK_PHASE_LIVE || MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN)
			? 30 : 120)) == 0)
		MpHostSendMark(-1);
}

int MpHostMarkForceEnd(void)
{
	int wasLive, ev;

	if (!MpIsHost() || !MpModeUsesTimer(gMp.gamemode))
		return 0;

	wasLive = (MpMarkPhase() == MP_MARK_PHASE_LIVE);
	ev = MpMarkForceEnd();
	if (!(ev & MP_MARK_EV_END))
		return 0;

	if (wasLive)
		MpHostCnrFlush();
	gMpCnrPending = 0;
	gMpCnrEscapeMs = 0;
	MpRoundEnd();
	MpHostSendMark(-1);
	MpHostSendRound(-1);
	return 1;
}

int MpHostMarkForceRestart(void)
{
	int ev;

	if (!MpIsHost())
		return 0;

	if (!MpModeUsesTimer(gMp.gamemode))
	{
		MpRoundHostSync();
		MpRoundBegin(gMp.gamemode);
		MpHostSendRound(-1);
		return 1;
	}

	MpMarkHostSync();
	gMpCnrPending = 0;
	gMpCnrEscapeMs = 0;
	gMpClusterState = 0;
	ev = MpMarkForceRestart(MpMarkRand());

	if (ev & MP_MARK_EV_BEGIN)
	{
		MpRoundHostSync();
		MpRoundBegin(gMp.gamemode);
		MpHostSendRound(-1);
	}

	MpHostSendMark(-1);
	return 1;
}

/* Host: a contact between players `a` and `b`. Our own (`a` is us) comes from the
 * engine or the probe and is taken as seen; a client's report (MP_HIT) is checked
 * against where the host sees both cars, with room for the two links' lag. A
 * client never moves the mark itself -- it only reports, and this decides. */
void MpMarkHostReport(int a, int b, int checkDistance, int strength)
{
	if (!MpIsHost() || !MpModeUsesTimer(gMp.gamemode) || MpMarkPhase() != MP_MARK_PHASE_LIVE)
		return;

	if (checkDistance)
	{
		MP_PLAYER* pa = MpGetPlayer(a);
		MP_PLAYER* pb = MpGetPlayer(b);
		long long dx, dy, dz;

		if (pa == NULL || pb == NULL || pa->carId < 0 || pb->carId < 0 ||
			pa->carId >= MAX_CARS || pb->carId >= MAX_CARS)
			return;

		dx = (long long)car_data[pa->carId].hd.where.t[0] - car_data[pb->carId].hd.where.t[0];
		dy = (long long)car_data[pa->carId].hd.where.t[1] - car_data[pb->carId].hd.where.t[1];
		dz = (long long)car_data[pa->carId].hd.where.t[2] - car_data[pb->carId].hd.where.t[2];

		if (dx * dx + dy * dy + dz * dz > (long long)MP_MARK_RELAY_RADIUS * MP_MARK_RELAY_RADIUS)
		{
			if (MpDebugOn() && gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] mark: contact %d-%d refused (too far apart here)\n", a, b);
			return;
		}
	}

	/* Cops n Robbers: a contact is a HIT, the cop's points, and the heat meter */
	if (MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_GETAWAY)
	{
		int hev = 0;
		int pts = MpMarkHit(a, b, strength, &hev);
		int cop = (a == MpMarkMarked()) ? b : a;

		if (pts <= 0)
			return;

		MpRoundAddScore(cop, pts);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark: player %d hit the getaway for %d (strength %d), heat %d/%d\n",
				cop, pts, strength, MpMarkHeat(), MP_MARK_CNR_BUST_POINTS);

		if (hev & MP_MARK_EV_END)
		{
			MpHostCnrFlush();
			MpRoundEnd();

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] mark: round %d over - player %d busted\n",
					MpRoundNumber(), MpMarkMarked());
		}

		MpHostSendMark(-1);
		MpHostSendRound(-1);
		return;
	}

	if (MpMarkContact(a, b))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark: tag %d-%d, player %d is marked\n", a, b, MpMarkMarked());

		MpHostSendMark(-1);
	}
}

/* Client: a host snapshot. A host drops one a peer sends, as with JPRD. */
void MpHandleMark(int connIndex, const unsigned char* p, int len)
{
	if (gMp.role != MP_ROLE_CLIENT)
	{
		if (!gMpMarkRefusedLogged && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark state from conn %d ignored: only the host writes it\n", connIndex);
		gMpMarkRefusedLogged = 1;
		return;
	}

	if (!MpMarkApply(p, len))
	{
		if (!gMpMarkRefusedLogged && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark state refused: malformed (%d bytes)\n", len);
		gMpMarkRefusedLogged = 1;
		return;
	}

	gMpMarkApplyFrame = gMp.frame;
	MpSessionAdoptLiveJoin();
}

static const char* MpMarkName(int id)
{
	MP_PLAYER* pl = MpGetPlayer(id);

	return (pl != NULL && pl->name[0] != '\0') ? pl->name : "Someone";
}

/* Drop everything MpMarkPresentTick put up. Safe to call any time. */
void MpMarkPresentClear(void)
{
	if (gMpMarkPanelOn)
		jer_hud_panel_clear(MP_MARK_PANEL_SLOT);

	if (gMpMarkArrowOn)
		jer_arrow_clear(MP_MARK_ARROW_SLOT);

	if (gMpMarkMusicOn && jer_music_forced() != JER_MUSIC_AUTO)
		jer_music_force(JER_MUSIC_AUTO);

	gMpMarkPanelOn = gMpMarkArrowOn = gMpMarkMusicOn = 0;
	gMp321Count = 0;
	gMpGoFrame = -1;
	gMpSpectating = 0;
	gMpSpectateCar = -1;
	gMpMarkShownSeq = -1;
	gMpMarkShownPhase = -1;
	gMpMarkShownCount = -1;
	gMpIntermissionShown = -1;
	gMpClusterState = 0;
	gMpClusterTries = 0;
	gMpChaseFromSet = 0;
	gMpMarkTickFrame = -1;
	gMpFoxLinesOn = 0;
	gMpFoxLine1[0] = '\0';
	gMpFoxLine2[0] = '\0';
	gMpFoxHud.on = 0;
	memset(gMpFoxFade, 0, sizeof(gMpFoxFade));
	gMpRoundRepairPending = 0;
	gMpHudR = 128;
	gMpHudG = 128;
	gMpHudB = 64;
	jer_carexplode_reset(-1);
	jer_carglow_clear();
	gJerMapForceFlash = 0;
	gJerMapCopCars = 0;
	MpCnrRadioClear();
}

/* The map's cop sight cones for PLAYER cops (overmap.c gJerMapCopCars). The
 * engine draws a cone only for its own cops (CONTROL_TYPE_PURSUER_AI, or
 * CONTROL_FLAG_COP), and a remote player's car is CONTROL_TYPE_PLAYER with
 * no cop flag, so a player in a police car never showed one. Every frame:
 * each OTHER player's car that is a police car (JerCarSlotIsPolice, the
 * engine's own answer, imported police cars included), and in Cops n Robbers
 * every other player but the getaway while the round runs. Our own car never
 * draws one (the map is centred on it). Drawing only. */
void MpMapCopsTick(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	unsigned mask = 0;
	int id, cnr, marked = -1;

	cnr = MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_GETAWAY && MpMarkHaveState() &&
		MpMarkMode() == gMp.gamemode && MpMarkPhase() == MP_MARK_PHASE_LIVE;

	if (cnr)
		marked = MpMarkMarked();

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		int car;

		if (p == NULL || p == me || p->carId < 0 || p->carId >= MAX_CARS || p->carId >= 32)
			continue;

		car = p->carId;

		if (car_data[car].controlType == CONTROL_TYPE_NONE)
			continue;

		if (JerCarSlotIsPolice(car_data[car].ap.model) || (cnr && id != marked))
			mask |= 1u << car;
	}

	gJerMapCopCars = mask;
}

int MpMarkChaseGetaway(void)
{
	if (!gMp.running || MpModeTimerKind(gMp.gamemode) != MP_MODE_TIMER_GETAWAY ||
		!MpMarkHaveState() || MpMarkMode() != gMp.gamemode || MpMarkPhase() != MP_MARK_PHASE_LIVE)
		return -1;

	return MpMarkMarked();
}

/* JERICHO round hold (mp 0.23.4): is the local car held still -- the COUNTDOWN and
 * the ENDED result screen of a countdown mode, never WAITING (the lobby drives
 * free) and never Take a Ride. v24: also while this player is benched in a live
 * round (a late joiner WAITING, or OUT with no respawn) -- they spectate. See mp.h. */
int MpRoundHoldsCar(void)
{
	MP_PLAYER* me;
	int ph;

	if (!gMp.running || !MpModeUsesTimer(gMp.gamemode) || !MpModeWantsCountdown(gMp.gamemode) ||
		!MpMarkHaveState() || MpMarkMode() != gMp.gamemode)
		return 0;

	ph = MpMarkPhase();

	if (ph == MP_MARK_PHASE_LIVE)
	{
		me = MpLocalPlayer();
		return me != NULL && MpMarkPresent(me->id) && !MpMarkInRound(me->id);
	}

	return ph == MP_MARK_PHASE_COUNTDOWN || ph == MP_MARK_PHASE_ENDED;
}

static void MpMarkNotices(int me, int since)
{
	int seq = (int)MpMarkSeq(), phase = MpMarkPhase(), marked = MpMarkMarked();
	int bomb = MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_SHARED;
	int cnr = MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_GETAWAY;
	int go = gMpMarkShownPhase == MP_MARK_PHASE_COUNTDOWN;	/* LIVE straight out of the 3-2-1 */
	char line[JER_NOTIFY_TEXT_MAX];

	/* ONE COUNTDOWN, a client running the time on from its last snapshot, as it
	 * does the clocks:
	 *   first-load: firstCountdownSec as N..1, then GO;
	 *   later rounds: GET READY while the cluster is placed, then N..1, then GO.
	 * v24: N..1 and GO are the engine's own race-start digits, drawn by
	 * MpMarkDrawOverlay (Print321Go); only GET READY is a golden notice. */
	gMp321Count = 0;

	if (phase == MP_MARK_PHASE_COUNTDOWN)
	{
		int left = MpMarkCountdownMs(since);
		int first = MpMarkCountdownIsFirst();
		int spawn = !first && left > MpMarkCountdownTotalMs() - MP_MARK_COUNTDOWN_SPAWN_MS;
		int count = spawn ? 4 : (left + 999) / 1000;

		if (gMpMarkShownPhase != phase)
			gMpMarkShownCount = -1;	/* a fresh countdown (or one restarted) */

		/* only ever DOWN: a client's next snapshot (rounded up to 0.1 s) must not
		 * put a "3" back after the "2" at a boundary */
		if (count > 0 && (gMpMarkShownCount < 0 || count <= gMpMarkShownCount))
		{
			if (spawn && count >= 4)
			{
				if (gMpMarkShownCount != count)
					jer_notify("GET READY", 4, 1);
			}
			else
			{
				/* mdraw.c DrawMission: 32 - (g321GoDelay & 31), each digit
				 * shrinking from full size over its second */
				int into = 1000 - (left - (count - 1) * 1000);

				gMp321Count = count;
				gMp321Scale = 32 - into * 32 / 1000;
				if (gMp321Scale < 1)
					gMp321Scale = 1;
				if (gMp321Scale > 32)
					gMp321Scale = 32;
			}
			gMpMarkShownCount = count;
		}
		else if (gMpMarkShownCount > 0 && gMpMarkShownCount < 4)
		{
			gMp321Count = gMpMarkShownCount;	/* a late snapshot: hold the digit, smallest */
			gMp321Scale = 1;
		}

		gMpMarkShownSeq = seq;
		gMpMarkShownPhase = phase;
		return;
	}

	gMpMarkShownCount = -1;

	/* the big GO, for a second, when the round goes live out of the count */
	if (phase == MP_MARK_PHASE_LIVE && go)
		gMpGoFrame = (int)gMp.frame;
	else if (phase != MP_MARK_PHASE_LIVE)
		gMpGoFrame = -1;

	/* a round ended since we last looked (not one we only joined into) */
	if (phase == MP_MARK_PHASE_ENDED && gMpMarkShownPhase == MP_MARK_PHASE_LIVE)
	{
		line[0] = '\0';

		int why = MpMarkEndReason();	/* v24: the referee says why (MP_MARK_END_*) */

		if (why == MP_MARK_END_VOID || why == MP_MARK_END_NONE)
			snprintf(line, sizeof(line), "Round over - not enough players");
		else if (cnr)
		{
			/* the getaway wins by the clock or by getting clear; a full meter (or
			 * the getaway out of the round) is a bust; or the getaway left */
			if ((why == MP_MARK_END_CLOCK || why == MP_MARK_END_ESCAPE) && MpMarkWinner() >= 0)
			{
				if (MpMarkWinner() == me)
					snprintf(line, sizeof(line), why == MP_MARK_END_ESCAPE ? "You got away!" : "Time's up - you got away!");
				else
					snprintf(line, sizeof(line), why == MP_MARK_END_ESCAPE ? "%s got away!" : "Time's up - %s got away!",
						MpMarkName(MpMarkWinner()));
			}
			else if (why == MP_MARK_END_BUST && marked >= 0)
			{
				if (marked == me)
					snprintf(line, sizeof(line), "BUSTED!");
				else
					snprintf(line, sizeof(line), "%s is busted!", MpMarkName(marked));
			}
			else if (why == MP_MARK_END_LEAVE)
				snprintf(line, sizeof(line), "The getaway left - round over");
			else
				snprintf(line, sizeof(line), "Round over");
		}
		else if (!bomb && MpMarkWinner() >= 0)
		{
			if (MpMarkWinner() == me)
				snprintf(line, sizeof(line), "You won the match!");
			else
				snprintf(line, sizeof(line), "%s won the match!", MpMarkName(MpMarkWinner()));
		}
		else if (bomb && marked >= 0)
		{
			if (marked == me)
				snprintf(line, sizeof(line), "BOOM! You lost the match!");
			else
				snprintf(line, sizeof(line), "%s blew up! You won the match!", MpMarkName(marked));
		}
		else
			snprintf(line, sizeof(line), "Round over - not enough players");	/* void: nobody marked, nobody won */

		if (line[0] != '\0')
			jer_notify(line, 4, 5);
	}
	/* the fox / the bomb changed hands (or a round went live) */
	else if (phase == MP_MARK_PHASE_LIVE && seq != gMpMarkShownSeq && marked >= 0)
	{
		const char* lead = "";	/* v24: the big GO (Print321Go) says GO */

		line[0] = '\0';

		/* Cops n Robbers names the getaway in the role line under the clock,
		 * and Fox N Hounds in the timer title: the notice does not say it again */
		if (cnr)
		{
			if (marked == me)
				snprintf(line, sizeof(line), "%sLose the cops!", lead);
			else
				snprintf(line, sizeof(line), "%sBust the getaway!", lead);
		}
		else if (bomb)
		{
			if (marked == me)
				snprintf(line, sizeof(line), "%sYou have the bomb! Get rid of it!", lead);
			else
				snprintf(line, sizeof(line), "%s%s has the bomb! Stay away!", lead, MpMarkName(marked));
		}
		else
		{
			if (marked == me)
				snprintf(line, sizeof(line), "%sYou are now the fox!", lead);
		}

		if (line[0] != '\0')
			jer_notify(line, go ? 4 : 3, 3);	/* 4: over the last digit still showing */
	}

	/* ENDED: a centred mission-style notice (jer_notify -> SetPlayerMessage /
	 * DrawMessage, the same path as GET READY and the end line). N is the
	 * intermission left in seconds, run on from the last snapshot
	 * (countdownDs in ENDED). Priority 3 so the end line (4, 5 s) is not
	 * replaced; once that drops, this is retried until it shows. */
	if (phase == MP_MARK_PHASE_ENDED)
	{
		int left = MpMarkIntermissionMs(since);
		int n = (left + 999) / 1000;
		char wait[JER_NOTIFY_TEXT_MAX];

		if (n > 0 && (gMpIntermissionShown < 0 || n <= gMpIntermissionShown) &&
			n != gMpIntermissionShown)
		{
			snprintf(wait, sizeof(wait), "Next round in %d", n);
			if (jer_notify(wait, 3, 2))
				gMpIntermissionShown = n;
		}
	}
	else
		gMpIntermissionShown = -1;

	gMpMarkShownSeq = seq;
	gMpMarkShownPhase = phase;
}

static int MpHudLerp(int cur, int tgt)
{
	int d = tgt - cur;
	int step = d / 4;

	if (step == 0 && d != 0)
		step = (d > 0) ? 1 : -1;

	return cur + step;
}

/* Car Bomb Tag only, while LIVE: a beep (SBK_ID_COUNTDOWN sample 0, not
 * the mission-23 clock tick) from the holder's car. Interval shrinks as the
 * clock runs down (1 s -> 400 ms approaching 10 s), then 200 ms at or under
 * 10 s, and every 4 frames under 5 s so the short beeps smear into a tone.
 *
 * Start3DSoundVolPitch always binds srcvelocity to dummylong (sound.c:645)
 * and then ComputeDoppler (sound.c:545) uses srcVel - camera_vel. The
 * per-frame DoDopplerSFX loop (gamesnd.c) re-applies that via
 * SetChannelPosition3 for any channel that still has a srcposition. A
 * local holder's beep would therefore doppler against the chase camera's
 * velocity as if the source were parked in the world. For the local
 * holder, StartSound (sound.c:585) is the non-doppler path: srcposition
 * is NULL, CompleteSoundSetup applies pitch with no dopplerScale, and
 * the update loop skips it. Volume stays -1500, which is already the
 * point-blank 3D level (CalculateVolume floors distance at 1024).
 * Remote holders stay positional: Start3DSoundVolPitch plus
 * SetChannelPosition3 on the live car pose and linearVelocity (the same
 * pairing engine and siren sounds use) so source velocity is not left
 * at zero. */
static void MpMarkBeepBomb(CAR_DATA* cp, int remMs, int fullMs, int localHolder)
{
	int interval, pitch;

	if (cp == NULL || remMs < 0)
		return;

	if (remMs < 5000)
		interval = 4;
	else if (remMs <= 10000)
		interval = 6;
	else
	{
		int span = fullMs - 10000;
		int ms;

		if (span < 1)
			span = 1;

		ms = 400 + (int)(((long)(remMs - 10000) * 600) / span);
		interval = (ms * 30 + 500) / 1000;
		if (interval < 6)
			interval = 6;
	}

	if (gMpMarkTickFrame >= 0 && (int)gMp.frame - gMpMarkTickFrame < interval)
		return;

	gMpMarkTickFrame = (int)gMp.frame;
	/* same pitch curve as mission 23 (mc_snd.c): 4096 - count/750,
	 * and count is milliseconds * 3 */
	pitch = 4096 - remMs / 250;
	if (pitch < 2048)
		pitch = 2048;
	if (pitch > 5120)
		pitch = 5120;

	if (localHolder)
	{
		StartSound(-1, jer_carexplode_bank(), jer_carexplode_beep_sample(),
			-1500, pitch);
		return;
	}

	{
		int chan = Start3DSoundVolPitch(-1, jer_carexplode_bank(), jer_carexplode_beep_sample(),
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2],
			-1500, pitch);

		if (chan >= 0)
			SetChannelPosition3(chan, (VECTOR*)cp->hd.where.t,
				(LONGVECTOR3*)cp->st.n.linearVelocity, -1500, pitch, 0);
	}
}

static void MpMarkGlowFor(int marked, int myId, int bomb)
{
	MP_PLAYER* mp = (marked >= 0) ? MpGetPlayer(marked) : NULL;
	int on, r, g, b;

	/* drawable is a question about the car's MODEL slot, not its car_data index: it
	 * used to be asked of the car's index, so the marked player's glow came and went
	 * with whichever resident model happened to share that number */
	if (mp == NULL || mp->carId < 0 || mp->carId >= MAX_CARS ||
	    car_data[mp->carId].controlType == CONTROL_TYPE_NONE ||
	    !JerCarSlotDrawable(car_data[mp->carId].ap.model))
	{
		jer_carglow_clear();
		return;
	}

	if (bomb)
	{
		r = 255;
		g = 0;
		b = 0;
	}
	else
	{
		if (mp->id == myId)
			on = MpModePlayerColour(gMp.gamemode, mp->id, gMp.config.colorOn,
				gMp.config.colorR, gMp.config.colorG, gMp.config.colorB,
				&r, &g, &b);
		else
			on = MpModePlayerColour(gMp.gamemode, mp->id, mp->colorOn,
				mp->colorR, mp->colorG, mp->colorB, &r, &g, &b);

		if (!on)
		{
			r = 0;
			g = 255;
			b = 255;
		}
	}

	jer_carglow_set(&car_data[mp->carId], r, g, b);
}

/* Cops n Robbers, host: the getaway's own damage (its carstate row's totalDamage /
 * maxDamage, v24, or our own car when the host drives it). MpMarkDamage moves the
 * meter; the cop it credits scores. */
void MpMarkHostDamage(int from, int damage, int maxDamage, const char* how)
{
	static int loggedRound = -1;
	int cop = -1, ev = 0, pts;

	if (!MpIsHost() || MpModeTimerKind(gMp.gamemode) != MP_MODE_TIMER_GETAWAY ||
		MpMarkPhase() != MP_MARK_PHASE_LIVE)
		return;

	pts = MpMarkDamage(from, damage, maxDamage, &cop, &ev);

	if (MpMarkDamageLive() && loggedRound != MpRoundNumber() && gMpCtx != NULL)
	{
		loggedRound = MpRoundNumber();
		gMpCtx->jer_log(gMpCtx, "[mp] mark: getaway %d reports its own damage (%s, max %d) - the meter follows it this round\n",
			from, how, maxDamage);
	}

	if (pts <= 0)
		return;

	if (cop >= 0)
		MpRoundAddScore(cop, pts);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] mark: getaway %d damage %d/%d -> meter +%d = %d/%d, %s\n",
			from, damage, maxDamage, pts, MpMarkHeat(), MP_MARK_CNR_BUST_POINTS,
			cop >= 0 ? MpMarkName(cop) : "no cop in contact (no points)");

	if (ev & MP_MARK_EV_END)
	{
		MpHostCnrFlush();
		MpRoundEnd();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark: round %d over - player %d busted (wrecked)\n",
				MpRoundNumber(), from);
	}

	MpHostSendMark(-1);
	MpHostSendRound(-1);
}

/* v24, host: a player is out of the live round (died with MP_MODE_RESPAWN_NONE).
 * Benched (MP_MARK_ENTRY_F_OUT, JPRD SPECTATING) until the next COUNTDOWN; if they
 * were marked, MpMarkOut moves the mark or ends the round (Cops n Robbers: busted).
 * dedi/core.py mark_carstate_round is the server's twin. */
void MpHostBenchOut(int id, const char* how)
{
	int ev;

	if (!MpIsHost() || !MpModeUsesTimer(gMp.gamemode) || MpMarkPhase() != MP_MARK_PHASE_LIVE ||
		!MpMarkInRound(id))
		return;

	if (id == MpMarkMarked())
		MpHostCnrFlush();	/* the getaway's points so far are banked first */

	ev = MpMarkOut(id, MpMarkRand());

	if (MpMarkBench(id) == MP_MARK_ENTRY_F_OUT)
		MpRoundSetState(id, MP_ROUND_PLAYER_SPECTATING);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] mark: player %d is out until the next round (%s, no respawn)\n", id, how);

	if (ev & MP_MARK_EV_END)
	{
		gMpCnrPending = 0;
		MpRoundEnd();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark: round %d over - the marked player is out (end reason %d)\n",
				MpRoundNumber(), MpMarkEndReason());
	}
	else if ((ev & MP_MARK_EV_MOVED) && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] mark: the marked player is out - player %d is marked now\n", MpMarkMarked());

	MpHostSendMark(-1);
	MpHostSendRound(-1);
}

/* v24, host: the round side of a client's carstate row, after the forged-row check
 * (MpHandleCarState). MP_CARSTATE_OUT benches the sender; in Cops n Robbers the
 * getaway's own totalDamage / maxDamage move the meter (this replaced MP_TAG_DAMAGE). */
void MpHostCarRowRound(int from, const MP_CARSTATE_ENTRY* e)
{
	if (!MpIsHost() || !MpModeUsesTimer(gMp.gamemode) || !MpMarkHaveState() || MpMarkMode() != gMp.gamemode)
		return;

	if ((e->flags & MP_CARSTATE_OUT) && MpMarkInRound(from))
		MpHostBenchOut(from, "carstate");

	if (e->model == MP_CARSTATE_NO_CAR || !(e->flags & MP_CARSTATE_HAS_BODY) || e->maxDamage == 0)
		return;	/* on foot / no body: no damage to read */

	MpMarkHostDamage(from, e->totalDamage, e->maxDamage, "carstate");
}

/* A3: the full pre-round repair, after the role-car swap has landed. */
static void MpRoundRepairTick(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;

	if (!gMpRoundRepairPending || MpCarChangeWaiting())
		return;

	gMpRoundRepairPending = 0;

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS || player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
		return;

	cp = &car_data[me->carId];
	MpMarkResetCarState(cp);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] round start: car repaired after the role car (slot %d model %d): damage %d, hubcaps lost 0x%x, felony %d/%d\n",
			me->carId, (int)cp->ap.model, cp->totalDamage, (int)(cp->ap.flags & 0xF),
			cp->felonyRating, pedestrianFelony);
}

/* v24: benched in a live round (a late joiner WAITING, or OUT), the camera watches
 * the round -- the marked player's car, else the first in-round car -- and comes
 * back to our own car when we are in again. The car itself is held
 * (MpRoundHoldsCar); MpCarFailsafeTick still re-seats a camera left on no body. */
static void MpSpectateTick(int phase, int myId)
{
	MP_PLAYER* me = MpLocalPlayer();
	int want, target = -1, id;

	want = me != NULL && myId >= 0 && phase == MP_MARK_PHASE_LIVE && MpMarkPresent(myId) &&
		!MpMarkInRound(myId) && player[0].playerType == PLAYER_TYPE_CAR &&
		me->carId >= 0 && me->carId < MAX_CARS;

	if (want)
	{
		for (id = -1; id < MP_MAX_PLAYERS && target < 0; id++)
		{
			int pid = (id < 0) ? MpMarkMarked() : id;
			MP_PLAYER* p = (pid >= 0 && pid != myId && MpMarkInRound(pid)) ? MpGetPlayer(pid) : NULL;

			if (p != NULL && p->carId >= 0 && p->carId < MAX_CARS &&
				car_data[p->carId].controlType != CONTROL_TYPE_NONE && JerCarSlotUsable(car_data[p->carId].ap.model))
				target = p->carId;
		}
	}

	if (!want || target < 0)
	{
		if (gMpSpectating && me != NULL && me->carId >= 0 && me->carId < MAX_CARS &&
			player[0].playerType == PLAYER_TYPE_CAR)
		{
			player[0].cameraCarId = me->carId;
			player[0].cameraAngle = car_data[me->carId].hd.direction + 1536;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] spectate: back on our own car (slot %d)\n", me->carId);
		}

		gMpSpectating = 0;
		gMpSpectateCar = -1;
		return;
	}

	if (!gMpSpectating)
	{
		if (MpMarkBench(myId) == MP_MARK_ENTRY_F_WAITING)
			jer_notify("Round in progress - spectating until the next one", 3, 4);
	}

	if (player[0].cameraCarId != target)
	{
		player[0].cameraCarId = target;
		player[0].cameraAngle = car_data[target].hd.direction + 1536;
	}

	if (target != gMpSpectateCar && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] spectate: watching car slot %d (bench 0x%x)\n", target, MpMarkBench(myId));

	gMpSpectating = 1;
	gMpSpectateCar = target;
}

/* Every machine, once a frame in a match. */
void MpMarkPresentTick(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	int myId = me != NULL ? me->id : -1;
	int phase, marked, since, ms, want;
	char text[32];

	MpRespawnHoldTick();
	MpListenRotationTick();

	if (!MpModeUsesTimer(gMp.gamemode) || !MpMarkHaveState() || MpMarkMode() != gMp.gamemode)
	{
		MpMarkPresentClear();
		return;
	}

	phase = MpMarkPhase();
	marked = MpMarkMarked();

	/* the clock: the host's is live; a client runs the ticking one on from its
	 * last snapshot (at most a second old) so it counts down smoothly */
	since = MpIsHost() ? 0 : (int)((gMp.frame - gMpMarkApplyFrame) * 1000u / 30u);
	if (since > 3000)
		since = 3000;

	MpMarkNotices(myId, since);

	/* the round-start cluster / chase: this machine places its own car once per
	 * countdown */
	if (phase == MP_MARK_PHASE_COUNTDOWN && (MpModeSpawnRule(gMp.gamemode) == MP_MODE_SPAWN_CLUSTER ||
		MpModeSpawnRule(gMp.gamemode) == MP_MODE_SPAWN_CHASE))
	{
		int chase = MpModeSpawnRule(gMp.gamemode) == MP_MODE_SPAWN_CHASE;

		if (gMpClusterState == 0)
		{
			/* WAITING is already satisfied: COUNTDOWN has just begun.
			 * The real respawn is here, once, not during waiting and not
			 * mid-count. Placement follows on this same tick. */
			gMpClusterState = 1;
			gMpClusterFrom = gMp.frame;
			gMpClusterTries = 0;
			gMpChaseLineSet = 0;
			gMpChaseReplaces = 0;
			MpMarkRoundReset();

			/* the car of the role this player has this round (role-car modes) */
			if (MpModeRoleSlotCount(gMp.gamemode) > 0)
				MpRoleCarApply();
			else
				MpPendingCarApply("round countdown");	/* a car saved in the pause menu */

			/* ...and the full repair AFTER that swap (a swap that waits for
			 * an import is repaired when it lands, MpRoundRepairTick) */
			gMpRoundRepairPending = 1;

			if (chase)
			{
				MP_PLAYER* gp = MpGetPlayer(marked);

				gMpWideRegion = -1;	/* a fresh map-wide pick every round */
				gMpWideTries = 0;
				gMpChaseFromX = gMpChaseFromZ = 0;
				gMpChaseFromSet = 0;

				if (gp != NULL && gp->carId >= 0 && gp->carId < MAX_CARS)
				{
					gMpChaseFromX = car_data[gp->carId].hd.where.t[0];
					gMpChaseFromZ = car_data[gp->carId].hd.where.t[2];
					gMpChaseFromSet = 1;
				}
			}
		}

		/* a cop already lined up, and the getaway has since landed far from where
		 * it was then: line up again behind where it is now */
		if (chase && gMpClusterState == 2 && gMpChaseLineSet && marked != myId &&
			gMpChaseReplaces < MP_CHASE_REPLACE_MAX)
		{
			MP_PLAYER* gp = MpGetPlayer(marked);

			if (gp != NULL && gp->carId >= 0 && gp->carId < MAX_CARS &&
				car_data[gp->carId].controlType != CONTROL_TYPE_NONE)
			{
				long long dx = (long long)car_data[gp->carId].hd.where.t[0] - gMpChaseLineX;
				long long dz = (long long)car_data[gp->carId].hd.where.t[2] - gMpChaseLineZ;

				if (dx * dx + dz * dz > (long long)(2 * MP_CHASE_MOVED) * (2 * MP_CHASE_MOVED))
				{
					gMpChaseReplaces++;
					gMpChaseLineSet = 0;
					gMpChaseFromX = gMpChaseLineX;	/* "has it moved?" is answered: yes */
					gMpChaseFromZ = gMpChaseLineZ;
					gMpChaseFromSet = 1;
					gMpClusterState = 1;
					gMpClusterFrom = gMp.frame;
					gMpClusterTries = 0;

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx, "[mp] chase start: the getaway landed after we lined up - lining up again (%d)\n",
							gMpChaseReplaces);
				}
			}
		}

		if (gMpClusterState == 1)
		{
			if (chase)
				MpMarkChaseTick();
			else
				MpMarkClusterTick();
		}
	}
	else
		gMpClusterState = 0;

	MpRoundRepairTick();

	if (phase == MP_MARK_PHASE_COUNTDOWN && MpMarkCountdownIsFirst())
		snprintf(text, sizeof(text), "PRE-GAME %d", (MpMarkCountdownMs(since) + 999) / 1000);
	else if (phase == MP_MARK_PHASE_WAITING)
	{
		int ready = 0, n = MpMarkCountWaiting(&ready);
		int need = MpModeWaitMinPlayers(gMp.gamemode);

		if (need < 1)
			need = 2;

		/* enough people are here but somebody is still in the car select: say so */
		if (n >= need && MpModeWaitForCars(gMp.gamemode) && ready < n)
			snprintf(text, sizeof(text), "WAITING FOR CARS %d/%d", ready, n);
		else
			snprintf(text, sizeof(text), "WAITING FOR PLAYERS");
	}
	else if (MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_GETAWAY)
	{
		/* one clock; the cops' damage meter beside it */
		ms = MpMarkClockMs(-1, since);
		snprintf(text, sizeof(text), "%s %d:%02d DMG %d%%", marked == myId ? "GETAWAY" : "COP",
			(ms + 999) / 1000 / 60, (ms + 999) / 1000 % 60,
			MpMarkHeat() * 100 / MP_MARK_CNR_BUST_POINTS);
	}
	else if (MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_SHARED)
	{
		ms = MpMarkClockMs(-1, since);
		snprintf(text, sizeof(text), "%s %d:%02d", marked == myId ? "BOMB" : "TIMER",
			(ms + 999) / 1000 / 60, (ms + 999) / 1000 % 60);
	}
	else
	{
		ms = MpMarkClockMs(myId, since);
		snprintf(text, sizeof(text), "%s %d:%02d", marked == myId ? "FOX" : "HOUND",
			(ms + 999) / 1000 / 60, (ms + 999) / 1000 % 60);
	}

	MpCnrDamageTick(phase, marked, myId);
	MpSpectateTick(phase, myId);
	MpFoxHudBuild(phase, marked, myId, since);

	/* own clock, top centre (0.23.0: back from the top right): cream while it is
	 * running, mid grey if this viewer is a frozen hound, red and flashing while
	 * they hold the bomb */
	{
		int bombKind = (MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_SHARED);
		int live = (phase == MP_MARK_PHASE_LIVE);
		int tr, tg, tb, flashOff = 0;

		if (bombKind && live && marked == myId)
		{
			tr = 128;
			tg = 0;
			tb = 0;
		}
		else if (!bombKind && live && myId >= 0 && marked != myId &&
			MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_PERSONAL)
		{
			tr = 128;
			tg = 128;
			tb = 128;	/* frozen hound */
		}
		else
		{
			tr = 128;
			tg = 128;
			tb = 64;	/* cream: running fox, shared timer, lobby */
		}

		gMpHudR = MpHudLerp(gMpHudR, tr);
		gMpHudG = MpHudLerp(gMpHudG, tg);
		gMpHudB = MpHudLerp(gMpHudB, tb);

		/* the digit timers replace the text clock in every timer mode (MpFoxHudBuild) */
		if (gMpFoxHud.on)
		{
			if (gMpMarkPanelOn)
				jer_hud_panel_clear(MP_MARK_PANEL_SLOT);
			gMpMarkPanelOn = 0;
		}
		else
		{
			jer_hud_panel(MP_MARK_PANEL_SLOT, JER_HUD_ANCHOR_TOP_CENTRE,
				flashOff ? "" : text, gMpHudR, gMpHudG, gMpHudB);
			gMpMarkPanelOn = 1;
		}
	}

	/* the arrow over the marked car: never our own (we have the notice and the
	 * clock), only while a round is live */
	{
		MP_PLAYER* mp = (phase == MP_MARK_PHASE_LIVE && marked >= 0 && marked != myId) ? MpGetPlayer(marked) : NULL;

		if (mp != NULL && mp->carId >= 0 && mp->carId < MAX_CARS && jer_arrow_on_car(MP_MARK_ARROW_SLOT, &car_data[mp->carId]))
			gMpMarkArrowOn = 1;
		else if (gMpMarkArrowOn)
		{
			jer_arrow_clear(MP_MARK_ARROW_SLOT);
			gMpMarkArrowOn = 0;
		}
	}

	/* the mode's music while live; the engine's own otherwise */
	want = (phase == MP_MARK_PHASE_LIVE) ? MpModeMusic(gMp.gamemode) : MP_MODE_MUSIC_AUTO;

	if (want != MP_MODE_MUSIC_AUTO)
	{
		if (jer_music_forced() != want)
			jer_music_force(want);	/* MP_MODE_MUSIC_* == JER_MUSIC_* */
		gMpMarkMusicOn = 1;
	}
	else if (gMpMarkMusicOn)
	{
		jer_music_force(JER_MUSIC_AUTO);
		gMpMarkMusicOn = 0;
	}

	/* presentation only: tick, wreck, glow, fox centre line. No wire change. */
	{
		int bombKind = (MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_SHARED);
		int live = (phase == MP_MARK_PHASE_LIVE);
		MP_PLAYER* markedPl = (marked >= 0) ? MpGetPlayer(marked) : NULL;
		CAR_DATA* markedCar = (markedPl != NULL && markedPl->carId >= 0 &&
			markedPl->carId < MAX_CARS) ? &car_data[markedPl->carId] : NULL;

		if (bombKind && live && markedCar != NULL)
		{
			int rem = MpMarkClockMs(-1, since);
			int full = MpMarkSeconds() * 1000;	/* the host's time, from the wire */

			if (full < 15000)
				full = 15000;

			MpMarkBeepBomb(markedCar, rem, full, marked == myId);
		}
		else
			gMpMarkTickFrame = -1;

		if (bombKind && markedCar != NULL)
		{
			/* only the clock sets it off: a round that ended any other way
			 * (a leaver, MpMarkSync / MpMarkTick) has nobody marked anyway */
			if (phase == MP_MARK_PHASE_ENDED && MpMarkEndReason() == MP_MARK_END_CLOCK)
				jer_carexplode(markedCar);

			jer_carexplode_hold(markedCar);
		}

		int cnrKind = (MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_GETAWAY);

		if (live && marked >= 0 && !cnrKind)	/* CnR: no shell on the getaway (0.24.0 polish) */
			MpMarkGlowFor(marked, myId, bombKind);
		else
			jer_carglow_clear();

		/* Cops n Robbers: the map's red/blue felony flash for the whole chase, on
		 * every machine (overmap.c gJerMapForceFlash), and the cops' radio */
		gJerMapForceFlash = (cnrKind && live) ? 1 : 0;
		MpCnrRadioTick(cnrKind && live, marked, myId);

		/* ...and no "hit another vehicle" line for a cop-vs-getaway hit */
		gMpCnrHitMarked = (cnrKind && live) ? marked : -1;
		gJerCopHitLineVeto = (cnrKind && live && marked >= 0) ? MpCnrHitLineVeto : NULL;

		gMpFoxLinesOn = 0;
		gMpFoxLine1[0] = '\0';
		gMpFoxLine2[0] = '\0';

		if (cnrKind && marked >= 0 && (live || phase == MP_MARK_PHASE_COUNTDOWN))
		{
			/* who is who, from the countdown on (the getaway is named there) */
			gMpFoxLinesOn = 1;

			if (marked == myId)
				snprintf(gMpFoxLine1, sizeof(gMpFoxLine1), "You're the getaway!");
			else
				snprintf(gMpFoxLine1, sizeof(gMpFoxLine1), "%s is the getaway!", MpMarkName(marked));
		}
		/* Fox N Hounds: who the fox is is the timer title (MpFoxHudBuild) */
	}
}

/* Fox N Hounds timers in the mission timer's own font (mdraw.c DrawTimer:
 * PrintDigit "MM:SS", then PrintString ".FF" at y + 13). `scale` 16 = that size.
 * Colour: grey while frozen, fading to cyan while this player is the fox. */
static void MpFoxTimerColour(int id)
{
	int f = (id >= 0 && id < MP_MAX_PLAYERS) ? gMpFoxFade[id] : 0;

	if (gMpFoxHud.kind == MP_MODE_TIMER_SHARED)
	{
		/* white (128,128,128) -> red (128,16,16) while WE hold the bomb, and back */
		f = gMpBombFade;
		SetTextColour(128, 128 + (16 - 128) * f / MP_FOX_FADE_FRAMES,
			128 + (16 - 128) * f / MP_FOX_FADE_FRAMES);
		return;
	}

	if (gMpFoxHud.kind == MP_MODE_TIMER_GETAWAY)
	{
		SetTextColour(128, 128, 128);	/* white: the clock is everyone's */
		return;
	}

	/* grey (72,72,72) -> cyan (24,112,128); 128 = the texture's own brightness */
	SetTextColour(72 + (24 - 72) * f / MP_FOX_FADE_FRAMES,
		72 + (112 - 72) * f / MP_FOX_FADE_FRAMES,
		72 + (128 - 72) * f / MP_FOX_FADE_FRAMES);
}

/* One timer, its right edge at `right` (or centred on the digits when right < 0:
 * the ".FF" hangs right, which keeps it clear of the Damage bar, overlay.c x
 * 16..118 y 24). Returns its left x. */
static int MpFoxDrawTimer(int id, int ms, int right, int y, int scale)
{
	MpFoxTimerColour(id);

	if (right < 0)
		return PrintTimer(160, y, ms, scale, PRINT_TIMER_CENTRE);

	return PrintTimer(right, y, ms, scale, PRINT_TIMER_RIGHT);
}

/* Every frame from MpMarkPresentTick: what the overlay draws, and the fades. */
static void MpFoxHudBuild(int phase, int marked, int myId, int since)
{
	int id, live = phase == MP_MARK_PHASE_LIVE;

	int kind = MpModeTimerKind(gMp.gamemode);

	gMpFoxHud.kind = kind;
	gMpFoxHud.on = (kind == MP_MODE_TIMER_PERSONAL || kind == MP_MODE_TIMER_SHARED ||
		kind == MP_MODE_TIMER_GETAWAY) &&
		(live || phase == MP_MARK_PHASE_ENDED ||
		 (phase == MP_MARK_PHASE_COUNTDOWN && !MpMarkCountdownIsFirst()));

	if (!gMpFoxHud.on)
	{
		memset(gMpFoxFade, 0, sizeof(gMpFoxFade));
		gMpBombFade = 0;
		return;
	}

	gMpFoxHud.sub[0] = '\0';

	if (kind != MP_MODE_TIMER_PERSONAL)
	{
		/* one round clock. Bomb: it fades to red while WE hold the bomb (and
		 * stays red as it goes off), back to white once we pass it. */
		int hold = (kind == MP_MODE_TIMER_SHARED && marked >= 0 && marked == myId &&
			(live || phase == MP_MARK_PHASE_ENDED));

		if (hold && gMpBombFade < MP_FOX_FADE_FRAMES)
			gMpBombFade++;
		else if (!hold && gMpBombFade > 0)
			gMpBombFade--;

		gMpFoxHud.shared = MpMarkClockMs(-1, since);
		gMpFoxHud.me = -1;
		gMpFoxHud.marked = marked;
		memset(gMpFoxHud.ms, 0xff, sizeof(gMpFoxHud.ms));	/* no side column */

		if (kind == MP_MODE_TIMER_GETAWAY)
			snprintf(gMpFoxHud.sub, sizeof(gMpFoxHud.sub), "DAMAGE %d%%",
				MpMarkHeat() * 100 / MP_MARK_CNR_BUST_POINTS);

		gMpFoxHud.title = marked >= 0 && (live || phase == MP_MARK_PHASE_COUNTDOWN);

		if (gMpFoxHud.title)
		{
			char name[16];
			int k;

			snprintf(name, sizeof(name), "%s", MpMarkName(marked));
			for (k = 0; name[k] != '\0'; k++)
				if (name[k] >= 'a' && name[k] <= 'z')
					name[k] = (char)(name[k] - 'a' + 'A');

			if (kind == MP_MODE_TIMER_SHARED)
			{
				if (marked == myId)
					snprintf(gMpFoxHud.titleText, sizeof(gMpFoxHud.titleText), "YOU HAVE THE BOMB!");
				else
					snprintf(gMpFoxHud.titleText, sizeof(gMpFoxHud.titleText), "%s HAS THE BOMB!",
						name[0] != '\0' ? name : "?");
			}
			else if (marked == myId)
				snprintf(gMpFoxHud.titleText, sizeof(gMpFoxHud.titleText), "YOU'RE THE GETAWAY!");
			else
				snprintf(gMpFoxHud.titleText, sizeof(gMpFoxHud.titleText), "%s IS THE GETAWAY!",
					name[0] != '\0' ? name : "?");
		}
		return;
	}

	gMpFoxHud.marked = live ? marked : -1;
	gMpFoxHud.me = (myId >= 0 && MpMarkPresent(myId)) ? myId : -1;

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		gMpFoxHud.ms[id] = MpMarkPresent(id) ? MpMarkClockMs(id, since) : -1;

		if (live && id == marked)
		{
			if (gMpFoxFade[id] < MP_FOX_FADE_FRAMES)
				gMpFoxFade[id]++;
		}
		else if (gMpFoxFade[id] > 0)
			gMpFoxFade[id]--;
	}

	gMpFoxHud.title = live && marked >= 0;

	if (gMpFoxHud.title)
	{
		if (marked == myId)
			snprintf(gMpFoxHud.titleText, sizeof(gMpFoxHud.titleText), "YOU ARE THE FOX!");
		else
		{
			char name[16];
			int k;

			snprintf(name, sizeof(name), "%s", MpMarkName(marked));
			for (k = 0; name[k] != '\0'; k++)
				if (name[k] >= 'a' && name[k] <= 'z')
					name[k] = (char)(name[k] - 'a' + 'A');

			snprintf(gMpFoxHud.titleText, sizeof(gMpFoxHud.titleText), "%s IS THE FOX!",
				name[0] != '\0' ? name : "?");
		}
	}
}

/* The persistent overlay lines, from DRAW_OVERLAY: Cops n Robbers' role line
 * just under the top-centre clock (0.23.4: off the centre, where the engine's
 * notices are), and Fox N Hounds' timers. */
void MpMarkDrawOverlay(void)
{
	extern int gShowMap;

	if (gShowMap)
		return;	/* the full map is up: it owns the screen */

	/* v24: the race start's own 3-2-1 (mdraw.c DrawMission: y 96, golden, each
	 * digit shrinking over its second), then a big GO for a second */
	if (gMp321Count > 0)
	{
		SetTextColour(128, 128, 64);
		Print321Go(96, gMp321Count, gMp321Scale);
	}
	else if (gMpGoFrame >= 0 && (int)gMp.frame - gMpGoFrame < 30)
	{
		SetTextColour(128, 128, 64);
		Print321Go(96, 0, 32 - ((int)gMp.frame - gMpGoFrame) * 16 / 30);	/* 32 -> 16 */
	}

	if (gMpFoxLinesOn && gMpFoxLine1[0] != '\0' && !gMpFoxHud.on)
	{
		SetTextColour(128, 128, 64);
		PrintStringCentred(gMpFoxLine1, 38);	/* JER_HUD_FIRST_Y 22 + one 16 px row */
	}

	if (gMpFoxHud.on)
	{
		int id, row = 0, more = 0, sideX;

		jer_screen_edges(NULL, &sideX);
		sideX -= MP_FOX_SIDE_INSET;

		if (gMpFoxHud.title)
		{
			MpFoxTimerColour(gMpFoxHud.marked);
			PrintStringCentred(gMpFoxHud.titleText, MP_FOX_TITLE_Y);
		}

		/* the round clock (bomb, CnR): the mission timer's size, top centre */
		if (gMpFoxHud.kind != MP_MODE_TIMER_PERSONAL)
		{
			MpFoxDrawTimer(-1, gMpFoxHud.shared, -1, MP_FOX_OWN_Y, 16);

			if (gMpFoxHud.sub[0] != '\0')
				PrintStringCentred(gMpFoxHud.sub, MP_FOX_OWN_Y + 30);
		}

		/* our own: the mission timer's size, top centre */
		if (gMpFoxHud.me >= 0)
			MpFoxDrawTimer(gMpFoxHud.me, gMpFoxHud.ms[gMpFoxHud.me], -1, MP_FOX_OWN_Y, 16);

		/* everyone else: smaller, down the right, each with a name */
		for (id = 0; id < MP_MAX_PLAYERS; id++)
		{
			char label[16];
			int y, x;

			if (id == gMpFoxHud.me || gMpFoxHud.ms[id] < 0)
				continue;

			if (row >= MP_FOX_SIDE_ROWS)
			{
				more++;
				continue;
			}

			y = MP_FOX_SIDE_Y + row * MP_FOX_SIDE_STEP;
			x = MpFoxDrawTimer(id, gMpFoxHud.ms[id], sideX, y, MP_FOX_SIDE_SCALE);

			snprintf(label, sizeof(label), "%.8s", MpMarkName(id));
			if (label[0] == '\0')
				snprintf(label, sizeof(label), "P%d", id + 1);

			MpFoxTimerColour(id);
			PrintStringRightAligned(label, x - 4, y + 3);
			row++;
		}

		if (more > 0)
		{
			char extra[8];

			snprintf(extra, sizeof(extra), "+%d", more);
			SetTextColour(72, 72, 72);
			PrintStringRightAligned(extra, sideX, MP_FOX_SIDE_Y + row * MP_FOX_SIDE_STEP + 3);
		}
	}
}

/* The repair-and-unlock half of a countdown-start respawn. Placement
 * (road, heading) is separate. Also used after InitCarPhysics, which
 * zeros thrust and wheel_angle but leaves handbrake and the wreck latch. */
static void MpMarkResetCarState(CAR_DATA* cp)
{
	if (cp == NULL)
		return;

	cp->totalDamage = 0;
	memset(cp->ap.damage, 0, sizeof(cp->ap.damage));
	CreateDentableCar(cp);
	DentCar(cp);		/* MpSelfRepairCar's recipe: the geometry AND the damage UVs pristine */
	CarReapplyHubcapState(cp->id);
	cp->ap.needsDenting = 0;
	cp->hd.speed = 0;
	cp->hd.wheel_speed = 0;
	cp->wheel_angle = 0;
	cp->thrust = 0;
	cp->handbrake = 0;
	cp->wheelspin = 0;
	MpClearFelony(0);	/* no "Wanted level cleared" notice every round */
	jer_carexplode_reset(cp->id);
}

/* Once, when this machine first sees COUNTDOWN. Clears the wreck latch
 * on every slot (hold is a local presentation), then repairs the local
 * car even if the cluster has not found a road yet. */
static void MpMarkRoundReset(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;
	JER_ARGS_RESET_CAR rc;

	jer_carexplode_reset(-1);
	jer_carglow_clear();
	MpClearDeathState();
	gMpRespawnOut = 0;		/* v24: everyone is back in at the countdown */
	gMpRespawnDelayArmed = 0;

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	if (player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
		return;

	/* a death the hold kept (MpRespawnAfterDeath) is answered by this reset */
	gMpRespawnDeferred = 0;

	cp = &car_data[me->carId];
	MpMarkResetCarState(cp);

	rc.carId = me->carId;
	jer_fire(JER_EVENT_RESET_CAR, &rc);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] round start: player %d respawned as the countdown began (wreck and lock cleared)\n",
			me->id);
}

/* Land our car at (x,z) facing `dir`, stopped and repaired. 0 = no ground. */
static int MpClusterPlace(MP_PLAYER* me, CAR_DATA* cp, int x, int z, int dir, int anchorId, const char* how)
{
	JER_ARGS_RESET_CAR rc;

	if (!MpWarpStreamTo(x, z) || !MpPlaceOnGround(cp, x, z, dir))
		return 0;

	/* InitCarPhysics (inside PlaceOnGround) does not clear handbrake or the latch */
	MpMarkResetCarState(cp);

	player[0].cameraAngle = cp->hd.direction + 1536;
	player[0].cameraCarId = cp->id;

	rc.carId = me->carId;
	jer_fire(JER_EVENT_RESET_CAR, &rc);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] round start: player %d placed at %d,%d heading %d (%s, anchor player %d, try %d, %u frame(s) into the countdown)\n",
			me->id, cp->hd.where.t[0], cp->hd.where.t[2], dir & 0xfff, how, anchorId,
			gMpClusterTries, gMp.frame - gMpClusterFrom);

	return 1;
}

static void MpMarkClusterTick(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;
	unsigned int rnd;
	int id, anchorId = -1, rank = 0, ax, az, t;

	if (me == NULL || !gMp.running)
		return;

	/* ON FOOT: there is no car to put on the line; the pad hold still applies */
	if (me->carId < 0 || me->carId >= MAX_CARS || player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] round start: player %d is on foot - not placed in the start cluster\n", me->id);
		gMpClusterState = 2;
		return;
	}

	cp = &car_data[me->carId];

	/* the anchor: the lowest present id with a car this machine can see */
	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p;

		if (!MpMarkPresent(id))
			continue;

		p = MpGetPlayer(id);

		if (p != NULL && p->carId >= 0 && p->carId < MAX_CARS &&
			car_data[p->carId].controlType != CONTROL_TYPE_NONE)
		{
			anchorId = id;
			break;
		}
	}

	if (anchorId < 0)
		anchorId = me->id;	/* nobody else has a car here yet: we are the anchor */

	gMpClusterTries++;
	rnd = gMp.seed ^ (unsigned int)MpNowMs() ^ (unsigned int)((me->id + 11) * 2654435761u) ^ (unsigned int)gMpClusterTries;
	rnd = rnd * 1103515245u + 12345u;

	/* THE ANCHOR: onto the nearest road, facing along its lane */
	if (anchorId == me->id)
	{
		int x = 0, z = 0, dir = -1, ring = 0;

		/* the road's lane heading (MpRoadHeadingAt); random only when the road
		 * gave none (before 0.23.0 it was always random, `dir` ignored) */
		if (MpFindRoadSpotNear(cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -1, &x, &z, &dir, &ring) &&
			MpClusterPlace(me, cp, x, z, dir >= 0 ? dir : (int)((rnd >> 8) & 0xfff), me->id, "anchor, nearest road"))
		{
			gMpClusterState = 2;
			return;
		}

		/* NO ROAD NEAR WHERE THE ANCHOR ENDED THE LAST ROUND -- off the edge of an
		 * arena (Ghost Town), in a field, in the water. The level's own start is a
		 * road by construction: start the cluster there instead of "where it is",
		 * which is how the whole lobby used to begin a round out of bounds. */
		if (PlayerStartInfo[0] != NULL)
		{
			int sx = PlayerStartInfo[0]->position.vx;
			int sz = PlayerStartInfo[0]->position.vz;

			if (gMpClusterTries == 1)
				MpWarpStreamTo(sx, sz);

			if (MpFindRoadSpotNear(sx, 0, sz, -1, &x, &z, &dir, &ring) &&
				MpClusterPlace(me, cp, x, z, dir >= 0 ? dir : PlayerStartInfo[0]->rotation, me->id,
					"anchor, no road near its car - the level start"))
			{
				gMpClusterState = 2;
				return;
			}
		}

		if (gMp.frame - gMpClusterFrom >= (unsigned)MP_CLUSTER_GIVEUP_FRAMES)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] round start: anchor player %d found no road near %d,%d - starts where it is\n",
					me->id, cp->hd.where.t[0], cp->hd.where.t[2]);
			gMpClusterState = 2;
		}
		return;
	}

	/* everyone else: ring around the anchor's current pose, this same
	 * countdown-start frame -- do not wait mid-count for the snap */
	{
		MP_PLAYER* ap = MpGetPlayer(anchorId);

		ax = car_data[ap->carId].hd.where.t[0];
		az = car_data[ap->carId].hd.where.t[2];
	}

	/* our slot: our place among the present players, the anchor left out */
	for (id = 0; id < me->id && id < MP_MAX_PLAYERS; id++)
		if (id != anchorId && MpMarkPresent(id))
			rank++;

	/* the anchor's ground first, as the pause-menu warp does (once: forcing it
	 * every frame would fight the streamer that follows our own car) */
	if (gMpClusterTries == 1)
		MpWarpStreamTo(ax, az);

	for (t = 0; t < MP_CLUSTER_TRIES_PER_FRAME; t++)
	{
		int ringNo = 1 + rank / MP_CLUSTER_PER_RING;
		int ang = ((rank % MP_CLUSTER_PER_RING) * 4096 / MP_CLUSTER_PER_RING +
			((gMpClusterTries - 1) * MP_CLUSTER_TRIES_PER_FRAME + t) * 331) & 0xfff;	/* first candidate = the rank's own slot */
		int r = ringNo * MP_CLUSTER_STEP;
		int cx = ax + (int)(((long)rsin(ang) * r) >> 12);
		int cz = az + (int)(((long)rcos(ang) * r) >> 12);
		int x = 0, z = 0, dir = -1;
		long long dx, dz;

		if (!MpFindRoadSpotNear(cx, 0, cz, me->id + (gMpClusterTries * MP_CLUSTER_TRIES_PER_FRAME + t) * 64, &x, &z, &dir, NULL))
			continue;

		dx = (long long)x - ax;
		dz = (long long)z - az;

		if (dx * dx + dz * dz > (long long)MP_CLUSTER_RADIUS * MP_CLUSTER_RADIUS)
			continue;	/* the nearest road to the slot is too far from the anchor */

		dx = (long long)x - cx;
		dz = (long long)z - cz;

		if (dx * dx + dz * dz > (long long)MP_CLUSTER_DRIFT * MP_CLUSTER_DRIFT)
			continue;	/* snapped into a neighbour's slot: the other machine cannot see us yet */

		if (!MpJoinSpotClearOfPlayers(x, z, me->id, me->carId))
			continue;

		rnd = rnd * 1103515245u + 12345u;

		if (MpClusterPlace(me, cp, x, z, dir >= 0 ? dir : (int)((rnd >> 8) & 0xfff), anchorId, "start cluster"))
		{
			gMpClusterState = 2;
			return;
		}
	}

	if (gMp.frame - gMpClusterFrom < (unsigned)MP_CLUSTER_GIVEUP_FRAMES)
		return;		/* the ground may still be streaming: next frame */

	/* LAST RESORT: the pause menu's warp beside the anchor (its heading, not a
	 * random one). Failing that, start where we are. */
	if (MpWarpToPlayer(anchorId))
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] round start: no clear road in the cluster around player %d - warped beside them instead\n",
				anchorId);
	}
	else if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] round start: no clear road within %d of player %d's car at %d,%d - player %d starts where it is\n",
			MP_CLUSTER_RADIUS, anchorId, ax, az, me->id);

	gMpClusterState = 2;
}

/* the road's heading at (x,z), turned to the way closer to `want` -- along the
 * ROUTE the getaway took, whatever the lane under this point says */
static int MpChaseHeading(int x, int z, int want)
{
	int dir;

	if (!MpRoadHeadingAt(x, 0, z, &dir))
		return want;

	if (((dir - want + 4096 + 1024) & 0xfff) >= 2048)
		dir = (dir + 2048) & 0xfff;

	return dir;
}

static void MpMarkChaseTick(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;
	int marked = MpMarkMarked();

	if (me == NULL || !gMp.running)
		return;

	if (me->carId < 0 || me->carId >= MAX_CARS || player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
	{
		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] chase start: player %d is on foot - not placed\n", me->id);
		gMpClusterState = 2;
		return;
	}

	cp = &car_data[me->carId];
	gMpClusterTries++;

	/* THE GETAWAY: a random road anywhere on the map, along its lane */
	if (marked == me->id)
	{
		int x = 0, z = 0, dir = -1;

		if (MpFindRoadSpotWide(me->carId, &x, &z, &dir))
		{
			unsigned rnd = (unsigned)MpNowMs() * 1103515245u + 12345u;

			if (MpClusterPlace(me, cp, x, z, dir >= 0 ? dir : (int)((rnd >> 8) & 0xfff), me->id,
				"getaway, a random road on the map"))
			{
				gMpClusterState = 2;
				return;
			}
		}
		else if (gMpWideTries >= MP_JOIN_WIDE_TRIES)
		{
			gMpWideRegion = -1;	/* that region had no clear road: another one */
			gMpWideTries = 0;
		}

		if (gMp.frame - gMpClusterFrom >= (unsigned)MP_CHASE_GIVEUP_FRAMES)
		{
			int ring = 0;

			if (!(MpFindRoadSpotNear(cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -1, &x, &z, &dir, &ring) &&
				MpClusterPlace(me, cp, x, z, dir >= 0 ? dir : cp->hd.direction, me->id, "getaway, nearest road (no map-wide spot)")) &&
				gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] chase start: getaway player %d found no road - starts where it is\n", me->id);
			gMpClusterState = 2;
		}
		return;
	}

	/* A COP: behind the getaway, once its new pose is here */
	{
		MP_PLAYER* gp = MpGetPlayer(marked);
		CAR_DATA* gc;
		int gx, gz, h, rank = 0, id, want, walked = 0, x, z;
		unsigned rnd;
		long long mx, mz;

		if (gp == NULL || gp->carId < 0 || gp->carId >= MAX_CARS ||
			car_data[gp->carId].controlType == CONTROL_TYPE_NONE)
		{
			if (gMp.frame - gMpClusterFrom >= (unsigned)MP_CHASE_GIVEUP_FRAMES)
			{
				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] chase start: no getaway car (player %d) here - player %d starts where it is\n",
						marked, me->id);
				gMpClusterState = 2;
			}
			return;
		}

		gc = &car_data[gp->carId];
		gx = gc->hd.where.t[0];
		gz = gc->hd.where.t[2];

		/* The baseline is captured when the getaway's car is first VISIBLE, not
		 * when the countdown began: on the first round its car may not exist here
		 * yet, and a 0 baseline makes "has it moved?" pass at once -- the cops
		 * then line up behind the getaway's session-spawn pose, not its map-wide
		 * one. */
		if (!gMpChaseFromSet)
		{
			gMpChaseFromSet = 1;
			gMpChaseFromX = gx;
			gMpChaseFromZ = gz;
		}

		mx = (long long)gx - gMpChaseFromX;
		mz = (long long)gz - gMpChaseFromZ;

		/* the getaway has not landed yet (on this machine): wait for it to move, or
		 * for a state of its that is clearly from after the count began (it places
		 * on its first countdown frame; a landing nearby moves it less than 2000) */
		if (mx * mx + mz * mz < (long long)MP_CHASE_MOVED * MP_CHASE_MOVED &&
			gMp.frame - gMpClusterFrom < (unsigned)MP_CHASE_WAIT_FRAMES &&
			!(gp->lastStateFrame != 0 &&
			  (long)(gp->lastStateFrame - gMpClusterFrom) >= MP_CHASE_FRESH_FRAMES))
			return;

		for (id = 0; id < me->id && id < MP_MAX_PLAYERS; id++)
			if (id != marked && MpMarkPresent(id))
				rank++;

		if (gMpClusterTries == 1 || (gMpClusterTries % 15) == 0)
			MpWarpStreamTo(gx, gz);

		rnd = gMp.seed ^ (unsigned)MpNowMs() ^ (unsigned)((me->id + 7) * 2654435761u);
		rnd = rnd * 1103515245u + 12345u;
		want = MP_CHASE_FIRST + rank * MP_CHASE_GAP + (int)((rnd >> 8) % (unsigned)(MP_CHASE_JITTER + 1));

		h = MpChaseHeading(gx, gz, gc->hd.direction & 0xfff);
		x = gx;
		z = gz;

		while (walked < want + MP_CHASE_EXTRA)
		{
			int nx = x - (int)(((long)rsin(h) * MP_CHASE_STEP) >> 12);
			int nz = z - (int)(((long)rcos(h) * MP_CHASE_STEP) >> 12);

			if (!JerRoadAt(nx, 0, nz))
			{
				int sx = 0, sz = 0;
				long long ddx, ddz;

				if (!MpFindRoadSpotNear(nx, 0, nz, -1, &sx, &sz, NULL, NULL))
					break;

				ddx = (long long)sx - nx;
				ddz = (long long)sz - nz;

				if (ddx * ddx + ddz * ddz > (long long)MP_CHASE_SNAP * MP_CHASE_SNAP)
					break;	/* a dead end: the road does not go on behind */

				ddx = (long long)sx - x;
				ddz = (long long)sz - z;

				if (ddx * ddx + ddz * ddz < (long long)(MP_CHASE_STEP / 2) * (MP_CHASE_STEP / 2))
					break;	/* snapped back onto the last spot: `walked` grew, the car did not --
						 * every cop then landed on this one spot (Babou log, 30060,7938) */

				nx = sx;
				nz = sz;
			}

			x = nx;
			z = nz;
			h = MpChaseHeading(x, z, h);
			walked += MP_CHASE_STEP;

			if (walked >= want && MpWarpSpotOk(x, z) && MpJoinSpotClearOfPlayers(x, z, me->id, me->carId))
			{
				if (MpClusterPlace(me, cp, x, z, h, marked, "cop, behind the getaway"))
				{
					gMpChaseLineX = gx;
					gMpChaseLineZ = gz;
					gMpChaseLineSet = 1;
					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx, "[mp] chase start: player %d is %d units behind the getaway (rank %d, wanted %d)\n",
							me->id, walked, rank, want);
					gMpClusterState = 2;
					return;
				}
			}
		}

		/* the walk found nothing (a dead end close behind, or the road not
		 * resident yet): a spot this far behind in a straight line, facing the
		 * getaway's way */
		if (walked < want)
		{
			int bx = gx - (int)(((long)rsin(h) * want) >> 12);
			int bz = gz - (int)(((long)rcos(h) * want) >> 12);
			int dir = -1;

			if (MpFindRoadSpotNear(bx, 0, bz, me->id + gMpClusterTries * 64, &x, &z, &dir, NULL) &&
				((long long)(x - bx) * (x - bx) + (long long)(z - bz) * (z - bz)) <=
					(long long)MP_CLUSTER_DRIFT * MP_CLUSTER_DRIFT &&	/* our band, not the next cop's */
				MpJoinSpotClearOfPlayers(x, z, me->id, me->carId) &&
				MpClusterPlace(me, cp, x, z, MpChaseHeading(x, z, gc->hd.direction & 0xfff), marked,
					"cop, near the road behind the getaway"))
			{
				gMpChaseLineX = gx;
				gMpChaseLineZ = gz;
				gMpChaseLineSet = 1;
				gMpClusterState = 2;
				return;
			}
		}

		if (gMp.frame - gMpClusterFrom < (unsigned)MP_CHASE_GIVEUP_FRAMES)
			return;

		if (MpWarpToPlayer(marked))
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] chase start: no road behind player %d - warped beside them instead\n", marked);
		}
		else if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] chase start: no road behind player %d - player %d starts where it is\n",
				marked, me->id);

		gMpClusterState = 2;
	}
}
