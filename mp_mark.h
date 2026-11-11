#ifndef MP_MARK_H
#define MP_MARK_H

/*
 * mp_mark.h -- the marked-player referee: Fox N Hounds and Car Bomb Tag (MP_TAG_MARK).
 *
 * ONE player is MARKED -- the fox, or whoever holds the bomb -- and clocks run:
 *   - MP_MODE_TIMER_PERSONAL (Fox N Hounds): every player has their own clock, set
 *     to the host's time at the start of each round. Only the fox's clock ticks; a
 *     hound's is frozen. The first player whose clock reaches 0:00 wins.
 *   - MP_MODE_TIMER_SHARED (Car Bomb Tag): one clock, the same setting, ticking for
 *     the whole round. Whoever holds the bomb when it reaches 0:00 loses.
 * A TAG is a contact between the marked player's car and another player's: the mark
 * moves to the other player, and for MP_MARK_TAG_COOLDOWN_MS nothing can move it
 * again, so one bump cannot bounce it straight back.
 *
 * Lifecycle, on the HOST (a listen host here, dedi/markref.py on a dedicated server),
 * when the mode opts into the pre-game framework (mp_mode.h: waitMinPlayers,
 * waitForCars, firstCountdownSec, roundCountdownSec):
 * after a MAP LOAD (MpMarkReset -- the host's START -- or the present-set emptying,
 * as a dedicated server's map vote does) the referee sits in WAITING until the
 * mode's wait is satisfied (>= waitMinPlayers, and if waitForCars every present
 * player has picked a car). Then one COUNTDOWN: firstCountdownSec as N..1 then GO
 * on a fresh map; later rounds on the same map use a one-second GET READY (the
 * start cluster) plus roundCountdownSec as N..1 then GO. A joiner without a car,
 * or a player leaving, during COUNTDOWN goes back to WAITING (the first-load
 * count is retried if GO has not happened yet). Then LIVE with a randomly chosen
 * marked player (GO); ENDED on a win (or when fewer than two are left), and after
 * MP_MARK_INTERMISSION_MS the same wait-then-countdown gate again. Nothing ticks
 * and nothing tags outside LIVE. Take a Ride leaves the four fields at 0 and this
 * referee stays inactive (no JPMK). MP_MARK_PHASE_LOBBY (4) is retired: the wire
 * value is unused, proto 23 is unchanged. The caller drives the round carrier
 * (mp_round.c) from the events these return.
 *
 * On a CLIENT this is a mirror: each 'JPMK' snapshot replaces it whole.
 *
 * Pure on purpose, like mp_round.c: no engine headers, no session state, no clock of
 * its own (the caller passes elapsed milliseconds) and no randomness of its own (the
 * caller passes a random number), so tools/mode_check/test_mp_mark.cpp and the
 * Python twin can be run against each other step by step.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define MP_MARK_TAG_COOLDOWN_MS	2000	/* after a transfer (and a go-live), no tag */
#define MP_MARK_INTERMISSION_MS	10000	/* ENDED -> the next round */
#define MP_MARK_COUNTDOWN_MS	4000	/* COUNTDOWN -> LIVE: GET READY (the cluster
					 * placement), then 3, 2, 1 -- a second each */
#define MP_MARK_COUNTDOWN_SPAWN_MS	1000	/* the first slice: placement, no digit yet */
#define MP_MARK_LOBBY_MS	5000	/* default firstCountdownSec (5-4-3-2-1), in ms.
					 * The phase MP_MARK_PHASE_LOBBY is retired;
					 * the first-load count runs as COUNTDOWN. */

/* Cops n Robbers (MP_MODE_TIMER_GETAWAY). The host's caller measures; these
 * decide. A HIT is a contact between the getaway and a cop that the host
 * accepted (the same contacts Fox N Hounds tags with); its strength is the
 * contact's push in whole units/frame (mp_session.c MpContactPush: 60% of the
 * closing speed, capped at 20; dedi: the same from two carstates). */
#define MP_MARK_CNR_HIT_COOLDOWN_MS	1000	/* per cop: one scored hit a second */
#define MP_MARK_CNR_HIT_MAX		20	/* strength cap (= MP_HIT_MAX_PUSH) */
#define MP_MARK_CNR_HIT_POINTS		5	/* points per unit of strength: 5..100 a hit */
#define MP_MARK_CNR_BUST_POINTS		1000	/* the cops' damage meter: full = busted */
#define MP_MARK_CNR_DRIVE_BASE		10	/* the getaway's points per second alive */
#define MP_MARK_CNR_DRIVE_BONUS		5	/* + up to this many per second at speed */
#define MP_MARK_CNR_DRIVE_TOPSPEED	300	/* whole units/frame (~90 mph) for the full bonus */
#define MP_MARK_CNR_ESCAPE_DIST		32000	/* x/z units from EVERY cop (~2x draw distance) */
#define MP_MARK_CNR_ESCAPE_HOLD_MS	5000	/* ...held this long (the host's caller) */
/* The meter follows the getaway's REAL damage: its carstate rows carry its
 * totalDamage and wreck point (v24; 0.23.4 sent them as the optional JPDM). Until
 * the first row of a round arrives the contact strength above scores. The meter
 * is damage / maxDamage of BUST_POINTS, so a full meter is a fully wrecked car;
 * the points go to the last cop that touched the getaway within CREDIT_MS (none:
 * the meter still fills, nobody scores). */
#define MP_MARK_CNR_CREDIT_MS		2000	/* a cop's contact earns the damage that follows it */

/* Car Bomb Tag (0.23.4): the holder left and two or more are still in -- a random
 * new holder, who cannot pass it on, and whose bomb cannot go off, for this long
 * (the shared clock is raised to it if less is left). */
#define MP_MARK_BOMB_GRACE_MS		12000

/* Event bits returned by MpMarkSync / MpMarkTick. */
#define MP_MARK_EV_BEGIN	0x01	/* a round went live: MpRoundBegin */
#define MP_MARK_EV_END		0x02	/* the round ended: MpRoundEnd */
#define MP_MARK_EV_MOVED	0x04	/* the marked player changed (not a go-live) */
#define MP_MARK_EV_COUNTDOWN	0x08	/* the wait is satisfied: a countdown started */
#define MP_MARK_EV_LOBBY	0x10	/* retired: never set (was the separate 5-count phase) */

int  MpMarkClampMinutes(int minutes);	/* MP_MARK_MINUTES_MIN..MAX */

/* ---- the host ------------------------------------------------------------- */
/* A new session in `mode`: WAITING, nobody present, nothing ticking. A mode with
 * no timer (Take a Ride) is accepted and simply never does anything. */
void MpMarkReset(int mode, int minutes);

/* Present-set = `present[id]` (MP_MAX_PLAYERS entries, nonzero = in the session).
 * An arrival gets a full, frozen clock (a hound / no bomb); if the marked player
 * leaves a live round, `rnd` picks the next one. Returns MP_MARK_EV_* bits. */
int  MpMarkSync(const int* present, unsigned rnd);

/* Ready-set = `ready[id]` (MP_MAX_PLAYERS entries, nonzero = that player has
 * picked a car). Only a present player's flag matters; a player is NOT ready until
 * the caller says so. Rides the snapshot (MP_MARK_ENTRY_F_READY) so every machine
 * can show who the round is waiting for. */
void MpMarkReady(const int* ready);

/* Advance by `ms`. `rnd` picks the marked player when a round goes live. */
int  MpMarkTick(int ms, unsigned rnd);

/* A contact between players `a` and `b`, accepted by the caller as real. Returns 1
 * when it moved the mark (one of them was marked, the round is live and the
 * cooldown is over), 0 when it was not a tag. */
int  MpMarkContact(int a, int b);

/* Cops n Robbers, host. A hit between players `a` and `b` (one the getaway, one a
 * cop) of `strength` (above): returns the points the COP earned (0 = not a scored
 * hit: not live, two cops, the cop's cooldown) and adds them to the heat meter;
 * a full meter ends the round, busted (*outEv = MP_MARK_EV_END, winner none). */
int  MpMarkHit(int a, int b, int strength, int* outEv);

/* Cops n Robbers, host: the getaway `from` reports its own car's damage (its
 * totalDamage out of `maxDamage`: its carstate row, or the host's own car). From the
 * first report of a round the contacts above stop scoring and only mark who
 * touched the getaway; the meter moves by the damage it gained since the last
 * report, scaled to BUST_POINTS per maxDamage, and fills when the car is wrecked.
 * Returns the points the meter rose by; *outCop = the cop credited (-1 = none,
 * MP_MARK_CNR_CREDIT_MS); *outEv = MP_MARK_EV_END on a bust. 0 for a report that
 * is not the live getaway's, or one that did not add damage. */
int  MpMarkDamage(int from, int damage, int maxDamage, int* outCop, int* outEv);
int  MpMarkDamageLive(void);		/* host: this round's meter follows reported damage */

/* ROUND LIFECYCLE v2 (proto 24), host. A player who arrives while a round is LIVE
 * in a mode whose late joiners wait (MP_MODE_LATEJOIN_SPECTATE) is present but
 * BENCHED (MP_MARK_ENTRY_F_WAITING): not marked, not tagged, not counted as "in
 * the round", until the next COUNTDOWN (or GO) puts everyone in. MpMarkOut benches
 * a player who died in a mode with no respawn (MP_MODE_RESPAWN_NONE,
 * MP_MARK_ENTRY_F_OUT): if they were marked it is handled like the marked player
 * leaving (Cops n Robbers: busted). Returns MP_MARK_EV_* bits; 0 when not LIVE or
 * the player is not in the round. `rnd` picks a new marked player. */
int  MpMarkOut(int playerId, unsigned rnd);

/* Cops n Robbers, host: `ms` more alive at `speed` whole units/frame. Returns the
 * whole points the GETAWAY earned (MP_MARK_CNR_DRIVE_BASE a second, plus up to
 * MP_MARK_CNR_DRIVE_BONUS a second scaled by speed); the remainder carries. */
int  MpMarkDrive(int ms, int speed);

/* Cops n Robbers, host: the getaway is clear of every cop (the caller held
 * MP_MARK_CNR_ESCAPE_DIST for MP_MARK_CNR_ESCAPE_HOLD_MS). Ends the round with
 * the getaway as the winner: MP_MARK_EV_END, or 0 when not live. */
int  MpMarkEscape(void);

int  MpMarkBuild(unsigned char* out, int cap);	/* bytes written, 0 = too small */
unsigned MpMarkRevision(void);		/* bumped on every change except a tick */

/* ---- the client ------------------------------------------------------------ */
int  MpMarkApply(const unsigned char* p, int len);	/* 1 = taken */

/* ---- what anyone may ask --------------------------------------------------- */
int  MpMarkHaveState(void);
int  MpMarkMode(void);
int  MpMarkPhase(void);			/* MP_MARK_PHASE_* */
int  MpMarkMarked(void);		/* player id, -1 = nobody */
int  MpMarkWinner(void);		/* player id, -1 = nobody */
int  MpMarkMinutes(void);
unsigned MpMarkSeq(void);
int  MpMarkPresent(int playerId);
int  MpMarkPlayerReady(int playerId);	/* present and has picked a car */
int  MpMarkCountWaiting(int* outReady);	/* present players; *outReady = how many have a car */
int  MpMarkHeat(void);			/* Cops n Robbers: the damage meter, 0..BUST; else 0 */
int  MpMarkEndReason(void);		/* MP_MARK_END_*: why the round in ENDED ended */
int  MpMarkFlags(void);			/* MP_MARK_F_* */
int  MpMarkInRound(int playerId);	/* present and not benched (WAITING / OUT) */
int  MpMarkBench(int playerId);		/* 0, MP_MARK_ENTRY_F_WAITING or MP_MARK_ENTRY_F_OUT */

/* COUNTDOWN: ms until GO; 0 otherwise. `sinceMs` is run on like MpMarkClockMs. */
int  MpMarkCountdownMs(int sinceMs);
int  MpMarkCountdownTotalMs(void);	/* the length of the current count, 0 if none */
int  MpMarkCountdownIsFirst(void);	/* 1 = the fresh-map N..1, not GET READY + N..1 */

/* The clock to SHOW, in ms: `playerId`'s own (MP_MODE_TIMER_PERSONAL) or the shared
 * one (playerId < 0). `sinceMs` is how long ago the state was applied: a client
 * runs the ticking clock on between snapshots (the host passes 0). Never below 0. */
int  MpMarkClockMs(int playerId, int sinceMs);

#ifdef __cplusplus
}
#endif

#endif /* MP_MARK_H */
