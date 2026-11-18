/*
 * mp_sync.c -- split out of mp_session.c
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"

#define MP_INTERP_RING		32	/* samples kept per remote player (about 1 s) */

typedef struct MP_SELF_POSE
{
	unsigned frame;
	int x, y, z;
} MP_SELF_POSE;

typedef struct MP_CAR_SNAP
{
	unsigned frame;		/* the owner's carstate frame */
	unsigned recvFrame;	/* local sim frame it arrived on (diagnostics) */
	int x, y, z;
	int heading;
	short orient[4];
	int vel[3];
	int angVel[3];
} MP_CAR_SNAP;

typedef struct MP_CAR_PLAY
{
	MP_CAR_SNAP snap[MP_INTERP_RING];
	int count;		/* valid samples; newest is at head */
	int head;
	int primed;
	int playFrame;		/* owner frame currently being displayed */
	int lastFrame;		/* newest received owner frame */
	int mode;		/* 0 = lerp, 1 = hold, 2 = extrapolate (diagnostics) */
	int playX, playY, playZ;	/* the pose actually written this frame */
} MP_CAR_PLAY;

#define MP_CRUSH_HALF_DEFAULT	2	/* 1.0x */
#define MP_FIXEDH		4096	/* the velocity fix-point scale (1 world unit/frame << 12) */
#define MP_HIT_ARM_FRAMES	120	/* let the spawn/drop settle before contacts count */
#define MP_HIT_COOLDOWN_FRAMES	15
#define MP_HIT_IMPULSE_MAX	(MP_FIXEDH * MP_HIT_MAX_PUSH)
#define MP_HIT_MAX_AGE_FRAMES	20
#define MP_HIT_MAX_PUSH		20	/* WHOLE units/frame cap */
#define MP_HIT_MIN_CLOSING	4	/* WHOLE units/frame; don't fire on mere adjacency */
#define MP_HIT_PROBE_QUIET	90	/* ... and the probe stands down this long after one */
#define MP_HIT_PUSH		60	/* per-cent of the closing speed given up */
#define MP_HIT_RADIUS		900	/* world units; car body scale (the probe FALLBACK only) */
#define MP_HIT_RX_RADIUS	MP_HIT_RADIUS	/* the same body scale, measured at ARRIVAL */
#define MP_HIT_SELF_FRAMES	4	/* our own engine reported this contact this recently */
#define MP_HIT_SUMMARY_FRAMES	300
#define MP_INTERP_DEBUG_FRAMES	60	/* MP_DEBUG state line every 2 s */
#define MP_INTERP_DELAY		3	/* playout delay, in 30 Hz frames (100 ms) */
#define MP_INTERP_EXTRAP_MAX	3	/* never extrapolate further than this */
#define MP_INTERP_RESYNC	8	/* cursor this far behind: jump to newest - DELAY */
#define MP_INTERP_SNAP_DIST	6000	/* one-sample jump: a teleport, not motion */
#define MP_SELF_HISTORY	64
#define MP_TRAFFIC_CLAIM_DEADBAND	2000	/* world units: closer than this, the lower player id wins */
#define MP_TRAFFIC_SLOT_COUNT	JER_SLOT_TRAFFIC_COUNT			/* 12 traffic slots, fixed */
#define MP_TRAFFIC_SLOT_FIRST	JER_SLOT_TRAFFIC_FIRST			/* 20: above the player and managed pools */
#define MP_TRAFFIC_STALE_FRAMES	60	/* ~2 s at 30 fps */

/* Slots that hold a PEER's traffic mirror. The band already keeps the two
 * spawners in disjoint slots, but a mirror must ALSO be remembered as not-ours so
 * teardown removes exactly what we built and nothing of the engine's. */
unsigned char gMpPeerTraffic[MAX_CARS];	/* 1 = we built a peer's car here */
uint8_t gMpPedSeq[MP_MAX_PLAYERS];	/* last on-foot sequence applied, per player row */
static unsigned gMpPeerTrafficSeen[MAX_CARS];	/* frame we last heard about a mirror */

static unsigned gMpPeerTrafficSiren[MAX_CARS];	/* frame we last heard it was on duty (MP_TRAFFIC_SIREN) */

static int gMpMirrorSirenChannel = -1;		/* our locked voice for a peer's siren loop */

static unsigned char gMpTrafficSent[MAX_CARS];	/* slots WE published last frame */

static unsigned gMpTrafficSelfContact[MAX_CARS];/* frame OUR engine resolved a contact with our traffic car */

static unsigned gMpTrafficHitFrame[MAX_CARS];	/* rate limit for reporting a contact with a peer's traffic */

/* Last reported palette value LOGGED per player, so the continuous carstate
 * stream logs a correction once per reported value instead of every packet.
 * File scope (not a function static) so the session can clear it: a leftover
 * "already logged" mark would swallow the first report after a reconnect. */
static signed char gPaletteLogged[MP_MAX_PLAYERS];

/* Frame at which car-to-car contacts start counting, for THIS match. Session
 * state rather than a function-local: MpSessionReset zeroes gMp.frame between
 * matches, and a function-local would survive holding the PREVIOUS match's
 * value, silently disabling collisions until the frame counter climbed back up
 * to it -- minutes into a second match if the first one ran long. */
static unsigned long gMpHitArmFrame;

/* Frame at which OUR engine last reported a contact with each peer's car. The
 * RECEIVING half of a hand-off consults it: when our own engine has just resolved
 * the same contact its rigid-body response is already in our car's velocity, and
 * the module's push on top of that would land one collision twice. Session state,
 * for the same reason as the arm frame above. */
static unsigned long gMpSelfContactFrame[MP_MAX_PLAYERS];

/* Hit-path instrumentation, session state like the counters above. These answer
 * "how often does the hand-off fire, and how often does a push arrive when the
 * two cars are already far apart on the receiver's screen" -- the measurement the
 * latency work is judged by. Increments are unconditional and nearly free (a few
 * per hit); only the SUMMARY line is MP_DEBUG-gated.
 *
 * MpSessionReset runs before these would otherwise be declared if they lived with
 * the collision code, so they belong here rather than beside MpHandleHit. */
static unsigned long gMpHitSent;		/* pushes we reported */

static unsigned long gMpHitRx;			/* pushes addressed to us */

static unsigned long gMpHitApplied;		/* ... applied (our engine did not have it) */

static unsigned long gMpHitKept;		/* ... dropped as the engine's duplicate */

static unsigned long gMpHitSeparated;		/* ... arrived with the cars > MP_HIT_RADIUS apart */

static unsigned long gMpHitRejected;		/* ... dropped: stale, unvalidatable or ghost */

static MP_CAR_PLAY gMpPlay[MP_MAX_PLAYERS];

static unsigned char gMpBandReserved[MAX_CARS];	/* slots WE turned on, so we clear only ours */

/* THE COP SLOT HOLD. Civilians and cops share this machine's band, and in
 * single player the engine never runs the pool dry (19 free slots against a
 * civilian cap of 13), so a cop request always finds a slot. A band is at most
 * 12 slots and the cap does not shrink with it, so traffic fills every slot and
 * a cop has to wait for a civilian to drive out of range -- which, for players
 * staying in one area, is a long time. While the session has cops on and fewer
 * than maxCopCars are out, the LAST free slot of the band is held back from the
 * civilian spawner (reservedSlots, like the band), and released on the frame a
 * cop is requested so the cop takes it. -1 = holding nothing. */
static int gMpCopHold = -1;

static int gMpBandFirst = -1;			/* this machine's band, inclusive; -1 = none */

static int gMpBandLast = -1;

static int gMpClaimCulled;		/* cars pinged out by the claim this reporting window */

static MP_SELF_POSE gMpSelfPose[MP_SELF_HISTORY];

int sMpWantCity = -2;	/* -2 = not known yet, -1 = the host city */

int sMpWantModel = -1;

int sMpStandIn;

static int MpRowOwnerOk(int from, int playerId);
static int MpHitArmed(void);
static int MpHitPoseTolerance(const CAR_DATA* target, const MP_PLAYER* src);
static long MpContactNormal(CAR_DATA* mine, CAR_DATA* other, long* px, long* py, long* pz);
static int MpContactPush(MP_PLAYER* me, MP_PLAYER* pl, int localHalf, const char* why);
static void MpCopHoldRelease(void);
static int MpTrafficMachineCount(void);
static void MpTrafficBandFor(int rank, int nMachines, int* first, int* last);
void MpTrafficBandClear(void);
int MpTrafficBand(int* first, int* last);
static int MpSlotInOurBand(int slot);
int MpTrafficSlotOwnerRank(int slot);
static void MpTrafficContactPush(MP_PLAYER* me, int slot);
long long MpDistSq64(int dx, int dz);
static int MpTrafficClaimOwner(int x, int z, long long* outSq);
static int MpPeerMirrorNear(int x, int z);
static int MpFrameAge(unsigned newer, unsigned older);
void MpPlayClear(int id);
static int MpSelfPoseAt(unsigned frame, int* x, int* y, int* z);
static void MpPlayPush(int id, unsigned frame, const MP_CARSTATE_ENTRY* e);
static void MpSnapUse(const MP_CAR_SNAP* s, int* x, int* y, int* z, int* heading, short orient[4], int vel[3], int angVel[3]);
static int MpLerpAngle(int a, int b, int num, int den);
static void MpPlayWriteBody(CAR_DATA* cp, int x, int y, int z, int heading, const short orient[4], const int vel[3], const int angVel[3]);
void MpSendColors(int whole);
static int MpCreateTrafficMirror(int slot, int city, int model, int palette);
static void MpRemoveTrafficMirror(int slot);
static int MpTrafficMirrorMatches(int slot, int city, int model);
static void MpApplyTrafficPose(CAR_DATA* cp, const MP_TRAFFIC_ENTRY* e);
static void MpApplyTrafficDamage(CAR_DATA* cp, const MP_TRAFFIC_ENTRY* e);

/* Called from MpSessionReset: this module's state is per session, so
 * the next match starts from the same values the module loaded with. */
void MpSyncReset(void)
{
	gMpHitSent = gMpHitRx = gMpHitApplied = gMpHitKept = gMpHitSeparated = gMpHitRejected = 0;
	gMpHitArmFrame = 0;	/* collisions arm afresh in every match */
	gMpHitSent = gMpHitRx = gMpHitApplied = gMpHitKept = gMpHitSeparated = gMpHitRejected = 0;
	gMpHitSent = gMpHitRx = gMpHitApplied = gMpHitKept = gMpHitSeparated = gMpHitRejected = 0;
	gMpHitSent = gMpHitRx = gMpHitApplied = gMpHitKept = gMpHitSeparated = gMpHitRejected = 0;
	gMpHitSent = gMpHitRx = gMpHitApplied = gMpHitKept = gMpHitSeparated = gMpHitRejected = 0;
	gMpHitSent = gMpHitRx = gMpHitApplied = gMpHitKept = gMpHitSeparated = gMpHitRejected = 0;
	memset(gMpSelfContactFrame, 0, sizeof(gMpSelfContactFrame));
	memset(gPaletteLogged, 0, sizeof(gPaletteLogged));
}

/* The pad the local player is holding this frame, in the engine's own mapped
 * bit space (what ProcessCarPad would read for a local car). */
int MpLocalPad(void)
{
	/* The pad that actually drove OUR car last frame, whatever produced it. Reading
	 * Pads[] alone missed the test bot, which injects its pad in the NET_INPUT hook
	 * rather than into the pad state, so a bot run replicated 0 and the peer's copy
	 * of our car only ever moved when a resync snapped it there. */
	if (gMpLocalAppliedPad >= 0)
		return gMpLocalAppliedPad;

	{
		int id = gMp.localPlayerId;
		int padId = 0;

		if (id >= 0 && id < MP_MAX_PLAYERS)
		{
			MP_PLAYER* me = &gMp.players[id];

			if (me->carId >= 0 && me->carId < MAX_CARS && car_data[me->carId].ai.padid != NULL)
			{
				int p = *car_data[me->carId].ai.padid;

				if (p >= 0 && p < 2)
					padId = p;
			}
		}

		return (int)Pads[padId].mapped;
	}
}

void MpSendInput(int pad)
{
	unsigned char buf[sizeof(MP_INPUT) + MP_MAX_PLAYERS * sizeof(MP_PLAYER_INPUT)];
	MP_INPUT h;
	int i, k = 0, len;

	if (!gMp.running || gMp.localPlayerId < 0 || gMp.localPlayerId >= MP_MAX_PLAYERS)
		return;

	/* our own row is part of the set whichever side we are */
	gMp.padForPlayer[gMp.localPlayerId] = pad;

	memset(&h, 0, sizeof(h));

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER_INPUT r;

		if (!gMp.players[i].active)
			continue;

		/* the host only forwards input it has actually seen */
		if (i != gMp.localPlayerId && !gMp.inputHave[i])
			continue;

		memset(&r, 0, sizeof(r));
		r.playerId = (uint8_t)i;
		r.pad = (uint16_t)gMp.padForPlayer[i];

		memcpy(buf + sizeof(MP_INPUT) + (size_t)k * sizeof(r), &r, sizeof(r));
		k++;
	}

	h.count = (uint8_t)k;
	memcpy(buf, &h, sizeof(h));

	len = (int)(sizeof(MP_INPUT) + (size_t)k * sizeof(MP_PLAYER_INPUT));

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_INPUT, 0, buf, len);
	else
		MpSendToHost(MP_TAG_INPUT, 0, buf, len);
}

/* Sender binding for a per-player row.
 *
 * Every state message names players by id, and an id on the wire is just a
 * number. Without this check any peer could speak for any other player: send
 * a row naming someone else and the hub would store it, then republish it as
 * that player's own word.
 *
 * The rule differs by side because the TOPOLOGY does. The host is the hub: it
 * receives one peer's own rows (so a row must name that peer) and rebroadcasts
 * a merged set to everyone. A client only ever has ONE inbound connection --
 * the host -- and its merged set legitimately carries every player, so there
 * is nobody to bind against: `from` is the host, not the row's owner.
 *
 * `from` is -1 before a HELLO, which is exactly the window in which a stranger
 * should not be able to name anybody at all. */
static int MpRowOwnerOk(int from, int playerId)
{
	if (playerId < 0 || playerId >= MP_MAX_PLAYERS)
		return 0;

	if (!MpIsHost())
		return 1;	/* the host already validated it, and it is our only peer */

	return from >= 0 && playerId == from;
}

void MpHandleInput(int connIndex, const unsigned char* p, int len)
{
	MP_INPUT h;
	int n, i, from;

	if (len < (int)sizeof(MP_INPUT))
		return;

	memcpy(&h, p, sizeof(h));

	from = MpConnPlayerId(connIndex);

	n = h.count;
	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	if (len < (int)(sizeof(MP_INPUT) + (size_t)n * sizeof(MP_PLAYER_INPUT)))
		return;

	for (i = 0; i < n; i++)
	{
		MP_PLAYER_INPUT r;

		memcpy(&r, p + sizeof(MP_INPUT) + (size_t)i * sizeof(MP_PLAYER_INPUT), sizeof(r));

		if (r.playerId >= MP_MAX_PLAYERS)
			continue;

		/* our own row comes back to us in the host's set: our local pad is the
		 * truth for our own car, so leave it alone */
		if (r.playerId == gMp.localPlayerId)
			continue;

		/* ...and a peer may only speak for ITSELF. Dropped rather than clamped:
		 * a row that names someone else is not a mistake, it is the spoof. */
		if (!MpRowOwnerOk(from, (int)r.playerId))
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] input: dropped a row from player %d claiming player %d\n",
					from, (int)r.playerId);

			continue;
		}

		gMp.padForPlayer[r.playerId] = (int)r.pad;
		gMp.inputHave[r.playerId] = 1;
	}
}

/* Contacts arm afresh in every match (see gMpHitArmFrame at the top of this file
 * for why the arm frame is session state and not a function local). */
static int MpHitArmed(void)
{
	if (gMpHitArmFrame == 0)
		gMpHitArmFrame = gMp.frame + MP_HIT_ARM_FRAMES;

	return (gMp.frame >= gMpHitArmFrame);
}

/* How far the sender's view of us may be from where we recorded ourselves at the
 * same frame. When the sample arrived it is EXACT (the sender displays a received
 * sample), so this is mostly room for the interpolation error of a lost sample;
 * half a car body per side makes it scale with the vehicles. */
static int MpHitPoseTolerance(const CAR_DATA* target, const MP_PLAYER* src)
{
	int tol = 300;

	if (target != NULL && target->ap.carCos != NULL)
		tol += target->ap.carCos->colBox.vz / 8;

	if (src != NULL && src->carId >= 0 && src->carId < MAX_CARS &&
	    car_data[src->carId].ap.carCos != NULL)
		tol += car_data[src->carId].ap.carCos->colBox.vz / 8;

	return tol;
}

/* The contact normal US -> THEM in the velocity fix-point (MP_FIXEDH = 1.0), and
 * the closing speed in WHOLE units per frame.
 *
 * THE UNITS ARE THE WHOLE POINT. A velocity is fixed-point, so the dot product of
 * a velocity difference with a fixed-point normal carries MP_FIXEDH TWICE; it is
 * reduced here, once, rather than left 4096x too large. The thresholds and the cap
 * above are documented in whole units/frame and are now actually in them -- the
 * previous code compared and scaled FIXEDH values against those numbers, which made
 * every hand-off 1/4096 of the intended push: a contact that moved nothing, which
 * is what "the cars drive through each other" was. */
static long MpContactNormal(CAR_DATA* mine, CAR_DATA* other, long* px, long* py, long* pz)
{
	/* 64-bit: a world span squared overflows 32, and two cars a region apart is
	 * an ordinary thing to ask about. */
	long long dx = (long long)other->hd.where.t[0] - (long long)mine->hd.where.t[0];
	long long dy = (long long)other->hd.where.t[1] - (long long)mine->hd.where.t[1];
	long long dz = (long long)other->hd.where.t[2] - (long long)mine->hd.where.t[2];
	long long d2 = dx * dx + dy * dy + dz * dz;
	long d;

	*px = MP_FIXEDH;
	*py = 0;
	*pz = 0;

	if (d2 <= 0)
		return 0;

	d = (long)sqrt((double)d2);

	if (d == 0)
		return 0;

	*px = (long)(dx * MP_FIXEDH / d);
	*py = (long)(dy * MP_FIXEDH / d);
	*pz = (long)(dz * MP_FIXEDH / d);

	return (long)(((long long)(mine->st.n.linearVelocity[0] - other->st.n.linearVelocity[0]) * *px
		+ (long long)(mine->st.n.linearVelocity[1] - other->st.n.linearVelocity[1]) * *py
		+ (long long)(mine->st.n.linearVelocity[2] - other->st.n.linearVelocity[2]) * *pz)
		/ MP_FIXEDH / MP_FIXEDH);
}

/* Report a contact between OUR car and `pl`'s: give up our own share of the
 * closing speed here, and ask THEIR owner to give up theirs. Returns 1 when a
 * contact was reported (the caller's cooldown mark is set here, so both triggers
 * share one rate limit).
 *
 * `localHalf` decides whether OUR car is pushed as well. The engine-contact path
 * passes 0: our own engine has just resolved this contact, so its response is
 * already ours and a second impulse would double the hit. The probe fallback
 * passes 1, because by definition the engine did not see that overlap. */
static int MpContactPush(MP_PLAYER* me, MP_PLAYER* pl, int localHalf, const char* why)
{
	CAR_DATA* mine = &car_data[me->carId];
	CAR_DATA* other = &car_data[pl->carId];
	long px, py, pz, closing, push, ix, iy, iz;
	MP_HIT h;

	closing = MpContactNormal(mine, other, &px, &py, &pz);

	if (closing < MP_HIT_MIN_CLOSING)
		return 0;

	push = closing * MP_HIT_PUSH / 100;

	if (push > MP_HIT_MAX_PUSH)
		push = MP_HIT_MAX_PUSH;

	/* The impulse is a VELOCITY delta, and px is ALREADY normal * MP_FIXEDH, so a
	 * whole-unit push along it is one multiply: (n << 12) * push == n * push << 12.
	 * Dividing here as well (what the old code did) is the 4096x that made the
	 * hand-off inert. */
	ix = px * push;
	iy = py * push;
	iz = pz * push;

	if (localHalf)
	{
		mine->st.n.linearVelocity[0] -= (int)ix;
		mine->st.n.linearVelocity[1] -= (int)iy;
		mine->st.n.linearVelocity[2] -= (int)iz;
	}

	memset(&h, 0, sizeof(h));
	h.targetId = (uint8_t)pl->id;
	h.impulse[0] = (int)ix;
	h.impulse[1] = (int)iy;
	h.impulse[2] = (int)iz;

	/* Name WHERE and WHEN in the target's own timeline. The playout state is
	 * what we actually collided with; if this machine has not played the target
	 * yet, leave 0 and the receiver will refuse an unvalidatable push. */
	{
		MP_CAR_PLAY* tp = &gMpPlay[pl->id];

		if (tp->primed && tp->playFrame != 0)
		{
			h.targetFrame = (uint32_t)tp->playFrame;
			h.tgtPos[0] = tp->playX;
			h.tgtPos[1] = tp->playY;
			h.tgtPos[2] = tp->playZ;
		}
		else
		{
			h.tgtPos[0] = other->hd.where.t[0];
			h.tgtPos[1] = other->hd.where.t[1];
			h.tgtPos[2] = other->hd.where.t[2];
		}
	}

	pl->lastHitFrame = gMp.frame;

	/* a timer mode: the host's own contact is a tag report, taken as seen; its
	 * push (whole units/frame) is a Cops n Robbers hit's strength */
	MpMarkHostReport(me->id, pl->id, 0, (int)push);

	if (MpIsHost())
	{
		int ci = MpConnFindByPlayer(pl->id);

		if (ci >= 0)
			MpSendConn(ci, MP_TAG_HIT, 0, &h, sizeof(h));
	}
	else
		MpSendToHost(MP_TAG_HIT, 0, &h, sizeof(h));

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] hit: we bumped player %d (%s: closing %ld, giving up %ld units/frame)\n",
			pl->id, why, closing, push);

	gMpHitSent++;

	return 1;
}

/* JER_EVENT_COLLISION -- the engine has just resolved a car-to-car contact
 * (handling.c fires it for the pair it resolved, BEFORE either car's impulse is
 * applied, so the velocities read here are still the pre-impact ones: the true
 * closing speed).
 *
 * This is the primary trigger for the hand-off, and it is the fix for "the
 * collision is not always registered on the remote player": probing for proximity
 * on a later frame asked the question after our own engine had already absorbed
 * the closing velocity, so it read ~0 and nothing was sent for exactly the hits
 * that mattered. */
int MpOnCarContact(void* ud, void* args)
{
	JER_ARGS_COLLISION* a = (JER_ARGS_COLLISION*)args;
	MP_PLAYER* me = MpLocalPlayer();
	MP_PLAYER* them;
	int mineId, otherId;

	(void)ud;

	/* car1 == NULL is a car-vs-WORLD hit: not ours to relay. */
	if (a == NULL || a->car0 == NULL || a->car1 == NULL)
		return JER_RESULT_CONTINUE;

	if (me == NULL || me->carId < 0 || !gMp.running)
		return JER_RESULT_CONTINUE;

	mineId = CAR_INDEX((CAR_DATA*)a->car0);
	otherId = CAR_INDEX((CAR_DATA*)a->car1);

	/* TRAFFIC contacts. Two things, both about cars that are not a player's own:
	 *  - remember a contact involving OUR traffic car, so the owner's engine
	 *    response and a peer's push do not both move it (see MpHandleHit);
	 *  - a contact between OUR car and a PEER's traffic car (a mirror on this
	 *    machine) must reach the slot's OWNER, where the car really is. */
	{
		int ourTraffic = -1;
		int localHit = -1;

		if (MpSlotInOurBand(mineId))
			ourTraffic = mineId;
		if (MpSlotInOurBand(otherId))
			ourTraffic = otherId;

		if (ourTraffic >= 0)
			gMpTrafficSelfContact[ourTraffic] = gMp.frame;

		if (mineId == me->carId && otherId >= 0 && otherId < MAX_CARS && gMpPeerTraffic[otherId])
			localHit = otherId;
		else if (otherId == me->carId && mineId >= 0 && mineId < MAX_CARS && gMpPeerTraffic[mineId])
			localHit = mineId;

		if (localHit >= 0)
		{
			MpTrafficContactPush(me, localHit);
			return JER_RESULT_CONTINUE;
		}
	}

	if (mineId == me->carId)
		them = MpGetPlayerByCar(otherId);
	else if (otherId == me->carId)
		them = MpGetPlayerByCar(mineId);
	else
		return JER_RESULT_CONTINUE;	/* two other cars; not our business */

	if (them == NULL || !them->active || them->isLocal || them->carId < 0)
		return JER_RESULT_CONTINUE;

	/* Remember it whether or not we act, so the RECEIVING half knows the engine
	 * has this contact in hand and does not add the peer's push on top. */
	gMpSelfContactFrame[them->id] = gMp.frame;

	if (!MpHitArmed())
		return JER_RESULT_CONTINUE;

	if (gMp.frame - them->lastHitFrame < MP_HIT_COOLDOWN_FRAMES)
		return JER_RESULT_CONTINUE;

	MpContactPush(me, them, 0, "engine contact");

	return JER_RESULT_CONTINUE;
}

/* The FALLBACK probe, on the sim frame. The engine's own contact hook above is
 * what reports a hit; this only catches an overlap the engine never resolved --
 * a snapshot teleport, or a car that arrived while we were loading. It applies the
 * LOCAL half as well, precisely because the engine did not move us in that case. */
void MpHitFrame(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* mine;
	int i;

	if (me == NULL || me->carId < 0)
		return;

	if (!MpHitArmed())
		return;

	mine = &car_data[me->carId];

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* pl = &gMp.players[i];
		CAR_DATA* other;
		long long dx, dy, dz, d2;

		if (!pl->active || pl->isLocal || pl->id == me->id || pl->carId < 0)
			continue;
		if (gMp.frame - pl->lastHitFrame < MP_HIT_COOLDOWN_FRAMES)
			continue;

		/* STAND DOWN while the engine is reporting contacts with this peer. The
		 * probe is the fallback for an overlap the engine never saw; left to run
		 * whenever it likes it takes the shared cooldown first (it is a distance
		 * test, so it fires on a near miss the engine's collision boxes do not
		 * touch) and masks the accurate trigger for the next 15 frames. Measured
		 * before this: 3..5 engine contacts against ~68 probe hits in one pair
		 * run, i.e. the primary trigger was starved by its own fallback. */
		if (gMpSelfContactFrame[pl->id] != 0 &&
			(gMp.frame - gMpSelfContactFrame[pl->id]) < MP_HIT_PROBE_QUIET)
			continue;

		/* The probe is the FALLBACK for a snapshot gap. While the owner's samples
		 * are arriving, the engine's own contact is the trigger -- and a
		 * distance-only test at this radius fires on near misses (measured in
		 * the docs at ~68 probe hits against 3-5 engine contacts), so most
		 * "phantom bumps" came from here. A gap is exactly the case the probe
		 * exists for, so gate on the newest SAMPLE's arrival, not lastStateFrame
		 * (which the apply path refreshes every frame even while holding). */
		{
			MP_CAR_PLAY* bp = &gMpPlay[pl->id];

			if (bp->count > 0 &&
			    (unsigned)(gMp.frame - bp->snap[bp->head].recvFrame) <= 2)
				continue;
		}

		other = &car_data[pl->carId];

		dx = (long long)other->hd.where.t[0] - (long long)mine->hd.where.t[0];
		dy = (long long)other->hd.where.t[1] - (long long)mine->hd.where.t[1];
		dz = (long long)other->hd.where.t[2] - (long long)mine->hd.where.t[2];
		d2 = dx * dx + dy * dy + dz * dz;

		if (d2 == 0 || d2 > (long long)MP_HIT_RADIUS * MP_HIT_RADIUS)
			continue;

		MpContactPush(me, pl, 1, "probe");
	}
}

void MpHandleHit(int connIndex, const unsigned char* p, int len)
{
	MP_HIT h;
	MP_PLAYER* me = MpLocalPlayer();
	int from, i;

	if (len < (int)sizeof(h))
		return;

	memcpy(&h, p, sizeof(h));

	/* Clamp BEFORE anything acts on it. Nothing here checked the magnitude, and a
	 * contact is reported by the peer that felt it -- so one message could have
	 * flung any car it named across the map, from any distance. Clamped rather than
	 * dropped so an over-large value still behaves like the hardest legal hit. */
	for (i = 0; i < 3; i++)
	{
		if (h.impulse[i] > MP_HIT_IMPULSE_MAX)
			h.impulse[i] = MP_HIT_IMPULSE_MAX;
		else if (h.impulse[i] < -MP_HIT_IMPULSE_MAX)
			h.impulse[i] = -MP_HIT_IMPULSE_MAX;
	}

	from = MpConnPlayerId(connIndex);

	/* A TRAFFIC contact: targetId is a car_data slot, not a player id. Only the
	 * OWNER of the slot applies it (its own CIV_AI/PURSUER_AI car) -- on anyone else
	 * it is a mirror rewritten from the wire, so a push there would be lost. Our own
	 * engine may already have resolved the same contact against the peer's mirror
	 * (marked in MpOnCarContact); within MP_HIT_SELF_FRAMES the push is that
	 * duplicate and is dropped. */
	if (h.flags & MP_HIT_F_TRAFFIC)
	{
		int slot = (int)h.targetId;

		if (slot >= 0 && slot < MAX_CARS)
		{
			CAR_DATA* cp = &car_data[slot];

			if (cp->controlType == CONTROL_TYPE_CIV_AI || cp->controlType == CONTROL_TYPE_PURSUER_AI)
			{
				if (gMpTrafficSelfContact[slot] != 0 &&
				    (gMp.frame - gMpTrafficSelfContact[slot]) <= MP_HIT_SELF_FRAMES)
				{
					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] hit: player %d bumped our traffic slot %d (our engine has it, kept)\n",
							from, slot);
				}
				else
				{
					cp->st.n.linearVelocity[0] += h.impulse[0];
					cp->st.n.linearVelocity[1] += h.impulse[1];
					cp->st.n.linearVelocity[2] += h.impulse[2];

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] hit: player %d bumped our traffic slot %d (push %ld,%ld,%ld)\n",
							from, slot, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
				}
			}
		}

		/* The host relays a traffic hit aimed at another client's band. */
		if (MpIsHost())
		{
			int owner = MpTrafficSlotOwnerRank((int)h.targetId);
			int ci = (owner >= 0) ? MpConnFindByPlayer(owner) : -1;

			if (ci >= 0 && ci != connIndex)
				MpSendConn(ci, MP_TAG_HIT, 0, p, len);
		}

		return;
	}

	/* A timer mode: a client's contact report is a tag CLAIM. The host checks it
	 * against its own view of both cars before the referee may act on it. */
	{
		/* the push back out of the impulse (normal * push << 12): a hit's strength */
		double ix = h.impulse[0], iy = h.impulse[1], iz = h.impulse[2];

		MpMarkHostReport(from, (int)h.targetId, 1, (int)(sqrt(ix * ix + iy * iy + iz * iz) / MP_FIXEDH));
	}

	if (me != NULL && h.targetId == me->id && me->carId >= 0 && me->carId < MAX_CARS)
	{
		CAR_DATA* cp = &car_data[me->carId];
		MP_PLAYER* src = MpGetPlayer(from);
		long d = -1;

		/* How far apart the two cars are ON THIS MACHINE when the push lands.
		 * Without a contact timestamp on the wire this is the only honest
		 * "was this hit stale?" measurement available to the receiver -- and it
		 * is the one the latency work moves: before interpolation/validation a
		 * delayed push commonly arrives with the cars already separated. */
		if (src != NULL && src->carId >= 0 && src->carId < MAX_CARS)
		{
			CAR_DATA* sc = &car_data[src->carId];
			long long dx = (long long)sc->hd.where.t[0] - (long long)cp->hd.where.t[0];
			long long dy = (long long)sc->hd.where.t[1] - (long long)cp->hd.where.t[1];
			long long dz = (long long)sc->hd.where.t[2] - (long long)cp->hd.where.t[2];

			d = (long)sqrt((double)(dx * dx + dy * dy + dz * dz));
		}

		gMpHitRx++;

		if (d > MP_HIT_RADIUS)
			gMpHitSeparated++;

		if (MpDebugOn() && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] hit rx: player %d |d|=%ld srcAge=%lu tf=%u push=%ld,%ld,%ld\n",
				from, d,
				(src != NULL && src->lastStateFrame != 0)
					? (unsigned long)(gMp.frame - src->lastStateFrame) : 0UL,
				(unsigned)h.targetFrame,
				(long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);

		/* VALIDATE BEFORE APPLYING. targetFrame is a frame of OUR OWN carstate,
		 * so we can ask two questions no arrival-time check could answer:
		 *  - is this recent enough to act on at all?
		 *  - were we actually where the sender says, when it says?
		 * A push that fails either is a ghost: the sender collided with a pose
		 * this machine had already left, and applying it now is the car that
		 * bounces off nothing. */
		{
			int age = (h.targetFrame != 0)
				? (int)((unsigned)gMp.frame - h.targetFrame) : 9999;
			int hx = 0, hy = 0, hz = 0;
			long poseD = -1;
			const char* why = NULL;

			if (h.targetFrame == 0)
				why = "no timeline";
			else if (age < 0 || age > MP_HIT_MAX_AGE_FRAMES)
				why = "stale";
			else if (!MpSelfPoseAt(h.targetFrame, &hx, &hy, &hz))
				why = "frame not in history";
			else
			{
				long long pdx = (long long)hx - h.tgtPos[0];
				long long pdy = (long long)hy - h.tgtPos[1];
				long long pdz = (long long)hz - h.tgtPos[2];

				poseD = (long)sqrt((double)(pdx * pdx + pdy * pdy + pdz * pdz));

				if (poseD > MpHitPoseTolerance(cp, src))
					why = "pose mismatch";
			}

			/* A consistent timestamp is still not enough: by the time the push
			 * lands the cars may have finished separating. Applying it then is
			 * exactly the phantom bump, so a late push also has to find us
			 * still near the other car. */
			if (why == NULL && age > MP_HIT_SELF_FRAMES && d > MP_HIT_RX_RADIUS)
				why = "cars separated";

			if (why != NULL)
			{
				gMpHitRejected++;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx,
						"[mp] hit: player %d bumped us (push %ld,%ld,%ld) "
						"ghost hit rejected (%s: age=%d |d|=%ld pose=%ld)\n",
						from, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2],
						why, age, d, poseD);
				return;
			}
		}

		/* ONCE per contact. Our own engine resolves the same collision (both
		 * machines simulate both cars), and its rigid-body response is already in
		 * our velocity -- so adding the peer's push as well lands one collision
		 * twice. Compare the frame the engine saw it against targetFrame, both in
		 * OUR numbering: the old arrival-frame window is wrong across latency,
		 * because the push arrives many frames after the collision it describes. */
		if (from >= 0 && from < MP_MAX_PLAYERS && gMpSelfContactFrame[from] != 0 &&
			(int)h.targetFrame - (int)gMpSelfContactFrame[from] <= MP_HIT_SELF_FRAMES &&
			(int)gMpSelfContactFrame[from] - (int)h.targetFrame <= MP_HIT_SELF_FRAMES)
		{
			gMpHitKept++;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] hit: player %d bumped us (push %ld,%ld,%ld; our engine has it, kept)\n",
					from, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
			return;
		}

		gMpHitApplied++;

		cp->st.n.linearVelocity[0] += h.impulse[0];
		cp->st.n.linearVelocity[1] += h.impulse[1];
		cp->st.n.linearVelocity[2] += h.impulse[2];

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] hit: player %d bumped us (push %ld,%ld,%ld)\n",
				from, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2]);
		return;
	}

	/* The host is the hub: a hit aimed at another CLIENT is relayed to it. */
	if (MpIsHost())
	{
		int ci = MpConnFindByPlayer(h.targetId);

		if (ci >= 0 && ci != connIndex)
			MpSendConn(ci, MP_TAG_HIT, 0, p, len);
	}
}

/* The hit counters, once per MP_HIT_SUMMARY_FRAMES. Counter-only: the increments
 * are unconditional and nearly free, this line is what MP_DEBUG pays for. */
void MpHitSummary(void)
{
	if (gMp.frame == 0 || (gMp.frame % MP_HIT_SUMMARY_FRAMES) != 0)
		return;

	if (gMpHitSent == 0 && gMpHitRx == 0)
		return;

	if (MpDebugOn() && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] hit summary: sent=%lu rx=%lu applied=%lu kept=%lu separated=%lu rejected=%lu\n",
			gMpHitSent, gMpHitRx, gMpHitApplied, gMpHitKept, gMpHitSeparated, gMpHitRejected);
}

static void MpCopHoldRelease(void)
{
	if (gMpCopHold >= 0 && !gMpBandReserved[gMpCopHold])
		reservedSlots[gMpCopHold] = 0;

	gMpCopHold = -1;
}

/* How many machines share the traffic pool. OUR OWN rank must always have a band,
 * so the count is never below rank+1: a client that has just joined and not yet
 * seen the roster still gets a private band rather than clamping onto the host's. */
static int MpTrafficMachineCount(void)
{
	int n = gMp.playerCount;

	if (n < gMp.localPlayerId + 1)
		n = gMp.localPlayerId + 1;

	if (n < 1)
		n = 1;

	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	return n;
}

/* The inclusive band for machine `rank` of `nMachines`. The last machine takes
 * the remainder so the whole pool is covered even when it does not divide.
 *
 * More machines than slots means there is no private slot to hand out: the machine
 * gets an EMPTY band (first = last = -1) and spawns no traffic of its own, living
 * off its peers' mirrors instead. The old code clamped every extra machine onto
 * the pool's LAST slot, so two machines spawned into one slot and then mirrored
 * each other's car into it. */
static void MpTrafficBandFor(int rank, int nMachines, int* first, int* last)
{
	int per, f, l;

	if (rank < 0 || rank >= MP_TRAFFIC_SLOT_COUNT || nMachines > MP_TRAFFIC_SLOT_COUNT)
	{
		*first = -1;
		*last = -1;
		return;
	}

	per = MP_TRAFFIC_SLOT_COUNT / nMachines;

	if (per < 1)
		per = 1;

	f = MP_TRAFFIC_SLOT_FIRST + rank * per;
	l = f + per - 1;

	if (rank == nMachines - 1)
		l = MAX_CARS - 1;

	if (f > MAX_CARS - 1)
		f = MAX_CARS - 1;

	if (l > MAX_CARS - 1)
		l = MAX_CARS - 1;

	if (l < f)
		l = f;

	*first = f;
	*last = l;
}

/* Release every slot WE reserved. Called when the session ends, so the engine's
 * traffic is untouched once we are not replicating it. */
void MpTrafficBandClear(void)
{
	int i;

	MpCopHoldRelease();

	for (i = 0; i < MAX_CARS; i++)
	{
		if (gMpBandReserved[i])
		{
			reservedSlots[i] = 0;
			gMpBandReserved[i] = 0;
		}
	}

	gMpBandFirst = -1;
	gMpBandLast = -1;
}

/* This machine's traffic band, or 0 when no session is live. */
int MpTrafficBand(int* first, int* last)
{
	if (gMpBandFirst < 0)
		return 0;

	if (first != NULL)
		*first = gMpBandFirst;

	if (last != NULL)
		*last = gMpBandLast;

	return 1;
}

/* Is `slot` a civilian a player could actually collide with -- ours or a mirror? */
static int MpSlotInOurBand(int slot)
{
	int f, l;

	return slot >= 0 && slot < MAX_CARS && MpTrafficBand(&f, &l) && slot >= f && slot <= l;
}

/* The machine whose band owns `slot`. Bands are cut by rank and our rank IS the
 * player id, so this is the id of the peer to send a traffic contact to. */
int MpTrafficSlotOwnerRank(int slot)
{
	int n = MpTrafficMachineCount();
	int r;

	for (r = 0; r < n; r++)
	{
		int f, l;

		MpTrafficBandFor(r, n, &f, &l);

		if (slot >= f && slot <= l)
			return r;
	}

	return -1;
}

/* Report a contact between OUR car and a PEER's traffic car (a mirror here). The
 * impulse goes to the slot's OWNER: on this machine the car is a mirror rewritten
 * from the peer's stream every frame, so a push applied here is simply discarded.
 * One rate limit per slot, shared with the engine-contact trigger, like the players. */
static void MpTrafficContactPush(MP_PLAYER* me, int slot)
{
	CAR_DATA* mine = &car_data[me->carId];
	CAR_DATA* other = &car_data[slot];
	long px, py, pz, closing, push;
	MP_HIT h;
	int owner;

	if (gMp.frame - gMpTrafficHitFrame[slot] < MP_HIT_COOLDOWN_FRAMES)
		return;

	closing = MpContactNormal(mine, other, &px, &py, &pz);

	if (closing < MP_HIT_MIN_CLOSING)
		return;

	push = closing * MP_HIT_PUSH / 100;

	if (push > MP_HIT_MAX_PUSH)
		push = MP_HIT_MAX_PUSH;

	owner = MpTrafficSlotOwnerRank(slot);

	if (owner < 0)
		return;

	gMpTrafficHitFrame[slot] = gMp.frame;

	memset(&h, 0, sizeof(h));
	h.flags = MP_HIT_F_TRAFFIC;
	h.targetId = (uint8_t)slot;
	h.impulse[0] = (int)(px * push);
	h.impulse[1] = (int)(py * push);
	h.impulse[2] = (int)(pz * push);

	if (MpIsHost())
	{
		int ci = MpConnFindByPlayer(owner);

		if (ci >= 0)
			MpSendConn(ci, MP_TAG_HIT, 0, &h, sizeof(h));
	}
	else
		MpSendToHost(MP_TAG_HIT, 0, &h, sizeof(h));

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] hit: we bumped a player's traffic car in slot %d (push %ld,%ld,%ld) -> owner %d\n",
			slot, (long)h.impulse[0], (long)h.impulse[1], (long)h.impulse[2], owner);
}

/* Once per sim frame, before anything spawns. Sets the ONE bit of state the two
 * machines have to agree on before traffic can be replicated at all: which
 * car_data slots this machine may put a civilian in. */
void MpTrafficBandRefresh(void)
{
	int n, rank, f, l, i, changed;

	if (!gMp.running)
	{
		MpTrafficBandClear();
		return;
	}

	n = MpTrafficMachineCount();
	rank = (gMp.localPlayerId > 0) ? gMp.localPlayerId : 0;

	MpTrafficBandFor(rank, n, &f, &l);

	changed = (f != gMpBandFirst || l != gMpBandLast);

	/* Everything OUTSIDE our band is kept off our spawner: the player and
	 * JERICHO-managed reserve [0..JER_SLOT_TRAFFIC_FIRST) because a civilian there
	 * would squat on a slot a player (or a managed vehicle, or a late joiner)
	 * needs, and every other machine's band because its cars get mirrored into
	 * those slots and OUR spawner must never race the peer for one.
	 *
	 * reservedSlots is the array PingInCivCar's free-slot scan consults
	 * (civ_ai.c:2058; the two collider indices are already outside that scan's
	 * bound), so this is the one bit of state that keeps the two spawners apart.
	 * We set only the bits we own and clear only those, so a level load dropping
	 * them is simply re-asserted on the next frame. */
	for (i = 0; i < MAX_CARS; i++)
	{
		int want = (i < f || i > l);

		if (want)
		{
			if (!gMpBandReserved[i])
			{
				reservedSlots[i] = 1;
				gMpBandReserved[i] = 1;
			}
		}
		else if (gMpBandReserved[i])
		{
			reservedSlots[i] = 0;
			gMpBandReserved[i] = 0;
		}
	}

	gMpBandFirst = f;
	gMpBandLast = l;

	/* The cop slot hold (see gMpCopHold): re-decided every frame, before the
	 * spawner runs. Only with the session's cops on, so a no-cops session and
	 * single player keep every slot for traffic. */
	MpCopHoldRelease();

	if (f >= 0 && gMp.ruleSet && gMp.ruleCops && CopsAllowed &&
	    numCopCars < maxCopCars && !requestCopCar)
	{
		int freeCount = 0, lastFree = -1;

		for (i = f; i <= l; i++)
		{
			if (car_data[i].controlType == CONTROL_TYPE_NONE && !reservedSlots[i])
			{
				freeCount++;
				lastFree = i;
			}
		}

		if (freeCount == 1)
		{
			reservedSlots[lastFree] = 1;
			gMpCopHold = lastFree;
		}
	}

	if (changed && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx,
			"[mp] traffic band: machine %d of %d owns car_data slots %d..%d\n",
			rank, n, f, l);
}

/* A squared world distance must be 64-BIT: coordinates reach six figures, so the
 * 32-bit square wraps negative and a far car reads as the nearest. */
long long MpDistSq64(int dx, int dz)
{
	long long a = dx, b = dz;

	return a * a + b * b;
}

/* The player nearest to (x,z), the lowest player id winning a tie, or -1 when no
 * player has a car to measure from. *outSq receives that player's squared distance. */
static int MpTrafficClaimOwner(int x, int z, long long* outSq)
{
	long long d[MP_MAX_PLAYERS];
	int id[MP_MAX_PLAYERS];
	int n = 0, pi, k, best = -1;
	long long dMin = 0;
	long long marginSq = (long long)MP_TRAFFIC_CLAIM_DEADBAND * MP_TRAFFIC_CLAIM_DEADBAND;

	for (pi = 0; pi < MP_MAX_PLAYERS; pi++)
	{
		MP_PLAYER* p = &gMp.players[pi];

		if (!p->active || p->carId < 0 || p->carId >= MAX_CARS)
			continue;

		d[n] = MpDistSq64(x - car_data[p->carId].hd.where.t[0],
				  z - car_data[p->carId].hd.where.t[2]);
		id[n] = p->id;
		n++;
	}

	if (n == 0)
		return -1;

	dMin = d[0];

	for (k = 1; k < n; k++)
	{
		if (d[k] < dMin)
			dMin = d[k];
	}

	/* Within the DEADBAND of the nearest, the LOWEST player id owns the car -- not an
	 * exact tie, which real positions never produce. That gives both machines the
	 * same answer AND a stable one: a car drifting a few units either way does not
	 * change who owns it, which is what stops the boundary flapping (and the
	 * spawn/cull churn it caused when the test was exact equality). */
	for (k = 0; k < n; k++)
	{
		if (d[k] <= dMin + marginSq && (best < 0 || id[k] < best))
			best = id[k];
	}

	if (outSq != NULL)
		*outSq = dMin;

	return best;
}

/* Is one of the PEER's mirrors already near (x,z)? The claim must only remove OUR
 * copy of a car the peer actually has HERE -- otherwise culling would leave a gap:
 * the machine that owns this patch has no car to show yet, so the car simply
 * vanishes until its owner's next spawn. Requiring the peer's car to be on our
 * screen first is what makes the handoff gap-free -- we stay until theirs arrives. */
static int MpPeerMirrorNear(int x, int z)
{
	long long best = -1;
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		long long d;

		if (!gMpPeerTraffic[i])
			continue;

		d = MpDistSq64(x - car_data[i].hd.where.t[0], z - car_data[i].hd.where.t[2]);

		if (best < 0 || d < best)
			best = d;
	}

	return best >= 0 && best <= (long long)MP_TRAFFIC_CLAIM_DEADBAND * MP_TRAFFIC_CLAIM_DEADBAND;
}

/* Once per sim frame: give up any civilian in OUR band that a peer's player owns.
 *
 * Note the direction of the rule: every machine KEEPS its nearer half, so both
 * machines still have traffic -- an earlier version made the higher-id machine stop
 * spawning entirely when a peer was close, which (before state is mirrored between
 * the machines) simply left it with no traffic at all. */
void MpTrafficClaimPass(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	int first, last, i, local = gMp.localPlayerId;
	int culled = 0, census = 0;
	long long nearSq = -1;

	if (!MpTrafficBand(&first, &last))
		return;

	/* A player on foot owns no car, so the claim has nothing to measure from and
	 * every car would look like a peer's -- leave the traffic alone instead. */
	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	for (i = first; i <= last; i++)
	{
		CAR_DATA* cp = &car_data[i];
		int owner;

		if (cp->controlType != CONTROL_TYPE_CIV_AI)
			continue;	/* PURSUER_AI (a cop in pursuit) is never handed off */

		/* Nor a patrolling cop: a cop off duty is a CIV_AI car with CONTROL_FLAG_COP
		 * (dr2types.h). It is OUR cop, there for OUR player; the peer's copy of the
		 * area is not chasing us, so handing it off deleted our police. */
		if (cp->controlFlags & CONTROL_FLAG_COP)
			continue;

		census++;
		{
			long long dSq = MpDistSq64(cp->hd.where.t[0] - car_data[me->carId].hd.where.t[0],
						  cp->hd.where.t[2] - car_data[me->carId].hd.where.t[2]);

			if (nearSq < 0 || dSq < nearSq)
				nearSq = dSq;
		}

		owner = MpTrafficClaimOwner(cp->hd.where.t[0], cp->hd.where.t[2], NULL);

		/* Hand off ONLY when the peer's copy is already on our screen. Culling our
		 * car while the owner has not spawned theirs yet is the gap the census used to
		 * show as a car blinking out and a different one taking its place. */
		if (owner >= 0 && owner != local &&
		    MpPeerMirrorNear(cp->hd.where.t[0], cp->hd.where.t[2]))
		{
			PingOutCar(cp);
			culled++;
		}
	}

	gMpClaimCulled += culled;

	/* A CENSUS, so "there is no traffic" is a number and not an impression -- how
	 * many civilians our band actually holds right now, and how many the claim gave
	 * away this window. The engine's own spawn rate is elsewhere (JERICHO_DIAG_PINGIN
	 * in civ_ai.c); this is the half that is OURS. */
	if ((gMp.frame % 120) == 0 && gMpCtx != NULL)
	{
		gMpCtx->jer_log(gMpCtx,
			"[mp] traffic: %d civ car(s) in our band %d..%d, nearest %d, culled %d this window\n",
			census, first, last, (nearSq < 0) ? -1 : (int)sqrt((double)nearSq), gMpClaimCulled);
		gMpClaimCulled = 0;
	}
}

/* Signed distance between two frame numbers. The counters are 32-bit on this
 * platform and a match lasts minutes, so a plain subtraction is exact; the
 * casts just keep the wrap arithmetic out of the compiler's way. */
static int MpFrameAge(unsigned newer, unsigned older)
{
	return (int)((unsigned)newer - (unsigned)older);
}

void MpPlayClear(int id)
{
	if (id >= 0 && id < MP_MAX_PLAYERS)
		memset(&gMpPlay[id], 0, sizeof(gMpPlay[id]));
}

void MpPlayResetAll(void)
{
	memset(gMpPlay, 0, sizeof(gMpPlay));
	memset(gMpSelfPose, 0, sizeof(gMpSelfPose));
}

void MpRecordSelfPose(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	MP_SELF_POSE* h;

	if (me == NULL || me->carId < 0 || me->carId >= MAX_CARS)
		return;

	h = &gMpSelfPose[gMp.frame % MP_SELF_HISTORY];
	h->frame = (unsigned)gMp.frame;
	h->x = car_data[me->carId].hd.where.t[0];
	h->y = car_data[me->carId].hd.where.t[1];
	h->z = car_data[me->carId].hd.where.t[2];
}

static int MpSelfPoseAt(unsigned frame, int* x, int* y, int* z)
{
	const MP_SELF_POSE* h = &gMpSelfPose[frame % MP_SELF_HISTORY];

	if (h->frame != frame)
		return 0;

	*x = h->x;
	*y = h->y;
	*z = h->z;
	return 1;
}

/* Store one sample. Older/duplicate frames are dropped: TCP is ordered, but a
 * relayed snapshot can still arrive after a newer one from another path. */
static void MpPlayPush(int id, unsigned frame, const MP_CARSTATE_ENTRY* e)
{
	MP_CAR_PLAY* p;
	MP_CAR_SNAP* s;
	int i;

	if (id < 0 || id >= MP_MAX_PLAYERS || e == NULL)
		return;

	p = &gMpPlay[id];

	if (p->count > 0 && MpFrameAge(frame, (unsigned)p->lastFrame) <= 0)
		return;

	/* A jump this large in one sample is not motion: a respawn, a car change
	 * or a snapshot gap. Do not slide the body across the map -- start a fresh
	 * timeline, exactly as the old adopt did by simply writing the new pose. */
	if (p->count > 0)
	{
		MP_CAR_SNAP* newest = &p->snap[p->head];
		long long dx = (long long)e->x - newest->x;
		long long dy = (long long)e->y - newest->y;
		long long dz = (long long)e->z - newest->z;

		if (dx * dx + dy * dy + dz * dz >
		    (long long)MP_INTERP_SNAP_DIST * MP_INTERP_SNAP_DIST)
		{
			MpPlayClear(id);
			p = &gMpPlay[id];
		}
	}

	p->head = (p->head + 1) % MP_INTERP_RING;
	s = &p->snap[p->head];

	s->frame = frame;
	s->recvFrame = gMp.frame;
	s->x = e->x;
	s->y = e->y;
	s->z = e->z;
	s->heading = e->heading;

	for (i = 0; i < 4; i++)
		s->orient[i] = e->orient[i];
	for (i = 0; i < 3; i++)
		s->vel[i] = e->vel[i];
	for (i = 0; i < 3; i++)
		s->angVel[i] = e->angVel[i];

	if (p->count < MP_INTERP_RING)
		p->count++;

	p->lastFrame = (int)frame;

	if (!p->primed)
	{
		/* Start 3 frames in the past. With fewer samples than that the apply
		 * step holds the oldest one until the cursor reaches it, which is the
		 * 100 ms of delay this whole scheme is built on. */
		p->playFrame = (int)frame - MP_INTERP_DELAY;
		p->primed = 1;
	}
}

static void MpSnapUse(const MP_CAR_SNAP* s, int* x, int* y, int* z,
	int* heading, short orient[4], int vel[3], int angVel[3])
{
	int i;

	*x = s->x;
	*y = s->y;
	*z = s->z;
	*heading = s->heading;

	for (i = 0; i < 4; i++)
		orient[i] = s->orient[i];
	for (i = 0; i < 3; i++)
		vel[i] = s->vel[i];
	for (i = 0; i < 3; i++)
		angVel[i] = s->angVel[i];
}

static int MpLerpAngle(int a, int b, int num, int den)
{
	int d = ((b - a + 2048) & 4095) - 2048;

	return (a + d * num / den) & 4095;
}

/* Write a remote car's rigid body. The same fields the old adopt wrote: the
 * engine re-derives hd.direction from the quaternion, so the orientation and
 * both velocities have to travel with the position. */
static void MpPlayWriteBody(CAR_DATA* cp, int x, int y, int z, int heading,
	const short orient[4], const int vel[3], const int angVel[3])
{
	LONGQUATERNION q;
	MATRIX m;
	int i;

	cp->controlType = CONTROL_TYPE_PLAYER;

	for (i = 0; i < 4; i++)
		q[i] = orient[i];
	for (i = 0; i < 4; i++)
		cp->st.n.orientation[i] = q[i];
	for (i = 0; i < 3; i++)
		cp->st.n.linearVelocity[i] = vel[i];
	for (i = 0; i < 3; i++)
		cp->st.n.angularVelocity[i] = MpClampAngVel(angVel[i]);

	LongQuaternion2Matrix(&q, &m);
	m.t[0] = x;
	m.t[1] = y;
	m.t[2] = z;
	memcpy(&cp->hd.where, &m, sizeof(m));

	cp->hd.direction = heading;
}

/* Once per sim frame, before the world steps: place every remote car at the
 * pose the playout cursor names. This is the ONLY writer of a remote car's
 * body now, so the engine and the snapshot stream cannot fight over it. */
void MpApplyRemoteCarPoses(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* pl = &gMp.players[i];
		MP_CAR_PLAY* p;
		MP_CAR_SNAP* s0 = NULL;	/* first sample at or older than the cursor */
		MP_CAR_SNAP* s1 = NULL;	/* first sample at or newer than the cursor */
		const MP_CAR_SNAP* newest;
		CAR_DATA* cp;
		int x, y, z, heading;
		int vel[3], angVel[3];
		short orient[4];
		int k;

		if (!pl->active || pl->isLocal || pl->carId < 0 || pl->carId >= MAX_CARS)
			continue;

		p = &gMpPlay[pl->id];

		if (p->count == 0)
			continue;

		cp = &car_data[pl->carId];

		if (!p->primed)
		{
			p->playFrame = p->lastFrame - MP_INTERP_DELAY;
			p->primed = 1;
		}
		else
		{
			p->playFrame++;
		}

		/* A long stall runs the cursor ahead of what we have. Clamp it, and
		 * re-anchor when the data that finally arrives is far newer than it. */
		if (p->playFrame > p->lastFrame + MP_INTERP_EXTRAP_MAX)
			p->playFrame = p->lastFrame + MP_INTERP_EXTRAP_MAX;

		if (MpFrameAge((unsigned)p->lastFrame, (unsigned)p->playFrame) > MP_INTERP_RESYNC)
			p->playFrame = p->lastFrame - MP_INTERP_DELAY;

		/* Walk newest -> oldest to bracket the cursor. */
		for (k = 0; k < p->count; k++)
		{
			MP_CAR_SNAP* s = &p->snap[(p->head - k + MP_INTERP_RING) % MP_INTERP_RING];

			if (MpFrameAge(s->frame, (unsigned)p->playFrame) >= 0)
				s1 = s;		/* at or newer than the cursor */
			else
			{
				s0 = s;		/* the first one older than it */
				break;
			}
		}

		newest = &p->snap[p->head];

		if (s0 != NULL && s1 != NULL)
		{
			int den = MpFrameAge(s1->frame, s0->frame);
			int num = MpFrameAge((unsigned)p->playFrame, s0->frame);
			long long dot = 0;
			int sign;
			int j;

			if (den <= 0)
				den = 1;
			if (num < 0)
				num = 0;
			if (num > den)
				num = den;

			x = (int)((long long)s0->x * (den - num) / den + (long long)s1->x * num / den);
			y = (int)((long long)s0->y * (den - num) / den + (long long)s1->y * num / den);
			z = (int)((long long)s0->z * (den - num) / den + (long long)s1->z * num / den);
			heading = MpLerpAngle(s0->heading, s1->heading, num, den);

			/* Normalised lerp with a sign correction: q and -q are the same
			 * attitude, and lerping without it takes the long way round and
			 * can flip the car. */
			for (j = 0; j < 4; j++)
				dot += (long long)s0->orient[j] * s1->orient[j];

			sign = (dot < 0) ? -1 : 1;

			for (j = 0; j < 4; j++)
			{
				orient[j] = (short)(((long long)s0->orient[j] * (den - num) +
					(long long)s1->orient[j] * sign * num) / den);
			}

			for (j = 0; j < 3; j++)
				vel[j] = s1->vel[j];
			for (j = 0; j < 3; j++)
				angVel[j] = s1->angVel[j];

			p->mode = 0;
		}
		else if (s1 != NULL)
		{
			/* Before the oldest sample: show it (the spawn case). */
			MpSnapUse(s1, &x, &y, &z, &heading, orient, vel, angVel);
			p->mode = 1;
		}
		else
		{
			/* Beyond the newest sample: extrapolate on its velocity, capped. */
			int dt = MpFrameAge((unsigned)p->playFrame, newest->frame);

			if (dt < 0)
				dt = 0;
			if (dt > MP_INTERP_EXTRAP_MAX)
				dt = MP_INTERP_EXTRAP_MAX;

			MpSnapUse(newest, &x, &y, &z, &heading, orient, vel, angVel);

			x += (int)((long long)vel[0] * dt / MP_FIXEDH);
			y += (int)((long long)vel[1] * dt / MP_FIXEDH);
			z += (int)((long long)vel[2] * dt / MP_FIXEDH);

			p->mode = 2;
		}

		MpPlayWriteBody(cp, x, y, z, heading, orient, vel, angVel);

		/* Remember the pose the sender is actually looking at: a contact report
		 * names this frame, and MpContactPush sends this position with it. */
		p->playX = x;
		p->playY = y;
		p->playZ = z;

		pl->onFoot = 0;
		pl->lastStateFrame = gMp.frame;	/* the fallback gate in MpOnNetInput reads this */

		if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % MP_INTERP_DEBUG_FRAMES) == 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] interp: player %d depth=%d lag=%d mode=%s\n",
				pl->id, p->count, MpFrameAge((unsigned)p->lastFrame, (unsigned)p->playFrame),
				(p->mode == 0) ? "lerp" : (p->mode == 1) ? "hold" : "extrap");
	}
}

/* OUR CARS ARE NEVER THE TRAFFIC AI'S -- and never a free slot either.
 *
 * Our cars are built by InitPlayer, so they have no traffic-AI data. The civ-AI
 * pass (main.c: PingInCivCar, which scans for a slot whose controlType is free)
 * will take one and then walk into those fields: an access violation at
 * PingInCivCar+0x105, on both machines, part-way into an ordinary match with
 * nobody asking for anything.
 *
 * A car of ours that a player steps out of is left as CIV_AI with
 * thrustState = STOP / ctrlState = EMPTY (see MpFollowLocalCar): safe for the AI
 * AND enterable again, so that state is deliberately NOT taken back here. What
 * still has to be taken back is CONTROL_TYPE_NONE: a wrecked/abandoned car is a
 * free slot the traffic AI will recycle, which it cannot survive.
 * CONTROL_TYPE_PLAYER with nobody driving it is exactly what an unowned remote
 * car already is: it sits where it was left, and the AI ignores it.
 *
 * Every frame, in PRE_SIM -- which is BEFORE the civ-AI pass in the same frame, so
 * a car handed over this frame never reaches the AI at all. */
/* A slot that is no longer OURS. The gMpOurCars mark is sticky on purpose - the
 * traffic AI must never reach a car one of us owns - but it has to be dropped
 * when the owner leaves. MpKeepOurCarsFromTrafficAi below takes any marked slot
 * straight back off CONTROL_TYPE_NONE, which is exactly what removing a
 * departing player's car sets: without this the car flickered out and re-appeared
 * on the very next frame, looking like an identical car parked in its place. */
void MpReleaseOurCarSlot(int slot)
{
	if (slot >= 0 && slot < MAX_CARS)
	{
		gMpOurCars[slot] = 0;
		gJerCarNoPingOut[slot] = 0;
	}
}

void MpKeepOurCarsFromTrafficAi(void)
{
	int i;

	/* First, mark what is ours right now (and keep the mark: see gMpOurCars). */
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];
		int slot = p->carId;

		if (p->active && slot >= 0 && slot < MAX_CARS)
			gMpOurCars[slot] = 1;
	}

	for (i = 0; i < MAX_CARS; i++)
	{
		if (!gMpOurCars[i])
			continue;

		if (car_data[i].controlType == CONTROL_TYPE_NONE)
		{
			int was = car_data[i].controlType;

			/* A slot the ENGINE cleared (PingOutCar -> ClearMem then controlType
			 * NONE) has no cosmetics and no model left. Asking it back as a player
			 * car made a LIVE CAR_DATA with ap.carCos == NULL: SetUpCivCollFlags
			 * reads cp->ap.carCos->colBox.vz (offset 0xD8) for every car in the
			 * world, so the next frame dereferenced NULL. The car is gone - give
			 * the mark up and leave the slot free for the traffic spawner. */
			if (car_data[i].ap.carCos == NULL ||
				car_data[i].ap.model < 0 ||
				car_data[i].ap.model >= MAX_CAR_RESIDENT_MODELS ||
				!JerCarSlotUsable(car_data[i].ap.model))
			{
				gMpOurCars[i] = 0;
				continue;
			}

			car_data[i].controlType = CONTROL_TYPE_PLAYER;
			car_data[i].wheel_angle = 0;

			/* Same as the get-out path: an unowned car must not keep reading
			 * somebody's pad, or it drives off on its own. */
			car_data[i].ai.padid = &gMpQuietPad;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] car: our slot %d was controlType %d (free); "
					"taken back so the AI cannot reach it\n", i, was);
		}
	}
	/* PUBLISH THE MARKS TO THE ENGINE: PingOutCar refuses a marked slot. A parked
	 * (CIV_AI, EMPTY) player car is otherwise fair game for CheckPingOut's 24000-unit
	 * cull, the roadblock and spawn culls and the stationary-car request -- the
	 * "my car got deleted" report. Parked player cars now stay until someone takes
	 * them or the session ends. */
	for (i = 0; i < MAX_CARS; i++)
		gJerCarNoPingOut[i] = gMpOurCars[i];
}

/* whole = 1: the host's table of everyone. whole = 0: just us. */
void MpSendColors(int whole)
{
	unsigned char buf[sizeof(MP_COLOR) + MP_MAX_PLAYERS * sizeof(MP_COLOR_ENTRY)];
	MP_COLOR h;
	int i, n = 0;

	memset(&h, 0, sizeof(h));

	if (whole)
	{
		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			MP_PLAYER* p = &gMp.players[i];
			MP_COLOR_ENTRY e;

			if (!p->active)
				continue;

			memset(&e, 0, sizeof(e));
			e.playerId = (uint8_t)p->id;
			/* The local player's own colour lives in the config, not the registry row
			 * (which is only ever filled from the wire, and is zeroed for the host).
			 * Both send paths must agree on this or the host's colour is broadcast as
			 * black while the just-us path sends the real one. */
			if (p->isLocal)
			{
				e.on = (uint8_t)(gMp.config.colorOn ? 1 : 0);
				e.r = (uint8_t)gMp.config.colorR;
				e.g = (uint8_t)gMp.config.colorG;
				e.b = (uint8_t)gMp.config.colorB;
				e.pedModel = (uint8_t)MpPedModelFromWire(gMp.config.pedModel);
			}
			else
			{
				e.on = (uint8_t)(p->colorOn ? 1 : 0);
				e.r = (uint8_t)p->colorR;
				e.g = (uint8_t)p->colorG;
				e.b = (uint8_t)p->colorB;
				e.pedModel = (uint8_t)MpPedModelFromWire(p->pedModel);
			}

			memcpy(buf + sizeof(h) + n * sizeof(e), &e, sizeof(e));
			n++;
		}
	}
	else
	{
		MP_PLAYER* me = MpLocalPlayer();
		MP_COLOR_ENTRY e;

		if (me == NULL)
			return;

		memset(&e, 0, sizeof(e));
		e.playerId = (uint8_t)me->id;
		e.on = (uint8_t)(gMp.config.colorOn ? 1 : 0);
		e.r = (uint8_t)gMp.config.colorR;
		e.g = (uint8_t)gMp.config.colorG;
		e.b = (uint8_t)gMp.config.colorB;
		e.pedModel = (uint8_t)MpPedModelFromWire(gMp.config.pedModel);

		memcpy(buf + sizeof(h), &e, sizeof(e));
		n = 1;
	}

	if (n <= 0)
		return;

	h.count = (uint8_t)n;
	memcpy(buf, &h, sizeof(h));

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_COLOR, 0, buf, MpColorLen(n));
	else
		MpSendToHost(MP_TAG_COLOR, 0, buf, MpColorLen(n));
}

void MpHandleColor(int connIndex, const unsigned char* p, int len)
{
	MP_COLOR h;
	int i, n, from;
	int changed = 0;

	if (len < (int)sizeof(MP_COLOR))
		return;

	memcpy(&h, p, sizeof(h));

	from = MpConnPlayerId(connIndex);

	n = h.count;

	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	if (len < MpColorLen(n))
		return;

	for (i = 0; i < n; i++)
	{
		MP_COLOR_ENTRY e;
		MP_PLAYER* pl;
		int model;

		memcpy(&e, p + sizeof(MP_COLOR) + i * sizeof(e), sizeof(e));

		/* A colour is a player's own, so the same binding as their input row:
		 * without it any peer could repaint somebody else on every screen. */
		if (!MpRowOwnerOk(from, (int)e.playerId))
			continue;

		pl = MpGetPlayer(e.playerId);

		if (pl == NULL)
			continue;

		/* The wire carries a raw byte. Keep a valid owner choice (a remote
		 * Jericho may be imported by this machine at apply time); only an id
		 * outside the registry becomes Tanner. The apply path decides whether
		 * this level can actually draw it. */
		model = MpPedModelFromWire((int)e.pedModel);

		/* A colour is applied to its OWNER's character. The local player's own
		 * colour lives in the config (that is what the menu edits); for everyone
		 * else it is this table. */
		if (!pl->isLocal)
		{
			int raw = (int)e.pedModel;
			int modelChanged = (pl->pedModel != model);

			if (modelChanged)
				MpLogInboundPedModel("JPCL", pl->id,
					(int)(pl - gMp.players), raw,
					MpPedModelEffective(model));

			if (pl->colorOn != (e.on ? 1 : 0) ||
			    pl->colorR != e.r || pl->colorG != e.g || pl->colorB != e.b ||
			    modelChanged)
				changed = 1;

			pl->colorOn = e.on ? 1 : 0;
			pl->colorR = e.r;
			pl->colorG = e.g;
			pl->colorB = e.b;
			pl->pedModel = model;
		}
	}

	/* A client sends its identity to the host alone. The host owns the table and
	 * has just updated it, so republish it now rather than making every other peer
	 * wait for the 120-frame heartbeat: a mid-match character change should show
	 * on the next client that has a stand-in for that player. */
	if (changed && MpIsHost() && connIndex >= 0)
		MpSendColors(1);
}

void MpSendOwnCarState(void)
{
	unsigned char buf[sizeof(MP_CARSTATE) + sizeof(MP_CARSTATE_ENTRY)];
	MP_CARSTATE h;
	MP_CARSTATE_ENTRY e;
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* cp;
	int len;

	if (me == NULL)
		return;

	/* NO WORLD OF OURS YET. A joining client reaches `running` a few frames before
	 * its level is up, and StepSim (and so this send) can still be running on the
	 * frontend/attract world. car_data[0] there is either zeroed or the demo's car
	 * at the origin, and a body from it put a pose at 0,0 on the wire with a model
	 * that is not ours -- peers built the joiner at the origin, in the air, before
	 * its real pose arrived. Nothing about our car is meaningful until we are in
	 * the match. `gameStarted` is set by JER_EVENT_GAME_START (MpOnGameStart), when
	 * the level's cars really exist; the frontend check is the same refusal for the
	 * window before it. */
	if (gInFrontend || !gMp.gameStarted)
		return;

	/* The engine may have moved us into another car (or out of one) since the
	 * last frame -- adopt it BEFORE reading the pose, so this very frame already
	 * carries the change and the peers never see the old vehicle again. */
	MpFollowLocalCar();
	MpCarFailsafeTick();

	memset(&h, 0, sizeof(h));
	h.frame = gMp.frame;
	h.count = 1;

	memset(&e, 0, sizeof(e));
	e.playerId = (uint8_t)me->id;
	e.flags = MP_CARSTATE_HAS_BODY;
	e.crushScale = MP_CRUSH_HALF_DEFAULT;

	/* NO CAR (on foot): model 0xFF and NO pose. The peer must not adopt the zero
	 * position -- that would drag the car it was driving to the origin -- it
	 * releases that car instead. */
	e.model = MP_CARSTATE_NO_CAR;
	e.modelCity = MP_CAR_CITY_SESSION;

	cp = NULL;

	if (me->carId >= 0 && me->carId < MAX_CARS)
	{
		cp = &car_data[me->carId];

		/* TEST LEVER (MP_TEST_HUBCAP=<0..15>): knock hubcaps off on this seat so the
		 * wire path can be exercised headlessly. Inert unless set. */
		{
			static int testHub = -2;

			if (testHub == -2)
			{
				const char* s = getenv("MP_TEST_HUBCAP");

				testHub = (s != NULL) ? atoi(s) : -1;
			}

			if (testHub >= 0 && cp->controlType != CONTROL_TYPE_NONE)
				cp->ap.flags = (u_char)((cp->ap.flags & ~0x0f) | (testHub & 0x0f));
		}

		/* NOT A CAR YET. WELCOME sets our row's carId to engine slot 0 before the
		 * level exists, and the frontend/loading world has that CAR_DATA zeroed
		 * (controlType NONE, no model). A body sent from it carried a pose of
		 * 0,0 with a model that is not ours -- peers built the joiner at the world
		 * origin, in the air, before its real pose ever arrived. Keep the NO_CAR
		 * default until the engine has actually given us a car. */
		if (player[0].playerType != PLAYER_TYPE_CAR ||
			cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL ||
			cp->ap.model < 0 || cp->ap.model >= MAX_CAR_RESIDENT_MODELS ||
			!JerCarSlotUsable(cp->ap.model))
		{
			cp = NULL;
		}
	}

	/* THE CAR THEY WALKED AWAY FROM. A machine that was present for the get-out
	 * already holds this body and ignores the extra fields; a joiner that arrived
	 * afterwards has only the pedstate stand-in, so this is the only way it can
	 * build the parked car. Only advertise a body still parked by us: if someone
	 * has commandeered it, it is a player car now, not our left car. */
	if (cp == NULL && me->leftCarSlot1 > 0)
	{
		int left = me->leftCarSlot1 - 1;

		if (left >= 0 && left < MAX_CARS &&
			car_data[left].controlType == CONTROL_TYPE_CIV_AI &&
			car_data[left].ai.padid == &gMpQuietPad &&
			car_data[left].ap.carCos != NULL &&
			car_data[left].ap.model >= 0 && car_data[left].ap.model < MAX_CAR_RESIDENT_MODELS &&
			residentCarModels[car_data[left].ap.model] >= 0 &&
			JerCarSlotUsable(car_data[left].ap.model))
		{
			int src = GetCarModelSourceCity(car_data[left].ap.model);

			e.flags |= MP_CARSTATE_LEFT_CAR;
			e.leftModel = (uint8_t)residentCarModels[car_data[left].ap.model];
			e.leftModelCity = MpCityToWire(src);
			e.palette = (uint8_t)car_data[left].ap.palette;
			e.x = car_data[left].hd.where.t[0];
			e.y = car_data[left].hd.where.t[1];
			e.z = car_data[left].hd.where.t[2];
			e.heading = car_data[left].hd.direction;
		}
	}

	/* v24: out of the live round (no respawn) -- the host benches us on it */
	if (gMpRespawnOut)
		e.flags |= MP_CARSTATE_OUT;

	if (cp != NULL)
	{
		/* v24: the car's damage and wreck point (Cops n Robbers' meter reads the
		 * getaway's); saturated to the wire's 16 bits */
		e.totalDamage = (uint16_t)(cp->totalDamage < 0 ? 0 : cp->totalDamage > 0xFFFF ? 0xFFFF : cp->totalDamage);
		e.maxDamage = (uint16_t)(MaxPlayerDamage[0] < 0 ? 0 : MaxPlayerDamage[0] > 0xFFFF ? 0xFFFF : MaxPlayerDamage[0]);

		/* Say it when this is the body we last got OUT of: the peers released it on
		 * the NO_CAR row and can re-seat us on it instead of building a second
		 * vehicle (MpHandleCarState). Kept on every frame while we drive that same
		 * body, so one lost packet cannot reopen the window. Entering a DIFFERENT
		 * car leaves leftCarSlot1 pointing elsewhere, so the bit stays clear and
		 * the peers keep the parked body and build the new one, as before. */
		if (me->leftCarSlot1 > 0 && me->carId == me->leftCarSlot1 - 1)
			e.flags |= MP_CARSTATE_SAME_BODY;

		/* MP_DEBUG: the ORIENTATION is now the only 16-bit field left on the wire, so
		 * it is the only one a violent impact can saturate. If this fires, the peer is
		 * seeing a CLAMPED attitude (the intended degradation) rather than a wrapped
		 * one. Log-only. */
		if (MpDebugOn() && gMpCtx != NULL)
		{
			int a, big = 0;

			for (a = 0; a < 4; a++)
			{
				if (cp->st.n.orientation[a] > 32767 || cp->st.n.orientation[a] < -32768)
					big = 1;
			}

			if (big)
			{
				gMpCtx->jer_log(gMpCtx,
					"[mp] WIRE: orientation clamped on the wire -- orient %d %d %d %d\n",
					(int)cp->st.n.orientation[0], (int)cp->st.n.orientation[1],
					(int)cp->st.n.orientation[2], (int)cp->st.n.orientation[3]);
			}
		}

		e.palette = (uint8_t)cp->ap.palette;	/* the colour WE see our car in -- we own it */

		/* ...and WHAT we are driving, as a model NUMBER plus the city that number
		 * belongs to -- NOT our resident slot index, which does not mean the same
		 * car on two machines. The peer resolves the pair to ITS own slot
		 * (MpResidentSlotForCar). */
		{
			int slot = cp->ap.model;

			/* THE FAILSAFE'S BODY IS NOT THE CHOICE. While the wanted car is being
			 * rebuilt, the local car wears the host city's car 1 so the camera has
			 * something to follow; the session must still be told what the player
			 * chose, or peers would see them switch to that stand-in and back. */
			if (sMpStandIn && sMpWantModel >= 0)
			{
				e.model = (uint8_t)sMpWantModel;
				e.modelCity = MpCityToWire(sMpWantCity);
			}
			else if (slot >= 0 && slot < MAX_CAR_RESIDENT_MODELS && residentCarModels[slot] >= 0)
			{
				int src = GetCarModelSourceCity(slot);

				e.model = (uint8_t)residentCarModels[slot];
				e.modelCity = MpCityToWire(src);
			}
			/* else: the slot holds no model, so there is no car to describe --
			 * e.model stays MP_CARSTATE_NO_CAR (set above) and the peers release. */
		}

		e.x = cp->hd.where.t[0];
		e.y = cp->hd.where.t[1];
		e.z = cp->hd.where.t[2];
		e.heading = cp->hd.direction;
		e.orient[0] = MpSat16(cp->st.n.orientation[0]);
		e.orient[1] = MpSat16(cp->st.n.orientation[1]);
		e.orient[2] = MpSat16(cp->st.n.orientation[2]);
		e.orient[3] = MpSat16(cp->st.n.orientation[3]);
		e.vel[0] = cp->st.n.linearVelocity[0];
		e.vel[1] = cp->st.n.linearVelocity[1];
		e.vel[2] = cp->st.n.linearVelocity[2];
		e.angVel[0] = cp->st.n.angularVelocity[0];
		e.angVel[1] = cp->st.n.angularVelocity[1];
		e.angVel[2] = cp->st.n.angularVelocity[2];

		/* Hubcaps are appearance the OWNER decides: LoseHubcap writes one bit per
		 * wheel in ap.flags, and the peer's rebuilt body would otherwise keep all
		 * four. The low nibble is all that is needed (0 = all present). */
		e.hubcaps = (uint8_t)(cp->ap.flags & 0x0f);

		/* ...and the body-crush multiplier. ASK crumple rather than read it here:
		 * the value and the pause menu that edits it belong to that module, and a
		 * session with crumple off simply answers 1.0x. Per CAR: this is the
		 * multiplier for the car WE are driving, and every peer applies it to the
		 * car they hold for us. */
		{
			JER_CRUMPLE_SCALE_ARGS crush;

			memset(&crush, 0, sizeof(crush));
			crush.carId = cp->id;
			crush.scale = 4096;
			jer_fire(JER_CRUMPLE_GET_SCALE, &crush);

			e.crushScale = MpCrushHalfFromScale(crush.scale);
		}

		/* Siren / lightbar state (owner-authoritative). Traffic already does this
		 * via MP_TRAFFIC_SIREN; player cars need the same so a remote copy draws
		 * the light and plays the sound. The engine only auto-draws for PURSUER_AI
		 * / roadblock / cutscene, so we must tell the peers explicitly.
		 * TODO: replace dying<75 with the real player "siren is on" condition. */
		if (CarHasSiren(cp->ap.model) && cp->ai.p.dying < 75)
			e.flags |= MP_CARSTATE_SIREN;
	}

	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), &e, sizeof(e));
	len = (int)(sizeof(h) + sizeof(e));

	/* The host is the hub: it broadcasts its OWN car to every client, and relays
	 * each client's car on (MpHandleCarState does the relay). A client sends its
	 * one car to the host. */
	if (MpIsHost())
		MpHostBroadcast(MP_TAG_CARSTATE, 0, buf, len);
	else
		MpSendToHost(MP_TAG_CARSTATE, 0, buf, len);
}

/* ------------------------------------------------------------------ */
/* Traffic state: publish the civilians and police OUR band owns
 *
 * The band is a DISJOINT set of car_data slots, so -- unlike a player car, which
 * is matched by model -- a slot NUMBER is a shared identity here: the receiver
 * mirrors a row straight into the slot it names. One row per civilian/police car
 * we currently hold, batched into a single message. The count is capped by the
 * band (at most a handful), so this needs no distance throttling.
 *
 * A police car is carried like any other: it is the level's own police MODEL
 * (model 0, the car CarHasSiren keys off), so the siren follows from the model at
 * the receiver and nothing extra is needed here. A REMOVE row (the despawn half)
 * rides the same message. */
void MpSendTrafficState(void)
{
	unsigned char buf[sizeof(MP_TRAFFIC) + MAX_CARS * sizeof(MP_TRAFFIC_ENTRY)];
	unsigned char live[MAX_CARS];
	MP_TRAFFIC h;
	MP_TRAFFIC_ENTRY e;
	int first, last, i, count = 0, len;

	if (!MpTrafficBand(&first, &last))
		return;

	memset(live, 0, sizeof(live));
	memset(&h, 0, sizeof(h));
	h.frame = gMp.frame;

	for (i = first; i <= last; i++)
	{
		CAR_DATA* cp = &car_data[i];
		int slot, src;

		if (cp->controlType != CONTROL_TYPE_CIV_AI &&
		    cp->controlType != CONTROL_TYPE_PURSUER_AI)
			continue;

		if (count >= MAX_CARS)
			break;

		memset(&e, 0, sizeof(e));
		e.carSlot = (uint8_t)i;

		/* WHAT it is, as a model NUMBER plus the city that number belongs to -- the
		 * peer resolves the pair to its own resident slot (MpResidentSlotForCar). */
		slot = cp->ap.model;

		if (slot < 0 || slot >= MAX_CAR_RESIDENT_MODELS || residentCarModels[slot] < 0)
			continue;	/* no model -> nothing the peer could build */

		src = GetCarModelSourceCity(slot);
		e.model = (uint8_t)residentCarModels[slot];
		e.modelCity = MpCityToWire(src);
		e.palette = (uint8_t)cp->ap.palette;

		/* ON DUTY: the siren and lightbar state, decided HERE because this is the
		 * only machine that can decide it -- the car is ours, driven by our AI, and
		 * `controlType == PURSUER_AI` is exactly what the engine checks before it
		 * lights a cop car. The receiver builds a posed mirror, not an AI pursuer,
		 * so its own test answers "no" for every peer car whatever the cop is doing;
		 * it has to be told. The condition is the engine's own (cars.c). */
		if (cp->controlType == CONTROL_TYPE_PURSUER_AI &&
		    cp->ai.p.dying < 75 && CarHasSiren(cp->ap.model) != 0)
			e.flags |= MP_TRAFFIC_SIREN;

		/* ...and its DAMAGE, so a bent car reads the same on both screens. */
		{
			int z;

			for (z = 0; z < 6; z++)
				e.damage[z] = cp->ap.damage[z];

			e.totalDamage = (uint16_t)cp->totalDamage;
		}

		e.x = cp->hd.where.t[0];
		e.y = cp->hd.where.t[1];
		e.z = cp->hd.where.t[2];
		e.heading = cp->hd.direction;
		e.orient[0] = MpSat16(cp->st.n.orientation[0]);
		e.orient[1] = MpSat16(cp->st.n.orientation[1]);
		e.orient[2] = MpSat16(cp->st.n.orientation[2]);
		e.orient[3] = MpSat16(cp->st.n.orientation[3]);
		e.vel[0] = cp->st.n.linearVelocity[0];
		e.vel[1] = cp->st.n.linearVelocity[1];
		e.vel[2] = cp->st.n.linearVelocity[2];

		memcpy(buf + sizeof(h) + count * sizeof(e), &e, sizeof(e));
		count++;
		live[i] = 1;
	}

	/* DESPAWN: a car we published last frame that is NOT in this frame's rows has
	 * left our band (pinged out, wrecked, taken by a player). Tell the peer to drop
	 * its mirror now, rather than leaving a ghost for its staleness backstop. */
	for (i = first; i <= last && count < MAX_CARS; i++)
	{
		if (!gMpTrafficSent[i] || live[i])
			continue;

		memset(&e, 0, sizeof(e));
		e.carSlot = (uint8_t)i;
		e.flags = MP_TRAFFIC_REMOVE;
		memcpy(buf + sizeof(h) + count * sizeof(e), &e, sizeof(e));
		count++;
	}

	memcpy(gMpTrafficSent, live, sizeof(gMpTrafficSent));

	h.count = (uint8_t)count;
	memcpy(buf, &h, sizeof(h));
	len = (int)(sizeof(h) + count * sizeof(e));

	/* The host is the hub: it broadcasts its own band, and relays each client's on
	 * (MpHandleTraffic does the relay). A client sends its band to the host. */
	if (MpIsHost())
		MpHostBroadcast(MP_TAG_TRAFFIC, 0, buf, len);
	else
		MpSendToHost(MP_TAG_TRAFFIC, 0, buf, len);
}

void MpHandleCarState(int connIndex, const unsigned char* p, int len)
{
	MP_CARSTATE h;
	int i, n, forged = 0;

	if (len < (int)sizeof(MP_CARSTATE))
		return;

	memcpy(&h, p, sizeof(h));
	n = h.count;
	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	if (len < (int)(sizeof(MP_CARSTATE) + (size_t)n * sizeof(MP_CARSTATE_ENTRY)))
		return;

	for (i = 0; i < n; i++)
	{
		MP_CARSTATE_ENTRY e;
		MP_PLAYER* pl;
		CAR_DATA* cp;

		memcpy(&e, p + sizeof(MP_CARSTATE) + i * sizeof(e), sizeof(e));

		/* A carstate may only describe its SENDER (as for pedstate): otherwise one
		 * client could release or re-body another player's car on every machine.
		 * Checked before the local/unknown skip, or a row naming the host (local
		 * here) or an id not in the roster would be relayed unchecked. */
		if (MpIsHost() && connIndex >= 0 && e.playerId != MpConnPlayerId(connIndex))
		{
			forged = 1;
			continue;
		}

		pl = MpGetPlayer(e.playerId);

		if (pl == NULL || pl->isLocal)
			continue;

		/* v24: OUT and the getaway's damage, for the referee */
		if (MpIsHost() && connIndex >= 0)
			MpHostCarRowRound(pl->id, &e);

		/* NO CAR: the owner is on foot. Let the car we were driving for them go
		 * back to the world -- it stays where they left it (their own engine
		 * does the same), so nobody is left steering an empty car. */
		if (e.model == MP_CARSTATE_NO_CAR)
		{
			/* Just got OUT (this machine still had them in a car): the ped pose we
			 * hold is from their PREVIOUS time on foot. Forget it, so the stand-in
			 * is built from their first fresh JPPD at the car door instead of at
			 * the old spot and then snapped across the map (on-foot desync). */
			if (pl->carId >= 0)
				pl->pedLastMs = 0;

			/* No car to drive: drop the timeline too, or walking back into a
			 * body (or a different one) would replay the old car's motion. */
			MpPlayClear(pl->id);
			MpReleaseRemoteCar(pl);

			/* remember what they walked away from: if they leave, it goes too */
			if (pl->carId >= 0 && pl->carId < MAX_CARS)
			{
				pl->leftCarSlot1 = pl->carId + 1;
				pl->leftCarModel = car_data[pl->carId].ap.model;
			}

			/* ...AND mark the PLAYER as on foot.
			 *
			 * Releasing the car was not enough, and this is why neither machine
			 * could see the other on foot. Everything downstream keys off pl->carId
			 * -- not least MpDriveRemotePed, which refuses to stand a pedestrian in
			 * for a player while pl->carId >= 0. So the car went back to the world,
			 * the pedestrian was never created, and since BOTH ends made the same
			 * mistake it failed symmetrically rather than one way.
			 *
			 * carId is otherwise only assigned in the spawn paths and for our OWN
			 * player, so a peer who got out kept pointing at the car they had just
			 * left. The pose adopt below already skips a player with carId < 0,
			 * which is exactly right for someone who is on foot. */
			pl->carId = -1;
			pl->car = -1;
			pl->onFoot = 1;

			/* A joiner that arrived after the get-out has no local body for
			 * the car they left; the NO_CAR row names it. A machine that was
			 * present still holds the body and the helper is a no-op. */
			if (e.flags & MP_CARSTATE_LEFT_CAR)
				MpMaterializeLeftCar(pl, &e);

			continue;
		}

		/* REAL BODY: THE OWNER IS DRIVING. Whatever an earlier NO_CAR row
		 * recorded is stale now -- the carstate is the owner's authority and it
		 * arrives every frame, so a real model means they are back in a car.
		 *
		 * Clear this HERE, before any path below can `continue` (a car that is
		 * ours to drive locally, a body we could not adopt), because BOTH
		 * recovery paths gate on it: MpSpawnLateJoiners skips every row with
		 * onFoot set, and the roster's spawn request does the same. Leaving it
		 * set is what made a lost/reordered transition permanent: the owner was
		 * driving, this machine had no body for them, and the re-seat pass
		 * refused to touch them for the rest of the match. */
		if (pl->onFoot)
		{
			pl->onFoot = 0;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] heal: player %d is driving again (carstate model %d) - "
					"cleared onFoot before re-seating\n", pl->id, (int)e.model);
		}

		/* FIRST REAL POSE WINS. A later body state can still be the
		 * frontend/loading car at 0,0; overwriting the good pose with it is what
		 * built the joiner at the origin. The sender gates keep such a state out,
		 * but the ordering here is what makes the spawn deterministic. */
		if (!pl->joinedPose)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] join pose: player %d wire %d,%d h=%d model=%d frame=%u conn=%d\n",
					pl->id, e.x, e.z, (int)e.heading, (int)e.model, (unsigned)h.frame, connIndex);

			pl->joinedPose = 1;
			pl->joinPoseX = e.x;
			pl->joinPoseZ = e.z;
			pl->joinPoseHeading = (int)(e.heading & 0xfff);
		}

		/* HEAL: the owner's own carstate says they are DRIVING, but this machine
		 * holds no car for them. Believe the owner -- it is the authority for its
		 * car, and this message is current (a player on foot sends NO_CAR, handled
		 * just above, so reaching here with carId < 0 cannot mean "on foot").
		 *
		 * This is a deadlock, not a race. carId is only ever assigned by a spawn,
		 * and the pose adopt below bails on carId < 0, so once a get-OUT had set
		 * carId = -1 / onFoot = 1 nothing here ever looked at the owner's next
		 * "I have a car" again. The re-seat pass (MpSpawnLateJoiners) skips anyone
		 * with onFoot set, so the pair never broke: no car drawn on this machine
		 * for the rest of the match, and the stand-in pedestrian -- whose pedstate
		 * stops the instant its owner is back in a car -- stayed where it was,
		 * walking the spot and playing footstep sounds.
		 *
		 * The clear itself happens above, on every real-model carstate; this
		 * block only decides whether there is a body to hand back. When there is
		 * not, it arms the deferred spawn for the next frame, and the 30-frame
		 * re-seat pass remains the backstop. Logged once per get-out/get-in
		 * cycle, so the heal is visible when diagnosing. */
		if (pl->carId < 0 || pl->carId >= MAX_CARS)
		{
			int reused = 0;

			/* BACK IN THE BODY THEY LAST GOT OUT OF. The owner says so explicitly
			 * (MP_CARSTATE_SAME_BODY: walking back into their parked car, or the
			 * pause menu's Change car while on foot, which re-enters it and
			 * re-models it in place). We released that body on their NO_CAR row and
			 * it is still standing here, so put them back on it. Letting the heal
			 * below run instead would clear onFoot and the re-seat pass would call
			 * InitPlayer on ANOTHER slot: the same vehicle twice, one of them an
			 * orphan nobody drives.
			 *
			 * The body is only ours to hand back while it is still that car and
			 * nobody else has taken it: a cleared/recycled slot, a released mesh, a
			 * model that changed under us, a car another player (or WE) now drive,
			 * or ANOTHER PLAYER'S PARKED CLAIM on it (their leftCarSlot1) - any of
			 * those and this falls back to the spawn path, exactly as before.
			 * The claim test matters: a player who commandeered the body and then
			 * got out has carId < 0 but still owns it through leftCarSlot1, and
			 * MpGetPlayerByCar alone would hand it back to the original owner. */
			if ((e.flags & MP_CARSTATE_SAME_BODY) && pl->leftCarSlot1 > 0)
			{
				int slot = pl->leftCarSlot1 - 1;

				if (slot >= 0 && slot < MAX_CARS &&
					slot != (int)player[0].playerCarId &&
					MpGetPlayerByCar(slot) == NULL &&
					!MpCarSlotTakenByOther(pl, slot) &&
					car_data[slot].controlType != CONTROL_TYPE_NONE &&
					car_data[slot].ap.carCos != NULL &&
					car_data[slot].ap.model == pl->leftCarModel &&
					JerCarSlotUsable(car_data[slot].ap.model))
				{
					pl->carId = slot;
					pl->onFoot = 0;
					pl->leftCarSlot1 = 0;
					pl->leftCarModel = -1;
					reused = 1;

					/* Back in the body they left: it has been standing still, so
					 * there is no motion to interpolate from the old timeline. */
					MpPlayClear(pl->id);

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] player %d is back in the car they left (slot %d) - "
							"reusing the body instead of spawning another\n",
							pl->id, slot);
				}
			}

			/* ENTERED A DIFFERENT PARKED CAR. SAME_BODY only covers the car they
			 * last got out of; walking into ANY other parked player body (an older
			 * car of theirs, someone else's parked car, ours) used to fall through
			 * to the spawn below, which built a second copy while the body they
			 * really took stayed parked here: the MP car dupe on enter. That body
			 * is in this machine's world at the owner's reported pose, so find it
			 * and seat them in it. Traffic mirrors are excluded (their REMOVE row
			 * retires them); only stopped quiet-pad bodies nobody drives qualify. */
			if (!reused)
			{
				int slot = MpFindEnteredParkedBody(pl, &e);

				if (slot >= 0)
				{
					int k;

					/* the claim on that body moves with it: nobody else's parked
					 * car any more (MpMaterializeLeftCar would otherwise rebuild it) */
					for (k = 0; k < MP_MAX_PLAYERS; k++)
					{
						MP_PLAYER* o = &gMp.players[k];

						if (o->active && !o->isLocal && o->leftCarSlot1 == slot + 1)
						{
							o->leftCarSlot1 = 0;
							o->leftCarModel = -1;
						}
					}

					if (pl->leftCarSlot1 == slot + 1)
					{
						pl->leftCarSlot1 = 0;
						pl->leftCarModel = -1;
					}

					pl->carId = slot;
					pl->onFoot = 0;
					reused = 1;
					MpPlayClear(pl->id);

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] player %d got into the parked car in slot %d - "
							"seating them in it instead of spawning a copy\n",
							pl->id, slot);
				}
			}

			if (!reused)
			{
				/* No body of theirs to hand back: build one through the deferred
				 * spawn path. Arm it for the NEXT FRAME instead of waiting up to
				 * 30 for the periodic pass -- the owner says they are driving, so
				 * the sooner the body exists the shorter the window in which they
				 * are invisible. The flag only requests the pass; the spawn
				 * itself still happens on a frame, never inside the poll. */
				gMp.pendingSpawn = 1;
				continue;
			}
		}

		if (MpRemoteTargetsLocalCar(pl->carId, "carstate", pl->id))
		{
			pl->carId = -1;	/* that row's car is not ours to drive from the wire */
			MpPlayClear(pl->id);
			continue;
		}

		cp = &car_data[pl->carId];

		/* The owner changed VEHICLE: match it BEFORE adopting this pose, so the
		 * body we are about to write lands on the right car.
		 *
		 * The wire carries a model NUMBER plus its city (it used to carry our
		 * resident SLOT, which only worked while both machines' resident tables
		 * happened to agree). Resolve it to OUR slot and compare what this car
		 * ACTUALLY renders with -- not pl->car: the roster refresh (every 120
		 * frames) writes the owner's new model into pl->car long before the car
		 * itself is changed, so gating on pl->car silently agreed with the roster
		 * while the car on screen stayed the old one. */
		{
			int wantCity = MpCityFromWire(e.modelCity);
			int haveSlot = cp->ap.model;
			int haveModel = (haveSlot >= 0 && haveSlot < MAX_CAR_RESIDENT_MODELS) ? residentCarModels[haveSlot] : -1;
			int haveCity = (haveSlot >= 0 && haveSlot < MAX_CAR_RESIDENT_MODELS) ? GetCarModelSourceCity(haveSlot) : -2;

			if (haveModel != (int)e.model || haveCity != wantCity)
			{
				/* MP_DEBUG: the wire (model, city) against what we render, with the
				 * slot it resolves to HERE. A mismatch that fires every frame is the
				 * signature of a car this machine cannot hold. Throttled to once a
				 * second, because the repetition IS the signature. */
				if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % 30) == 0)
					gMpCtx->jer_log(gMpCtx,
						"[mp] MODEL: player %d drives %s model %d on the wire; we render slot %d "
						"(model %d, src %d, mesh %s); this machine holds it in slot %d\n",
						pl->id, MpCarCityName(wantCity), (int)e.model, haveSlot, haveModel, haveCity,
						JerCarSlotUsable(haveSlot) ? "present" : "NULL",
						MpResidentSlotForCar(wantCity, (int)e.model));

				if (MpAdoptRemoteCar(pl, wantCity, (int)e.model))
				{
					if (pl->carId < 0 || pl->carId >= MAX_CARS)
						continue;

					/* A new body: its timeline starts here, not at the old car's
					 * last sample. */
					MpPlayClear(pl->id);

					cp = &car_data[pl->carId];
				}
			}
		}

		/* OWNER-AUTHORITATIVE: this car belongs to another machine, so its owner is
		 * the truth. Adopt the WHOLE body every snapshot -- no easing, no tolerance.
		 * A contact is a separate message (MP_HIT: the owner of the MOVING car
		 * pushes its own), so there is no push to leave room for here. */
		{
			/* The NEWEST SAMPLE against the pose the playout is displaying. With
			 * the 3-frame buffer this is expected to be about 100 ms of travel
			 * (tens to a couple of hundred units while moving) -- it is the
			 * buffer's lead, NOT a desync metric any more. A spike well beyond
			 * that means a lost sample, an extrapolation or a teleport.
			 * (|d| is world units, dh the heading error in 1/4096 turns.)
			 * Logged on arrival, NOT gated on our own frame counter: the
			 * peer's snapshot arrives on ITS cadence, which need not line up with
			 * ours -- gating on `frame % 30` silently logged nothing.
			 *
			 * It IS MP_DEBUG-gated, and that matters: this is a sqrt plus a
			 * 10-field format per remote car per snapshot, and a shipped run
			 * (packaged launchers do not set MP_DEBUG) must not pay for it. The
			 * math lives inside the guard so a production frame does none of it. */
			if (MpDebugOn() && gMpCtx != NULL)
			{
				long dx = (long)e.x - (long)cp->hd.where.t[0];
				long dy = (long)e.y - (long)cp->hd.where.t[1];
				long dz = (long)e.z - (long)cp->hd.where.t[2];
				long d2 = dx * dx + dy * dy + dz * dz;
				int dh = ((e.heading - cp->hd.direction + 2048) & 4095) - 2048;

				gMpCtx->jer_log(gMpCtx,
					"[mp] adopt: player %d snap %u |d|=%ld d2=%ld (dx=%ld dy=%ld dz=%ld) dh=%d pal=%d md=%d\n",
					e.playerId, (unsigned)h.frame, (long)sqrt((double)d2), d2, dx, dy, dz, dh, (int)e.palette, (int)e.model);
			}

			/* Adopt in full: the owner is the truth for its own car -- but as a
			 * SAMPLE on the owner's timeline, not as a pose to write now. The
			 * playout buffer smooths the gaps between samples; MpApplyRemoteCarPoses
			 * is the only writer of this car's body. */
			MpPlayPush(pl->id, h.frame, &e);

			/* keep the remote car placed and alive: the engine spools a
			 * non-local player car out of the world, so re-assert it
			 * (controlType included, or the spooler parks it). */
			cp->controlType = CONTROL_TYPE_PLAYER;
		}

		/* Apply owner's siren state so the remote car lights up. The engine
		 * only draws the bar for PURSUER_AI / roadblock / cutscene, so we
		 * force the same call the traffic mirror uses. */
		if (e.flags & MP_CARSTATE_SIREN)
			AddCopCarLight(cp);

		/* Owner-authoritative wheel appearance: a hubcap knocked off on their
		 * machine is missing here too. Only write on a change, so the per-frame
		 * state does not re-apply the palette every tick. */
		pl->hubcaps = (int)(e.hubcaps & 0x0f);

		/* ...and the owner's body-crush multiplier for THIS car. Every machine
		 * simulates it, so the value has to be the owner's, not ours; crumple
		 * applies it to the slot we hold for that player. */
		MpSetCarCrushScale(pl->carId, MpCrushScaleFromHalf(e.crushScale));

		if ((cp->ap.flags & 0x0f) != pl->hubcaps)
		{
			CarApplyHubcapState(pl->carId, pl->hubcaps);

			if (gMpCtx != NULL)
			{
				int m = (cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS) ? cp->ap.model : -1;

				gMpCtx->jer_log(gMpCtx,
					"[mp] hubcaps: player %d -> %d (slot %d, model %d from %s)\n",
					pl->id, pl->hubcaps, pl->carId,
					(m >= 0) ? residentCarModels[m] : -1,
					(m >= 0) ? MpCarCityName(GetCarModelSourceCity(m)) : "?");
			}
		}

		pl->onFoot = 0;			/* driving again: the roster may seat them */
		pl->lastStateFrame = gMp.frame;	/* the fallback gate in MpOnNetInput reads this */

		/* The OWNER is the colour authority: paint its car the colour the owner
		 * sees. cp->ap.palette is exactly what the renderer hands to
		 * DrawCarObject, so this recolours the car on the very next frame.
		 *
		 * A palette index only means something on a car that IS the owner's vehicle,
		 * and this wire carries a resident SLOT, not a (city, model) - so let the
		 * modules answer whether this machine really holds this player's car. A car
		 * that only looks like the owner's gets the palette it can actually support,
		 * instead of colours that belong to a vehicle that is not here */
		if (!pl->isLocal)
			pl->palette = (int)e.palette;	/* the roster (late joiners) names the owner's colour */

		if (!pl->isLocal)
		{
			int pal = (int)e.palette;
			int corrected;
			JER_ARGS_CAR_PEER_DRAW draw;

			memset(&draw, 0, sizeof(draw));
			draw.player = pl->id;
			draw.car = cp;
			draw.model = cp->ap.model;
			draw.sourceCity = GetCarModelSourceCity(cp->ap.model);
			draw.paletteIn = (int)e.palette;
			draw.paletteOut = pal;

			jer_fire(JER_EVENT_CAR_PEER_DRAW, &draw);

			if (draw.handled && draw.paletteOut >= 0)
				pal = draw.paletteOut;

			/* defensive: a palette this model does not have (an older peer, or one
			 * cycled on another car) is drawn in the model's own colours */
			pal = CarSlotClampPalette(cp->ap.model, pal);

			/* Log a correction when the car's palette actually changes, and once per
			 * reported value when it does not (the state stream is continuous, so an
			 * unthrottled line here would repeat every packet). */
			corrected = draw.handled && (pal != (int)e.palette);

			if (corrected || cp->ap.palette != (u_char)pal)
			{
				int fresh = corrected && pl->id >= 0 && pl->id < MP_MAX_PLAYERS &&
					(gPaletteLogged[pl->id] != (signed char)e.palette);

				if (fresh)
					gPaletteLogged[pl->id] = (signed char)e.palette;

				if ((cp->ap.palette != (u_char)pal || fresh) && gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] palette: player %d reports %d, drawn as %d%s\n",
						pl->id, (int)e.palette, pal,
						corrected ? " (corrected: not their car here)" : "");

				cp->ap.palette = (u_char)pal;
			}
		}

		/* Client-side gather: the first time we hear a peer's car, drop our
		 * own car right next to it so both players start together -- each
		 * machine's take-a-ride spawn point can be right across the map.
		 *
		 * NEVER FOR A LIVE JOIN. A joiner arrives where the host already is, so
		 * gathering would put it back on top of them; and because this runs in the
		 * state poll, it used to move the car AFTER MpPlaceSelfTick had read it as
		 * "still on the level start", which made place-self abort instead of moving
		 * it away. A live join is placed by MpPlaceSelfTick alone. */
		if (!MpIsHost() && !gMp.localPlaced && !gMp.liveJoin)
		{
			MP_PLAYER* me = MpLocalPlayer();

			if (me != NULL && me->carId >= 0 && me->carId < MAX_CARS && me->carId != pl->carId)
			{
				CAR_DATA* my = &car_data[me->carId];
				MATRIX m;
				int gy;
				int oldX = my->hd.where.t[0];	/* so a REFUSED gather can put us back */
				int oldY = my->hd.where.t[1];
				int oldZ = my->hd.where.t[2];
				int oldDir = my->hd.direction;
				VECTOR* oldSpoolXZ = MainPlayer.spoolXZ;

				/* OUR x/z (the peer's spot offset along X) -- but the HEIGHT is
				 * resolved from the ground UNDER where we land, never carried from
				 * the peer's y 600 units away. A y taken at one x/z and applied at
				 * another is the single-Y trap (ARCHITECTURE §4): the car ends up
				 * above or below the road and the engine has to drag it down. This
				 * mirrors the engine's own "place a car at a position" recipe
				 * (civ_ai.c: MapHeight at the target minus the suspension offset). */
				my->hd.where.t[0] = e.x + 600;
				my->hd.where.t[1] = 0;
				my->hd.where.t[2] = e.z;
				my->hd.direction = e.heading;

				/* MAKE THE DESTINATION REAL BEFORE WE READ ITS GROUND.
				 *
				 * This gather is a HOP across the map, and the streamer does not follow a
				 * hop: the region we are dropped into is very often not the one that is
				 * resident, and MapHeight answers 0 for ground that is not there
				 * (sdGetCell -> NULL -> 0, dr2roads.c:544). A car placed on that 0 is a
				 * car below the world -- it falls, and the client has "no cells".
				 *
				 * jer_map_spool_to is the SDK's "stream there" call, and this is the same
				 * sequence antfarm and the arena use for their own hops: point the
				 * streamer at the destination first (it follows MainPlayer.spoolXZ), then
				 * ask, and the engine unpacks that region through its own level-start
				 * path -- the 2x2 barrel, its geometry and roadmap and its texture area
				 * together, none of which a bare UnpackRegion would do. */
				{
					static VECTOR sGatherSpool;	/* spoolXZ must outlive this call */
					int gx = my->hd.where.t[0];
					int gz = my->hd.where.t[2];
					int region = jer_map_ready() ? jer_map_region_of(gx, gz) : -1;
					int hasData = (region >= 0) ? jer_map_region_has_data(region) : 0;
					int before = MapHeight((VECTOR*)my->hd.where.t);
					int after = before;
					int resident = (region >= 0) ? jer_map_region_resident(region) : 0;
					int placed = 0;

					if (jer_map_ready() && region >= 0 && hasData)
					{
						if (resident)
						{
							/* Already unpacked and in the barrel: nothing to stream, so do
							 * NOT call the helper. jer_map_spool_to is not free when the
							 * region is resident -- it still runs CheckLoadAreaData +
							 * StartSpooling + UpdateSpool, a synchronous spool pass -- and
							 * calling it on every gather stalled both a pair run and a
							 * 3-seat run (measured). Stream only when the region genuinely
							 * is not in, which is the hop this gather is.
							 *
							 * And be plain about the limit of this test: residency is NOT
							 * the same question as "is there ground under us". A region
							 * can read resident=1 hasData=1 and still give MapHeight 0
							 * (measured on this path), which is why placement is not made
							 * conditional on the height -- on the level's own start that
							 * 0 is correct and the car is fine, so refusing on it would
							 * break the ordinary case. The streamer is what has to be
							 * right, and for a destination that was never streamed this
							 * branch is not taken at all. */
							placed = 1;
						}
						else
						{
							/* THE HOP. A region you are dropped into is never put into a
							 * barrel by the streamer, so the ground there never loads and
							 * MapHeight answers 0: this is the client's "no cells, fell
							 * into the void". jer_map_spool_to is the SDK's "stream there"
							 * call -- the same one antfarm and the arena use for their own
							 * teleports -- and it unpacks the region under
							 * MainPlayer.spoolXZ through the engine's own level-start
							 * path, so the caller must point spoolXZ at the destination
							 * first. That pointer needs to outlive the call, hence the
							 * static. */
							sGatherSpool.vx = gx;
							sGatherSpool.vy = 0;
							sGatherSpool.vz = gz;
							sGatherSpool.pad = 0;
							MainPlayer.spoolXZ = &sGatherSpool;

							/* 0 = there is nothing there to stream (off the map or no
							 * regions), in which case we must not place at all. */
							placed = jer_map_spool_to(gx, gz);
							after = MapHeight((VECTOR*)my->hd.where.t);

							/* And hand the streamer back. Ours pointed at a static, whose
							 * contents cannot follow a moving car, whereas the engine's own
							 * convention is the player car's position vector itself
							 * (cutscene.c:673 does exactly that) -- which is the car we are
							 * moving, so it already follows us. */
							MainPlayer.spoolXZ = oldSpoolXZ;
						}
					}

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx,
							"[mp] gather ground: region %d (resident=%d, hasData=%d) at %d,%d - MapHeight before %d, after %d, %s\n",
							region, resident, hasData, gx, gz, before, after,
							placed ? (resident ? "already streamed, placing" : "streamed, placing")
								: "nothing to stream, NOT placing");

					if (!placed)
					{
						/* NO GROUND, NO PLACE -- and put the car back. The x/z above was
						 * already written, so without this the refusal would leave the car
						 * standing at the destination with no ground under it: the very void
						 * this is here to prevent. It stays where the level put it -- real
						 * ground by construction -- and tries again on the next carstate,
						 * because localPlaced stays 0. */
						my->hd.where.t[0] = oldX;
						my->hd.where.t[1] = oldY;
						my->hd.where.t[2] = oldZ;
						my->hd.direction = oldDir;

						if (gMpCtx != NULL)
							gMpCtx->jer_log(gMpCtx,
								"[mp] gather: there is no ground to stream at %d,%d (region %d, hasData=%d) - staying where the level put us, will retry\n",
								gx, gz, region, hasData);
					}
					else
					{
						gy = after;	/* the height read immediately before placing */
						if (my->ap.carCos != NULL)
							gy -= my->ap.carCos->wheelDisp[0].vy;
						my->hd.where.t[1] = gy;

						/* Rebuild the handling matrix too: setting only t[]/direction left
						 * the collision box at the OLD spot until the engine next
						 * recomputed it -- and this is a teleport, so it matters. */
						_RotMatrixY(&m, (short)e.heading);
						memcpy(my->hd.where.m, m.m, sizeof(my->hd.where.m));

						/* No need to re-point the streamer here: the engine's spoolXZ is
						 * the player car's own position vector, and this IS that car, so
						 * it follows us to the new spot on its own. (Re-pointing it at a
						 * module static -- which is what this used to do -- leaves the
						 * streamer reading a vector that never moves again.) */

						gMp.localPlaced = 1;

						if (gMpCtx)
							gMpCtx->jer_log(gMpCtx,
								"[mp] gathered next to peer at %d,%d,%d (ground under us, not the peer's y)\n",
								my->hd.where.t[0], my->hd.where.t[1], my->hd.where.t[2]);
					}
				}
			}
		}

		/* The host is the hub: pass a CLIENT's car on to the other clients, so every
		 * machine sees every car (each sends only its own).
		 *
		 * OUTSIDE the row loop, and after every `continue`: a NO_CAR row is the
		 * transition a late joiner needs most (it releases the parked body and lets
		 * the stand-in pedestrian spawn), and relaying from inside the loop skipped
		 * exactly that row. It also relayed a multi-row frame once per row. */
	}

	if (MpIsHost() && connIndex >= 0 && !forged)
		MpHostRelay(connIndex, MP_TAG_CARSTATE, 0, p, len);
}

static int MpCreateTrafficMirror(int slot, int city, int model, int palette)
{
	CAR_DATA* cp;
	EXTRA_CIV_DATA civDat;
	LONGVECTOR4 pos;
	int resident;

	if (slot < 0 || slot >= MAX_CARS)
		return 0;

	resident = MpResidentSlotForCar(city, model);

	/* Never point ap.model at a mesh this machine does not have (DrawCar would read
	 * it) -- a car we cannot build is simply left out. */
	if (!JerCarSlotUsable(resident))
		return 0;

	cp = &car_data[slot];

	memset(&civDat, 0, sizeof(civDat));
	civDat.distAlongSegment = -5;

	memset(&pos, 0, sizeof(pos));

	InitCar(cp, 0, &pos, CONTROL_TYPE_CIV_AI, resident, palette, (char*)&civDat);

	/* NOW it is the peer's car, driven by the peer: our AI must not touch it and
	 * nothing of ours may read a pad through it. */
	cp->controlType = CONTROL_TYPE_PLAYER;
	cp->ai.padid = &gMpQuietPad;
	cp->hndType = 0;
	cp->ap.palette = (u_char)CarSlotClampPalette(resident, palette);
	cp->lowDetail = -1;

	gMpPeerTraffic[slot] = 1;

	return 1;
}

/* Drop a mirror. Deliberately NOT PingOutCar: that decrements the civil counters
 * only for a CIV_AI car and REFUSES to remove a non-CIV_AI one while PingOutCivsOnly
 * is set (civ_ai.c), which the engine sets every frame. A mirror is a plain
 * CONTROL_TYPE_PLAYER car of ours, so it is taken out of the world directly: the
 * slot is left reserved against our own spawner, so nothing reuses it. */
static void MpRemoveTrafficMirror(int slot)
{
	if (slot < 0 || slot >= MAX_CARS || !gMpPeerTraffic[slot])
		return;

	car_data[slot].controlType = CONTROL_TYPE_NONE;
	gMpPeerTraffic[slot] = 0;
}

/* Is `slot` already the mirror of (city,model)? */
static int MpTrafficMirrorMatches(int slot, int city, int model)
{
	int resident;

	if (slot < 0 || slot >= MAX_CARS)
		return 0;

	if (!gMpPeerTraffic[slot] || car_data[slot].controlType == CONTROL_TYPE_NONE)
		return 0;

	resident = MpResidentSlotForCar(city, model);

	return resident >= 0 && car_data[slot].ap.model == resident;
}

/* Write a row's whole body onto the mirror, the same way a player car's snapshot
 * is written (hd.direction is an OUTPUT the engine re-derives, so the attitude
 * comes from the quaternion). */
static void MpApplyTrafficPose(CAR_DATA* cp, const MP_TRAFFIC_ENTRY* e)
{
	LONGQUATERNION q;
	MATRIX m;
	int i;

	for (i = 0; i < 4; i++)
		q[i] = e->orient[i];

	for (i = 0; i < 4; i++)
		cp->st.n.orientation[i] = q[i];
	for (i = 0; i < 3; i++)
		cp->st.n.linearVelocity[i] = e->vel[i];

	LongQuaternion2Matrix(&q, &m);
	m.t[0] = e->x;
	m.t[1] = e->y;
	m.t[2] = e->z;
	memcpy(&cp->hd.where, &m, sizeof(m));

	cp->hd.where.t[0] = e->x;
	cp->hd.where.t[1] = e->y;
	cp->hd.where.t[2] = e->z;
	cp->hd.direction = e->heading;
}

/* Damage parity. The owner is the authority, so the mirror copies its zone damage
 * and health. ap.damage[] is exactly what the deformation pass reads --
 * DentCarDirectional deforms from those values alone (its collision point argument
 * is unused) -- so a change is re-dented here and the mesh follows without any
 * vertices crossing the wire. */
static void MpApplyTrafficDamage(CAR_DATA* cp, const MP_TRAFFIC_ENTRY* e)
{
	VECTOR p;
	int z, changed = 0;

	for (z = 0; z < 6; z++)
	{
		if (cp->ap.damage[z] != e->damage[z])
		{
			cp->ap.damage[z] = e->damage[z];
			changed = 1;
		}
	}

	if (cp->totalDamage != e->totalDamage)
	{
		cp->totalDamage = e->totalDamage;
		changed = 1;
	}

	if (changed)
	{
		p.vx = e->x;
		p.vy = e->y;
		p.vz = e->z;
		DentCarDirectional(cp, p);

		if (MpDebugOn() && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] traffic mirror slot %d re-dented (totalDamage %u)\n",
				cp->id, (unsigned)cp->totalDamage);
	}
}

/* Take every mirror out of the world (session teardown). */
void MpTrafficMirrorsClear(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
		MpRemoveTrafficMirror(i);

	memset(gMpTrafficSent, 0, sizeof(gMpTrafficSent));
}

void MpTrafficMirrorAgeTick(void)
{
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (!gMpPeerTraffic[i])
			continue;

		if ((unsigned)(gMp.frame - gMpPeerTrafficSeen[i]) > MP_TRAFFIC_STALE_FRAMES)
			MpRemoveTrafficMirror(i);
	}
}

/* LIGHT AND SOUND A MIRRORED POLICE CAR.
 *
 * A cop car belongs to ONE machine's traffic band; everywhere else it is a pose
 * rewritten from the wire, built as CONTROL_TYPE_PLAYER so no AI touches it. That
 * is why the siren never worked for a cop chasing somebody else: the engine turns
 * a cop's lightbar and siren on in cars.c and gamesnd.c only for
 * CONTROL_TYPE_PURSUER_AI, and a mirror never is one. The owner now says so in
 * the row (MP_TRAFFIC_SIREN), and this is what acts on it -- with the ENGINE's own
 * two calls, so a mirrored cop looks and sounds exactly like a local one instead
 * of like a module's approximation:
 *
 *   AddCopCarLight  -- the same flashing bar the engine adds to a pursuing cop;
 *   Start3DSoundVolPitch -- the siren sample CarHasSiren names for that model,
 *                      played on a voice jer_sound_ensure gives us (which refuses
 *                      rather than starving the engine's own SFX).
 *
 * The sound is re-issued every frame, which is how the engine starts a player's
 * siren (StartPlayerCarSounds): the sample itself loops in the SPU.
 *
 * Nothing here touches controlType. Handing a mirror to the traffic AI is the
 * access-violation class this file already documents (see MpReleaseRemoteCar). */
void MpMirrorSirenTick(void)
{
	int i, playing = 0;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp;
		int siren;

		if (!gMpPeerTraffic[i] || gMpPeerTrafficSiren[i] == 0)
			continue;

		if ((unsigned)(gMp.frame - gMpPeerTrafficSiren[i]) > MP_TRAFFIC_STALE_FRAMES)
			continue;	/* the owner stopped saying it: off, like the pose */

		cp = &car_data[i];

		if (cp->controlType == CONTROL_TYPE_NONE)
			continue;

		AddCopCarLight(cp);

		siren = CarHasSiren(cp->ap.model);

		if (siren == 0)
			continue;

		/* ONE loop at a time: a second siren for the same cop would double the
		 * sample on top of itself. The first mirror found wins, and the engine's
		 * own limit for cop sirens is two anyway. */
		if (playing)
			continue;

		playing = 1;

		/* Say it once a second or so: "did the flag ever arrive, and was it acted
		 * on" is the one question a run of this could not otherwise answer. */
		if (MpDebugOn() && gMpCtx != NULL && (gMp.frame % 30) == 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] siren: mirrored police slot %d is on duty here too (lightbar + siren)\n", i);

		Start3DSoundVolPitch(jer_sound_ensure(&gMpMirrorSirenChannel),
			(siren & 0xff00) >> 8, siren & 0xff,
			cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], -10000, 129);
	}
}

void MpHandleTraffic(int connIndex, const unsigned char* p, int len)
{
	const MP_TRAFFIC* h;
	const MP_TRAFFIC_ENTRY* e;
	int i, n, forged = 0;
	int from = -1, bandFirst = -1, bandLast = -1;

	if (len < (int)sizeof(MP_TRAFFIC))
		return;

	h = (const MP_TRAFFIC*)p;
	n = h->count;

	if (n > MAX_CARS)
		n = MAX_CARS;

	if (len < (int)(sizeof(MP_TRAFFIC) + n * sizeof(MP_TRAFFIC_ENTRY)))
		n = (len - (int)sizeof(MP_TRAFFIC)) / (int)sizeof(MP_TRAFFIC_ENTRY);

	e = (const MP_TRAFFIC_ENTRY*)(p + sizeof(MP_TRAFFIC));

	/* A client may only speak for the slots of its OWN band (MpSendTrafficState
	 * publishes nothing else): otherwise one peer could despawn or InitCar over
	 * the host's or another peer's traffic. The band is computed the way the
	 * sender computes it, with at least from+1 machines. Clients trust the host. */
	if (MpIsHost() && connIndex >= 0)
	{
		int machines = MpTrafficMachineCount();

		from = MpConnPlayerId(connIndex);

		if (machines < from + 1)
			machines = from + 1;

		if (from >= 0)
			MpTrafficBandFor(from, machines, &bandFirst, &bandLast);
	}

	for (i = 0; i < n; i++)
	{
		int slot = e[i].carSlot;
		/* The wire spells "the level's own city" as MP_CAR_CITY_SESSION; the resolver
		 * wants -1 for it (see MpHandleCarState). */
		int wantCity = MpCityFromWire(e[i].modelCity);

		if (slot < 0 || slot >= MAX_CARS)
			continue;

		if (MpIsHost() && connIndex >= 0 && (bandFirst < 0 || slot < bandFirst || slot > bandLast))
		{
			forged = 1;
			continue;
		}

		if (MpRemoteTargetsLocalCar(slot, "traffic", -1))
			continue;

		if (e[i].flags & MP_TRAFFIC_REMOVE)
		{
			MpRemoveTrafficMirror(slot);
			continue;
		}

		if (!MpTrafficMirrorMatches(slot, wantCity, e[i].model))
		{
			MpRemoveTrafficMirror(slot);

			if (!MpCreateTrafficMirror(slot, wantCity, e[i].model, e[i].palette))
				continue;	/* cannot build their car here: leave the slot empty */
		}

		MpApplyTrafficPose(&car_data[slot], &e[i]);
		MpApplyTrafficDamage(&car_data[slot], &e[i]);
		gMpPeerTrafficSeen[slot] = gMp.frame;	/* seen now: do not age it out */

		/* Its owner says the siren and lightbar are up. Stamped with the frame so a
		 * cop that stops pursuing (or whose owner stops speaking) goes quiet on its
		 * own, exactly like the pose does. */
		gMpPeerTrafficSiren[slot] = (e[i].flags & MP_TRAFFIC_SIREN) ? gMp.frame : 0;
	}

	/* The host is the hub: relay a client's traffic to the other clients, exactly
	 * as it relays their carstate. */
	if (MpIsHost() && connIndex >= 0 && !forged)
		MpHostRelay(connIndex, MP_TAG_TRAFFIC, 0, p, len);
}
