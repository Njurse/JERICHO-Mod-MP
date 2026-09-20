/* ------------------------------------------------------------------ */
/* The multiplayer TEST BOT.                                            */
/*                                                                      */
/* A testing component: it drives a real player's car so two instances  */
/* can be exercised without two humans. NOTHING in the mod's session,   */
/* networking or gameplay path may depend on it, and it is inert unless */
/* MP_BOT asks for it (see mp_bot.h).                                   */
/* ------------------------------------------------------------------ */
#include "jericho.h"
#include "jer_events.h"

#include "driver2.h"
#include "cars.h"
#include "pad.h"
#include "objcoll.h"	/* CellEmpty: the engine's own scenery test */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mp.h"
#include "mp_bot.h"

/* The canned manoeuvre: a pad that changes every 0.5-4 s (steer left/right,
 * reverse, wheelspin, handbrake). Because the local car is what we replicate,
 * a peer seeing it move proves the controls arrived. */
static int MpBotCanned(void)
{
	static unsigned long nextChange;
	static int cur;

	unsigned long now = MpNowMs();

	if (now >= nextChange)
	{
		unsigned int r = (unsigned int)now ^ (unsigned int)(now >> 7);
		const char* name;

		switch (r % 6)
		{
			case 0:  cur = CAR_PAD_ACCEL | CAR_PAD_LEFT;      name = "accel+left";      break;
			case 1:  cur = CAR_PAD_ACCEL | CAR_PAD_RIGHT;     name = "accel+right";     break;
			case 2:  cur = CAR_PAD_ACCEL | CAR_PAD_WHEELSPIN; name = "accel+wheelspin"; break;
			case 3:  cur = CAR_PAD_BRAKE;                     name = "reverse";         break;
			case 4:  cur = CAR_PAD_ACCEL;                     name = "accel";           break;
			default: cur = CAR_PAD_ACCEL | CAR_PAD_HANDBRAKE; name = "accel+handbrake"; break;
		}

		nextChange = now + 500 + (r % 3500);	/* 0.5 - 4 s */

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] bot: %s (pad=%#x)\n", name, cur);
	}

	return cur;
}

/* Which bot, if any. OFF unless MP_BOT says otherwise -- these levers drive a
 * real player's car, so nothing here may be on by default. Returns 0 (none),
 * 1 (random), 2 (chase), 3 (fight) or 4 (pursuit: host hunts, joiner runs). */
static int MpBotMode(void)
{
	const char* m = getenv("MP_BOT");

	if (m == NULL)
		return (getenv("MP_TESTDRIVE") != NULL) ? 1 : 0;

	if (strcmp(m, "chase") == 0)
		return 2;
	if (strcmp(m, "fight") == 0)
		return 3;
	if (strcmp(m, "pursuit") == 0)
		return 4;
	if (strcmp(m, "off") == 0 || strcmp(m, "0") == 0)
		return 0;

	return 1;
}

/* chase/fight: drive the LOCAL car at (or, for the fleeing host, away from) the
 * nearest other player's car. Two instances then close the gap on their own,
 * which is how car-to-car collision and the two players meeting each other get
 * exercised without two humans. Returns 0 (coast) when there is nobody else. */
static int MpBotChase(int fight)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* mine;
	CAR_DATA* tgt = NULL;
	static int stuckFrames, recoverFrames, recoverDir, recoverReverse;
	int k;

	if (me == NULL || me->carId < 0)
		return 0;

	for (k = 0; k < MP_MAX_PLAYERS; k++)
	{
		MP_PLAYER* p = &gMp.players[k];

		if (p->active && p->carId >= 0 && p->carId != me->carId)
		{
			tgt = &car_data[p->carId];
			break;
		}
	}

	if (tgt == NULL)
		return 0;

	mine = &car_data[me->carId];

	/* The very primitive "pathfinder": a straight line at the peer is enough on
	 * an open map, but the cars wedge on the first building and never meet again.
	 * If the car is not moving, back out and turn (the only thing that frees a
	 * car off a wall) and on alternate attempts swing round on the wheel. NO
	 * WHEELSPIN: spinning the wheels is a grip loss, which is what made the cars
	 * bobble and slide into the scenery. */
	if (recoverFrames > 0)
	{
		recoverFrames--;

		if (recoverReverse)
			return CAR_PAD_BRAKE | (recoverDir ? CAR_PAD_LEFT : CAR_PAD_RIGHT);

		return (recoverDir ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	{
		int dx = tgt->hd.where.t[0] - mine->hd.where.t[0];
		int dz = tgt->hd.where.t[2] - mine->hd.where.t[2];
		int flee = (!fight && MpIsHost());	/* chase: the host runs, the joiner chases. fight: both charge. */
		int want = flee ? ((ratan2(dx, dz) + 2048) & 0xfff) : (ratan2(dx, dz) & 0xfff);
		int diff, adiff;
		long dist;
		int pad;

		/* Scenery awareness, using the engine's OWN test: CellEmpty is what the
		 * civ AI uses to know a spot is clear. Probe ALONG THE CAR'S VELOCITY
		 * VECTOR (falling back to where we want to go when parked) and, if that
		 * spot has scenery in it, take the first side heading whose probe is
		 * clear. This is the awareness a straight line at the peer never had -- it
		 * drove into the first building or tree between them. */
		{
			static const int RPROBE = 1100;
			int pdir = want;
			int vx = mine->st.n.linearVelocity[0];
			int vz = mine->st.n.linearVelocity[2];

			if ((long)vx * (long)vx + (long)vz * (long)vz > 400L * 400L)
				pdir = ratan2(vx, vz) & 0xfff;

			{
				const int probe[3] = { pdir, (pdir + 448) & 0xfff, (pdir - 448) & 0xfff };
				VECTOR p;
				int i;

				p.vx = mine->hd.where.t[0] + (int)(((long)rsin(probe[0]) * RPROBE) >> 12);
				p.vy = mine->hd.where.t[1];
				p.vz = mine->hd.where.t[2] + (int)(((long)rcos(probe[0]) * RPROBE) >> 12);

				if (!CellEmpty(&p, 350))
				{
					for (i = 1; i < 3; i++)
					{
						p.vx = mine->hd.where.t[0] + (int)(((long)rsin(probe[i]) * RPROBE) >> 12);
						p.vy = mine->hd.where.t[1];
						p.vz = mine->hd.where.t[2] + (int)(((long)rcos(probe[i]) * RPROBE) >> 12);

						if (CellEmpty(&p, 350))
						{
							want = probe[i];
							break;
						}
					}
				}
			}
		}

		diff = ((want - mine->hd.direction + 2048) & 4095) - 2048;	/* DIFF_ANGLES */
		adiff = (diff < 0) ? -diff : diff;

		{
			int spd = mine->hd.speed;

			if (spd < 0)
				spd = -spd;

			/* Wedged counts whether we are trying to go forward or just sitting
			 * there facing away: a car stopped against a wall with the peer behind
			 * it never moves, and a throttle-only test missed exactly that. */
			if (spd < 4)
			{
				if (++stuckFrames > 100)
				{
					recoverFrames = recoverReverse ? 45 : 70;
					recoverDir ^= 1;
					recoverReverse ^= 1;
					stuckFrames = 0;

					if (gMpCtx != NULL)
						gMpCtx->jer_log(gMpCtx, "[mp] chase: stuck, %s (dir %d)\n",
							recoverReverse ? "backing out" : "spinning round", recoverDir);
				}
			}
			else
			{
				stuckFrames = 0;
			}
		}

		dist = (long)dx * (long)dx + (long)dz * (long)dz;

		if (!flee && dist < 900L * 900L)
		{
			/* The chaser is on top of the runner: coast to a stop and just face
			 * them, so the pair does not ram itself out of sight. */
			pad = (adiff > 96) ? ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT) : 0;
		}
		else if (adiff > 1500)
			/* The peer is behind us: reversing round is the only coherent way
			 * about, and the one case where reverse is right. */
			pad = CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
		else if (adiff > 700)
			/* Badly off line: EASE OFF and steer. Powering through a big
			 * correction is what made them bobble and slide into the scenery. */
			pad = (diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT;
		else if (adiff > 120)
			pad = CAR_PAD_ACCEL | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
		else
			pad = CAR_PAD_ACCEL;

		if ((gMp.frame % 60) == 0 && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] chase: %s d=%d,%d want=%d dir=%d diff=%d pad=%#x stuck=%d\n",
				flee ? "flee" : "chase", dx, dz, want, mine->hd.direction, diff, pad, stuckFrames);

		return pad;
	}
}

/* ------------------------------------------------------------------ */
/* pursuit / evade: the chase test                                      */
/*                                                                      */
/* The JOINER hunts and the HOST runs -- the reverse of `chase`. Neither */
/* side is ever allowed to idle: a chase is CONSTANT action.            */
/* ------------------------------------------------------------------ */
#define MPBOT_PROBE	2400	/* how far ahead the pathfinder looks -- long, so it sees a
				 * wall at an intersection and commits to the turn BEFORE
				 * reaching it, instead of nosing into it */
#define MPBOT_NEAR	650	/* ... and the near probe that stops it nosing into a wall */

/* Is a heading's path clear at both the near and the far probe? */
static int MpBotHeadingClear(CAR_DATA* mine, int a)
{
	VECTOR p;

	p.vx = mine->hd.where.t[0] + (int)(((long)rsin(a) * MPBOT_NEAR) >> 12);
	p.vy = mine->hd.where.t[1];
	p.vz = mine->hd.where.t[2] + (int)(((long)rcos(a) * MPBOT_NEAR) >> 12);
	if (!CellEmpty(&p, 350))
		return 0;

	p.vx = mine->hd.where.t[0] + (int)(((long)rsin(a) * MPBOT_PROBE) >> 12);
	p.vz = mine->hd.where.t[2] + (int)(((long)rcos(a) * MPBOT_PROBE) >> 12);

	return CellEmpty(&p, 350);
}

static int MpBotClearHeading(CAR_DATA* mine, int desired)
{
	const int STEP = 0x1000 / 12;	/* 30 degrees */
	/* HYSTERESIS. Deciding the heading from scratch every frame made the steering
	 * FLAP: as `desired` drifted, the first clear candidate flipped between +30
	 * and -30 and the car twitched left/right. Once we have a heading, KEEP it
	 * while it is still clear and still points roughly the way we want; re-decide
	 * only when it is blocked or the goal has moved a long way off it. */
	static int held = -1;
	int i;

	if (held >= 0 && MpBotHeadingClear(mine, held))
	{
		int off = ((held - desired + 2048) & 4095) - 2048;

		if (off < 0)
			off = -off;

		if (off < 900)		/* still within ~80 degrees of the goal */
			return held;
	}

	for (i = 0; i < 12; i++)
	{
		int k = (i + 1) / 2;
		int a = (i == 0) ? desired : ((desired + ((i & 1) ? (k * STEP) : (-k * STEP))) & 0xfff);

		if (MpBotHeadingClear(mine, a))
		{
			held = a;
			return a;
		}
	}

	return desired;
}

/* Returns 0 (coast) when there is nobody else to chase. */
static int MpBotPursuit(void)
{
	MP_PLAYER* me = MpLocalPlayer();
	CAR_DATA* mine;
	CAR_DATA* tgt = NULL;
	static int stuckFrames, recoverFrames, recoverDir, recoverReverse;
	int k;

	if (me == NULL || me->carId < 0)
		return 0;

	for (k = 0; k < MP_MAX_PLAYERS; k++)
	{
		MP_PLAYER* p = &gMp.players[k];

		if (p->active && p->carId >= 0 && p->carId != me->carId)
		{
			tgt = &car_data[p->carId];
			break;
		}
	}

	if (tgt == NULL)
		return 0;

	mine = &car_data[me->carId];

	if (recoverFrames > 0)
	{
		recoverFrames--;

		if (recoverReverse)
			return CAR_PAD_BRAKE | (recoverDir ? CAR_PAD_LEFT : CAR_PAD_RIGHT);

		return (recoverDir ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	{
		int dx = tgt->hd.where.t[0] - mine->hd.where.t[0];
		int dz = tgt->hd.where.t[2] - mine->hd.where.t[2];
		int evade = MpIsHost();	/* the JOINER hunts, the HOST runs */
		int desired = evade ? ((ratan2(dx, dz) + 2048) & 0xfff) : (ratan2(dx, dz) & 0xfff);
		int want = MpBotClearHeading(mine, desired);
		int diff = ((want - mine->hd.direction + 2048) & 4095) - 2048;
		int adiff = (diff < 0) ? -diff : diff;
		long dist = (long)dx * (long)dx + (long)dz * (long)dz;
		int spd = mine->hd.speed;
		int pad;

		if (spd < 0)
			spd = -spd;

		/* same wedge detection as chase: a car stopped against scenery has to be
		 * backed out; the runner wedges at least as often as the pursuer */
		/* Constant action: a wedge is cleared in a fraction of a second, not after
		 * seconds of sitting still. A stopped car is a wasted frame of the test. */
		if (spd < 3)
		{
			if (++stuckFrames > 40)
			{
				recoverFrames = recoverReverse ? 18 : 26;
				recoverDir ^= 1;
				recoverReverse ^= 1;
				stuckFrames = 0;

				if (gMpCtx != NULL)
					gMpCtx->jer_log(gMpCtx, "[mp] bot: %s stuck, %s\n",
						evade ? "evade" : "pursue", recoverReverse ? "backing out" : "turning round");
			}
		}
		else
		{
			stuckFrames = 0;
		}

		/* Speed on the straights, DECISIVE turns in the corners. Powering into a big
		 * heading error is what scrapes the car along the wall: it understeers,
		 * nose-first, for as long as the turn takes. So a large error BRAKES into the
		 * turn (the move that replaces scraping along the wall), a medium one coasts
		 * through it, and only a roughly aligned car gets full throttle. */
		if (adiff > 1200)
			/* a real U-turn: slow right down and turn hard */
			pad = CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
		else if (adiff > 420)
			/* a corner: lift and steer; the momentum carries it round */
			pad = (diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT;
		else if (adiff > 70)
			pad = CAR_PAD_ACCEL | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
		else
			pad = CAR_PAD_ACCEL;

		/* the pursuer keeps the power on when it is closing, so the collision is
		 * actually tested; the runner never relents either */
		if (!evade && dist < 400L * 400L)
			pad |= CAR_PAD_ACCEL;

		if ((gMp.frame % 60) == 0 && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] bot: %s d=%d,%d want=%d dir=%d diff=%d pad=%#x stuck=%d\n",
				evade ? "evade" : "pursue", dx, dz, want, mine->hd.direction, diff, pad, stuckFrames);

		return pad;
	}
}

int MpBotEnabled(void)
{
	return MpBotMode() != 0;
}

int MpBotPadForLocalCar(void)
{
	switch (MpBotMode())
	{
	case 4:  return MpBotPursuit();
	case 3:  return MpBotChase(1);
	case 2:  return MpBotChase(0);
	case 1:  return MpBotCanned();
	default: return 0;
	}
}
