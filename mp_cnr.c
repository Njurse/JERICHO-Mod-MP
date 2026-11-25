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
#define MP_RADIO_RANGE		MP_MARK_CNR_ESCAPE_DIST	/* every cop the getaway has not escaped (was 16000, inside the 18000 escape: cops 16000..18000 out were silent) */
#define MP_RADIO_QUEUE_MAX_FRAMES	(MP_RADIO_LINE_MAX_FRAMES * 3)	/* the engine speech queue may hold the radio this long, no longer */
#define MP_SIREN_MAX		2	/* the engine's own limit for cop sirens (DoDopplerSFX) */
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

static unsigned gMpRadioQueueSince;		/* first frame the engine speech queue was seen busy, 0 = idle */

/* Cops n Robbers sirens: the cop players' own loops, local on every machine */
typedef struct MP_CNR_SIREN
{
	int chan;	/* our locked voice, -1 = none */
	int car;	/* car_data slot it sounds for, -1 = none */
	int sample;	/* packed CarHasSiren value it was started with */
} MP_CNR_SIREN;

static MP_CNR_SIREN gMpCnrSiren[MP_SIREN_MAX] = { { -1, -1, 0 }, { -1, -1, 0 } };
static int gMpCnrSirenWasLive;
static int gMpCnrSirenCountdownCleared;

extern int gDoCopSpeech;	/* gamesnd.c: 0 on the gang missions */

int MpPlayerSirenOn(int playerId);
void MpHostCnrFlush(void);
static void MpCnrRadioScorerTick(int marked);
static void MpCnrRadioSay(int carSlot, int sample, int local);
static void MpCnrSirenTick(int on, int marked);

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
	gMpRadioQueueSince = 0;
	gMpCnrSirenWasLive = 0;
	gMpCnrSirenCountdownCleared = 0;
	MpCnrSirenTick(0, -1);
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

/* COPS N ROBBERS SIRENS. A player controls their own siren/lightbar state via
 * the horn toggle (handling.c:1289: pad & 8 flips player[0].horn.on between 0
 * and 8), which the engine already plays on channel player*3+2 (gamesnd.c
 * ControlSound) and draws via AddCopCarLight (main.c). That state syncs out on
 * MP_CARSTATE_SIREN (mp_sync.c). At CnR GO (LIVE start), each cop player turns
 * their own siren on once; after that they can toggle it off/on as usual, and
 * remotes hear up to MP_SIREN_MAX nearest remote cars whose synced state is on.
 * DoDopplerSFX only handles AI pursuers, so remote CONTROL_TYPE_PLAYER cars
 * need this tracked-voice loop here. */
static void MpCnrSirenStop(MP_CNR_SIREN* s)
{
	if (s->chan >= 0)
	{
		StopChannel(s->chan);
		jer_sound_unlock(s->chan);
	}

	s->chan = -1;
	s->car = -1;
	s->sample = 0;
}

static void MpCnrSirenTick(int on, int marked)
{
	static int saidOn;
	MP_PLAYER* me = MpLocalPlayer();
	int want[MP_SIREN_MAX], wantId[MP_SIREN_MAX], wantSample[MP_SIREN_MAX], nWant = 0;
	long long wantD[MP_SIREN_MAX];
	int lx = 0, lz = 0, id, k, j;

	(void)marked;

	if (on)
	{
		if (me != NULL && me->carId >= 0 && me->carId < MAX_CARS)
		{
			lx = car_data[me->carId].hd.where.t[0];
			lz = car_data[me->carId].hd.where.t[2];
		}
		else
		{
			lx = player[0].cameraPos.vx;
			lz = player[0].cameraPos.vz;
		}
	}

	for (id = 0; on && id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p;
		CAR_DATA* cp;
		int sample;
		long long dx, dz, d;

		/* our own car's siren plays on the engine's player*3+2 channel */
		if (me != NULL && id == me->id)
			continue;

		if (!MpPlayerSirenOn(id))
			continue;

		p = MpGetPlayer(id);

		if (p == NULL || p->carId < 0 || p->carId >= MAX_CARS)
			continue;

		cp = &car_data[p->carId];

		if (cp->controlType == CONTROL_TYPE_NONE)
			continue;

		sample = CarHasSiren(cp->ap.model);

		if (sample == 0 || (sample & 0xff00) == 0)
			continue;	/* no siren sample (a civilian body, bank 0) */

		dx = (long long)cp->hd.where.t[0] - lx;
		dz = (long long)cp->hd.where.t[2] - lz;
		d = dx * dx + dz * dz;

		/* insert by distance, keep the nearest MP_SIREN_MAX */
		for (k = 0; k < nWant && wantD[k] <= d; k++)
			;

		if (k >= MP_SIREN_MAX)
			continue;

		for (j = (nWant < MP_SIREN_MAX ? nWant : MP_SIREN_MAX - 1); j > k; j--)
		{
			want[j] = want[j - 1];
			wantId[j] = wantId[j - 1];
			wantSample[j] = wantSample[j - 1];
			wantD[j] = wantD[j - 1];
		}

		want[k] = p->carId;
		wantId[k] = id;
		wantSample[k] = sample;
		wantD[k] = d;

		if (nWant < MP_SIREN_MAX)
			nWant++;
	}

	/* stop what is no longer wanted (or whose bank sample moved) */
	for (k = 0; k < MP_SIREN_MAX; k++)
	{
		MP_CNR_SIREN* s = &gMpCnrSiren[k];
		int keep = 0;

		if (s->car < 0)
			continue;

		for (j = 0; j < nWant; j++)
			if (want[j] == s->car && wantSample[j] == s->sample)
				keep = 1;

		if (!keep || !jer_sound_channel_busy(s->chan))
			MpCnrSirenStop(s);
	}

	/* start the new ones, then move every live loop with its car */
	for (j = 0; j < nWant; j++)
	{
		MP_CNR_SIREN* s = NULL;
		CAR_DATA* cp = &car_data[want[j]];

		for (k = 0; k < MP_SIREN_MAX; k++)
			if (gMpCnrSiren[k].car == want[j])
				s = &gMpCnrSiren[k];

		if (s == NULL)
		{
			int ch, lockCh;

			for (k = 0; k < MP_SIREN_MAX && s == NULL; k++)
				if (gMpCnrSiren[k].car < 0)
					s = &gMpCnrSiren[k];

			if (s == NULL)
				continue;

			/* a LOCKED voice or nothing: an unlocked loop could be taken over by
			 * the engine, and stopping it later would cut someone else's sound */
			lockCh = -1;

			if (jer_sound_ensure(&lockCh) < 0)
				continue;

			ch = Start3DTrackingSound(lockCh, (wantSample[j] & 0xff00) >> 8, wantSample[j] & 0xff,
				(VECTOR*)cp->hd.where.t, (LONGVECTOR3*)cp->st.n.linearVelocity);

			if (ch < 0)
			{
				jer_sound_unlock(lockCh);
				continue;
			}

			s->chan = ch;
			s->car = want[j];
			s->sample = wantSample[j];

			if (gMpCtx != NULL && (!saidOn || MpDebugOn()))
				gMpCtx->jer_log(gMpCtx, "[mp] cops n robbers: siren on for player %d car %d (slot %d, bank %d sample %d, voice %d)\n",
					wantId[j], want[j], cp->ap.model, (wantSample[j] & 0xff00) >> 8, wantSample[j] & 0xff, ch);

			saidOn = 1;
		}

		SetChannelPosition3(s->chan, (VECTOR*)cp->hd.where.t, (LONGVECTOR3*)cp->st.n.linearVelocity,
			-3000, 4096 + (int)(s - gMpCnrSiren) * 4, 0);
	}
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

	if (MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_GETAWAY &&
	    MpMarkPhase() == MP_MARK_PHASE_COUNTDOWN)
	{
		if (!gMpCnrSirenCountdownCleared)
		{
			player[0].horn.on = 0;
			gMpCnrSirenCountdownCleared = 1;
		}
	}
	else
		gMpCnrSirenCountdownCleared = 0;

	/* CnR GO (transition into LIVE): cop players start with sirens on locally
	 * so MP_CARSTATE_SIREN syncs out; the getaway is never auto-on. */
	if (on && !gMpCnrSirenWasLive)
	{
		MP_PLAYER* me = MpLocalPlayer();

		if (me != NULL && me->id != marked && MpMarkInRound(me->id) &&
		    player[0].playerType == PLAYER_TYPE_CAR &&
		    me->carId >= 0 && me->carId < MAX_CARS &&
		    CarHasSiren(car_data[me->carId].ap.model) != 0)
		{
			player[0].horn.on = 8;	/* matches handling.c:1290 toggle bit */

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] cops n robbers: auto-on siren for cop player %d (car %d, slot %d)\n",
					me->id, me->carId, car_data[me->carId].ap.model);
		}
	}
	else if (!on && gMpCnrSirenWasLive &&
		 MpModeTimerKind(gMp.gamemode) == MP_MODE_TIMER_GETAWAY)
	{
		player[0].horn.on = 0;
	}

	gMpCnrSirenWasLive = on;
	MpCnrSirenTick(on, marked);

	if (!on)
	{
		gMpRadioQueueSince = 0;
		gMpRadioSaidGo = 0;
		gMpRadioFollow = 0;
		gMpRadioHeat = 0;
		gMpRadioLastSpeaker = -1;
		gMpRadioScorer = -1;
		gMpRadioScoreInit = 0;
		return;
	}

	MpCnrRadioScorerTick(marked);

	/* Only the gang missions turn cop speech off. The host city's lines are NOT
	 * required any more: a cop whose own city's lines are resident can speak
	 * without them (the per-speaker check below). */
	if (!gDoCopSpeech)
	{
		if (!saidNoBank && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] cops n robbers: no police radio here (cop speech %d, %d voice sample(s))\n",
				gDoCopSpeech, GetSoundBankSampleCount(SOUND_BANK_VOICES));
		saidNoBank = 1;
		return;
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

	/* ...and never over the engine's own cop speech -- but only for so long. The
	 * queue clears is_playing only when SpuGetKeyStatus says its voice stopped
	 * (gamesnd.c ControlSpeech); a voice that never reports off kept the radio
	 * quiet for the rest of the level. Past MP_RADIO_QUEUE_MAX_FRAMES of one
	 * unbroken busy stretch (three of the longest lines) the radio goes on. */
	if (gSpeechQueue.is_playing || gSpeechQueue.count > 0)
	{
		if (gMpRadioQueueSince == 0)
			gMpRadioQueueSince = gMp.frame ? gMp.frame : 1;

		if (gMp.frame - gMpRadioQueueSince < (unsigned)MP_RADIO_QUEUE_MAX_FRAMES)
			return;

		if (gMp.frame - gMpRadioQueueSince == (unsigned)MP_RADIO_QUEUE_MAX_FRAMES && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] cops n robbers: engine speech queue busy %d frame(s) (playing %d, %d queued) - radio no longer waits on it\n",
				MP_RADIO_QUEUE_MAX_FRAMES, gSpeechQueue.is_playing, gSpeechQueue.count);
	}
	else
		gMpRadioQueueSince = 0;

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

		if (dx * dx + dz * dz > range2)
			continue;

		/* a cop speaks only when ITS lines (or, failing them, the host's) are
		 * resident: GetCopPhraseSample, the same answer MpCnrRadioSay plays */
		if (GetCopPhraseSample(car_data[p->carId].ap.model, MP_RADIO_MIN_SAMPLES - 1) < 0)
			continue;

		inRange[nIn++] = id;
	}

	if (nIn == 0)
		return;	/* nobody near the chase with a voice: no line (the GO line waits for one) */

	/* ...and say once that it is there: the playtest log is where "the MP map
	 * loaded the cop voices" is proven */
	if (!saidBank && gMpCtx != NULL)
	{
		saidBank = 1;
		gMpCtx->jer_log(gMpCtx, "[mp] cops n robbers: police radio on (%d voice sample(s), mission %d, cop bank %d, host lines %s)\n",
			GetSoundBankSampleCount(SOUND_BANK_VOICES), gCurrentMissionNumber, gCurrentMissionNumber % 4 + 1,
			GetCopPhraseSample(-1, MP_RADIO_MIN_SAMPLES - 1) >= 0 ? "resident" : "missing");
	}

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
		gMpCtx->jer_log(gMpCtx, "[mp] cops n robbers: radio line %d from player %d slot %d -> voices sample %d (%d cop(s) within %d)\n",
			phrase, speaker, car_data[best].ap.model, GetCopPhraseSample(car_data[best].ap.model, phrase), nIn, MP_RADIO_RANGE);

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
