/*
 * mp_cnr.c -- split out of mp_session.c
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"

#define MP_RADIO_CHATTER_FRAMES	720	/* cop_ai.c: TimeSinceLastSpeech > 720 */
#define MP_RADIO_CHATTER_JITTER	300
#define MP_RADIO_DAMAGE_FRAMES	90	/* a damage line at most every 3 s */
#define MP_RADIO_LINE_MAX_FRAMES	150
#define MP_RADIO_MIN_SAMPLES	18	/* the phrase bank reaches sample 17 */
#define MP_RADIO_RANGE		16000
#define MP_RADIO_SCORER_FRAMES	90	/* the scorer says the damage line within 3 s */

/* Cops n Robbers (MP_MODE_TIMER_GETAWAY) on THIS machine */
int gMpChaseFromX, gMpChaseFromZ;	/* the getaway's pose when the countdown began */
int gMpChaseFromSet;			/* ...actually captured (the car may not exist yet) */
int gMpCnrPending;		/* host: the getaway's drive points not yet in the round */
int gMpCnrEscapeMs;		/* host: how long the getaway has been clear of every cop */
static int gMpRadioChan = -1;		/* the police radio's voice (one line at a time, per listener) */

static int gMpRadioPlaying = -1;	/* the channel the current line plays on, -1 = quiet */

static int gMpRadioFollow;		/* the heading clip still to say after it, 0 = none */

static int gMpRadioFollowCar = -1;	/* ...from this car slot */

static unsigned gMpRadioNext;		/* no new line before this frame */

static int gMpRadioHeat;		/* the heat meter last heard */

static int gMpRadioSaidGo;

static int gMpRadioLastSpeaker = -1;		/* player id of the last cop that spoke */

static int gMpRadioScorer = -1;			/* the last cop whose score went up */

static unsigned gMpRadioScorerFrame;

static int gMpRadioScore[MP_MAX_PLAYERS];

static int gMpRadioScoreInit;

static unsigned gMpRadioPlayFrame;		/* when the playing line started */

extern int gDoCopSpeech;	/* gamesnd.c: 0 on the gang missions */

void MpHostCnrFlush(void);
static void MpCnrRadioScorerTick(int marked);
static void MpCnrRadioSay(int carSlot, int sample, int local);

/* Called from MpSessionReset: this module's state is per session, so
 * the next match starts from the same values the module loaded with. */
void MpCnrReset(void)
{
	/* no session state of its own */
}

/* Cops n Robbers, host: the getaway's points banked into the round (once a second,
 * so JPRD is not sent every frame for a score that moves every frame). */
void MpHostCnrFlush(void)
{
	if (gMpCnrPending > 0 && MpMarkMarked() >= 0)
		MpRoundAddScore(MpMarkMarked(), gMpCnrPending);

	gMpCnrPending = 0;
}

/* Cops n Robbers, host, once a frame while live: the getaway's drive points and
 * the escape -- clear of EVERY cop by MP_MARK_CNR_ESCAPE_DIST (x/z) for
 * MP_MARK_CNR_ESCAPE_HOLD_MS. dedi/core.py _getaway_step is the server's twin. */
int MpHostCnrTick(int ms)
{
	MP_PLAYER* gp;
	CAR_DATA* gc = NULL;
	int ev = 0, id, cops = 0, clear = 1, speed = 0;

	if (MpModeTimerKind(gMp.gamemode) != MP_MODE_TIMER_GETAWAY || MpMarkPhase() != MP_MARK_PHASE_LIVE)
	{
		gMpCnrEscapeMs = 0;
		return 0;
	}

	gp = MpGetPlayer(MpMarkMarked());

	if (gp != NULL && gp->carId >= 0 && gp->carId < MAX_CARS)
		gc = &car_data[gp->carId];	/* on foot: no car, no drive bonus, no escape */

	if (gc != NULL)
	{
		/* whole units/frame: the velocity is 1 unit/frame << 12 (MP_FIXEDH) */
		double vx = (double)gc->st.n.linearVelocity[0] / 4096.0;
		double vz = (double)gc->st.n.linearVelocity[2] / 4096.0;

		speed = (int)sqrt(vx * vx + vz * vz);
	}

	gMpCnrPending += MpMarkDrive(ms, speed);

	for (id = 0; gc != NULL && id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p;
		long long dx, dz;

		if (id == MpMarkMarked() || !MpMarkInRound(id))
			continue;	/* v24: a benched player is no cop */

		p = MpGetPlayer(id);

		if (p == NULL || p->carId < 0 || p->carId >= MAX_CARS)
			continue;

		cops++;
		dx = (long long)car_data[p->carId].hd.where.t[0] - gc->hd.where.t[0];
		dz = (long long)car_data[p->carId].hd.where.t[2] - gc->hd.where.t[2];

		if (dx * dx + dz * dz < (long long)MP_MARK_CNR_ESCAPE_DIST * MP_MARK_CNR_ESCAPE_DIST)
			clear = 0;
	}

	if (gc != NULL && cops > 0 && clear)
		gMpCnrEscapeMs += ms;
	else
		gMpCnrEscapeMs = 0;

	if (gMpCnrEscapeMs >= MP_MARK_CNR_ESCAPE_HOLD_MS)
	{
		gMpCnrEscapeMs = 0;
		MpHostCnrFlush();
		ev = MpMarkEscape();

		if ((ev & MP_MARK_EV_END) && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] mark: player %d got away - %d units clear of every cop for %d ms\n",
				MpMarkMarked(), MP_MARK_CNR_ESCAPE_DIST, MP_MARK_CNR_ESCAPE_HOLD_MS);
	}

	if ((gMp.frame % 30) == 0)
		MpHostCnrFlush();

	return ev;
}

int MpCnrHitLineVeto(int victimCarId)
{
	MP_PLAYER* me = MpLocalPlayer();
	MP_PLAYER* rob = (gMpCnrHitMarked >= 0) ? MpGetPlayer(gMpCnrHitMarked) : NULL;
	int id;

	if (me == NULL || rob == NULL || victimCarId < 0)
		return 0;

	if (me->id == gMpCnrHitMarked)
	{
		for (id = 0; id < MP_MAX_PLAYERS; id++)
		{
			MP_PLAYER* p;

			if (id == gMpCnrHitMarked || !MpMarkPresent(id))
				continue;

			p = MpGetPlayer(id);

			if (p != NULL && p->carId == victimCarId)
				return 1;
		}

		return 0;
	}

	return MpMarkPresent(me->id) && rob->carId >= 0 && victimCarId == rob->carId;
}

void MpCnrRadioClear(void)
{
	gJerCopHitLineVeto = NULL;
	gMpCnrHitMarked = -1;

	if (gMpRadioChan >= 0)
		jer_sound_unlock(gMpRadioChan);

	gMpRadioChan = -1;
	gMpRadioPlaying = -1;
	gMpRadioFollow = 0;
	gMpRadioFollowCar = -1;
	gMpRadioNext = 0;
	gMpRadioHeat = 0;
	gMpRadioSaidGo = 0;
	gMpRadioLastSpeaker = -1;
	gMpRadioScorer = -1;
	gMpRadioScoreInit = 0;
}

/* Who scored last: every frame the radio is on, even while a line plays. The
 * first look only records (a round's carried scores are not a fresh hit). */
static void MpCnrRadioScorerTick(int marked)
{
	int id;

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		int sc = MpRoundPlayerScore(id);

		if (gMpRadioScoreInit && id != marked && MpMarkPresent(id) && sc > gMpRadioScore[id])
		{
			gMpRadioScorer = id;
			gMpRadioScorerFrame = gMp.frame;
		}

		gMpRadioScore[id] = sc;
	}

	gMpRadioScoreInit = 1;
}

static void MpCnrRadioSay(int carSlot, int sample, int local)
{
	CAR_DATA* cp = &car_data[carSlot];
	int ch;

	/* the host's phrase number -> this car's own city's sample (or the host's) */
	sample = GetCopPhraseSample(cp->ap.model, sample);

	if (sample < 0)
		return;

	ch = jer_sound_ensure(&gMpRadioChan);

	if (local)
		ch = StartSound(ch, SOUND_BANK_VOICES, sample, -1500, 4096);
	else
	{
		ch = Start3DSoundVolPitch(ch, SOUND_BANK_VOICES, sample,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -1500, 4096);

		if (ch >= 0)
			SetChannelPosition3(ch, (VECTOR*)cp->hd.where.t,
				(LONGVECTOR3*)cp->st.n.linearVelocity, -1500, 4096, 0);
	}

	gMpRadioPlaying = ch;
	gMpRadioPlayFrame = gMp.frame;
}

void MpCnrRadioTick(int on, int marked, int myId)
{
	static unsigned rnd = 0x51u;
	static int lastChatter = -1, saidNoBank, saidBank;
	static unsigned lastLine;
	MP_PLAYER* gp;
	CAR_DATA* gc;
	int id, best = -1, phrase = 0, heading, follow = 0, speaker = -1;
	int inRange[MP_MAX_PLAYERS], nIn = 0, i;
	const long long range2 = (long long)MP_RADIO_RANGE * MP_RADIO_RANGE;

	if (!on)
	{
		gMpRadioSaidGo = 0;
		gMpRadioFollow = 0;
		gMpRadioHeat = 0;
		gMpRadioLastSpeaker = -1;
		gMpRadioScorer = -1;
		gMpRadioScoreInit = 0;
		return;
	}

	MpCnrRadioScorerTick(marked);

	if (!gDoCopSpeech || GetCopPhraseSample(-1, MP_RADIO_MIN_SAMPLES - 1) < 0)
	{
		if (!saidNoBank && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] cops n robbers: no police radio here (cop speech %d, %d voice sample(s))\n",
				gDoCopSpeech, GetSoundBankSampleCount(SOUND_BANK_VOICES));
		saidNoBank = 1;
		return;
	}

	/* ...and say once that it is there: the playtest log is where "the MP map
	 * loaded the cop voices" is proven (LoadLevelSFX loads them for every
	 * take-a-ride mission, M50..M65 and the +440 arenas, gamesnd.c) */
	if (!saidBank && gMpCtx != NULL)
	{
		saidBank = 1;
		gMpCtx->jer_log(gMpCtx, "[mp] cops n robbers: police radio on (%d voice sample(s), mission %d, cop bank %d)\n",
			GetSoundBankSampleCount(SOUND_BANK_VOICES), gCurrentMissionNumber, gCurrentMissionNumber % 4 + 1);
	}

	/* one line at a time: the last one (and its heading clip) first -- but never
	 * longer than any line can be (MP_RADIO_LINE_MAX_FRAMES) */
	if (gMpRadioPlaying >= 0 && jer_sound_channel_busy(gMpRadioPlaying) &&
		gMp.frame - gMpRadioPlayFrame < (unsigned)MP_RADIO_LINE_MAX_FRAMES)
		return;

	gMpRadioPlaying = -1;

	if (gMpRadioFollow != 0)
	{
		if (gMpRadioFollowCar >= 0 && gMpRadioFollowCar < MAX_CARS &&
			car_data[gMpRadioFollowCar].controlType != CONTROL_TYPE_NONE)
		{
			MP_PLAYER* me = MpLocalPlayer();

			MpCnrRadioSay(gMpRadioFollowCar, gMpRadioFollow,
				me != NULL && me->carId == gMpRadioFollowCar);
		}

		gMpRadioFollow = 0;
		return;
	}

	/* ...and never over the engine's own cop speech */
	if (gSpeechQueue.is_playing || gSpeechQueue.count > 0)
		return;

	gp = MpGetPlayer(marked);

	if (gp == NULL || gp->carId < 0 || gp->carId >= MAX_CARS)
		return;

	gc = &car_data[gp->carId];

	/* the cop players in range of the getaway, by id */
	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p;
		long long dx, dz;

		if (id == marked || !MpMarkPresent(id))
			continue;

		p = MpGetPlayer(id);

		if (p == NULL || p->carId < 0 || p->carId >= MAX_CARS ||
			car_data[p->carId].controlType == CONTROL_TYPE_NONE)
			continue;

		dx = (long long)car_data[p->carId].hd.where.t[0] - gc->hd.where.t[0];
		dz = (long long)car_data[p->carId].hd.where.t[2] - gc->hd.where.t[2];

		if (dx * dx + dz * dz <= range2)
			inRange[nIn++] = id;
	}

	if (nIn == 0)
		return;	/* nobody near the chase: no line (the GO line waits for one) */

	rnd = rnd * 1103515245u + 12345u;
	heading = GetCarDirectionOfTravel(gc);

	if (!gMpRadioSaidGo)
	{
		phrase = 10 + (int)((rnd >> 16) & 1);
		gMpRadioSaidGo = 1;
		gMpRadioHeat = MpMarkHeat();
		gMpRadioNext = gMp.frame + MP_RADIO_CHATTER_FRAMES / 2;
	}
	else if (MpMarkHeat() > gMpRadioHeat)
	{
		gMpRadioHeat = MpMarkHeat();

		if (gMp.frame - lastLine >= (unsigned)MP_RADIO_DAMAGE_FRAMES)
			phrase = 6;
	}
	else if (gMp.frame >= gMpRadioNext)
	{
		int pick = (int)((rnd >> 16) % 3);

		if (pick == lastChatter)
			pick = (pick + 1) % 3;

		lastChatter = pick;
		phrase = 15 + pick;
	}

	if (phrase == 0)
		return;

	/* the damage line: the cop who scored it, when they are near */
	if (phrase == 6 && gMpRadioScorer >= 0 &&
		gMp.frame - gMpRadioScorerFrame <= (unsigned)MP_RADIO_SCORER_FRAMES)
	{
		for (i = 0; i < nIn; i++)
			if (inRange[i] == gMpRadioScorer)
				speaker = gMpRadioScorer;
	}

	/* else the next cop in range after the last speaker (round-robin by id) */
	if (speaker < 0)
	{
		speaker = inRange[0];

		for (i = 0; i < nIn; i++)
			if (inRange[i] > gMpRadioLastSpeaker)
			{
				speaker = inRange[i];
				break;
			}
	}

	best = MpGetPlayer(speaker)->carId;
	gMpRadioLastSpeaker = speaker;

	/* the speaking car's lines decide: the stock D2 sets follow 10 (banks 2/3)
	 * and 15 (banks 1/4) with a heading clip; a baked D1/bank set always does */
	if (CopPhraseWantsHeading(car_data[best].ap.model, phrase))
		follow = heading + 1;

	{
		MP_PLAYER* me = MpLocalPlayer();

		MpCnrRadioSay(best, phrase, me != NULL && me->id != marked && me->carId == best);
	}

	if (MpDebugOn() && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] cops n robbers: radio line %d from player %d (%d cop(s) within %d)\n",
			phrase, speaker, nIn, MP_RADIO_RANGE);

	gMpRadioFollow = follow;
	gMpRadioFollowCar = best;
	lastLine = gMp.frame;

	if (phrase != 6)
		gMpRadioNext = gMp.frame + MP_RADIO_CHATTER_FRAMES + (int)((rnd >> 8) % MP_RADIO_CHATTER_JITTER);

	(void)myId;
}

/* Cops n Robbers, host driving the getaway: our own car's damage straight into
 * the referee (a client's rides its carstate rows, MpHostCarRowRound). */
void MpCnrDamageTick(int phase, int marked, int myId)
{
	static int lastSent = -1;
	MP_PLAYER* me = MpLocalPlayer();
	int dmg;

	if (!MpIsHost() || MpModeTimerKind(gMp.gamemode) != MP_MODE_TIMER_GETAWAY || phase != MP_MARK_PHASE_LIVE ||
		myId < 0 || marked != myId || me == NULL || me->carId < 0 || me->carId >= MAX_CARS ||
		player[0].playerType == PLAYER_TYPE_PEDESTRIAN)
	{
		lastSent = -1;
		return;
	}

	dmg = car_data[me->carId].totalDamage;

	if (dmg != lastSent)
		MpMarkHostDamage(myId, dmg, MaxPlayerDamage[0], "own car");

	lastSent = dmg;
}
