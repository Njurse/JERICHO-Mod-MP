#include "jer_carexplode.h"

#include "driver2.h"
#include "cars.h"
#include "denting.h"
#include "mission.h"	/* MaxPlayerDamage */
#include "job_fx.h"
#include "gamesnd.h"	/* SOUND_BANK_SFX, SBK_ID_MISSION_23 */
#include "sound.h"
#include "jer_sound.h"
#include "convert.h"	/* Random2: the stock bang-pitch jitter */
#include "main.h"	/* FrameCnt */
#include "glaunch.h"	/* NoPlayerControl: skip blasts/sound in a replay */

/* CC wreck impulses (cainescrossfirewreckfx.c). Copied, not linked. */
#define JER_CAREXPLODE_TUMBLE_LAUNCH	(0x18000 * 5)
#define JER_CAREXPLODE_TUMBLE_SPIN	(0x100000 * 50)

#define JER_CAREXPLODE_TICK	0
#define JER_CAREXPLODE_BANG	1
#define JER_CAREXPLODE_FALLBACK_TICK	1	/* smashable thud, always in SFX */
#define JER_CAREXPLODE_FALLBACK_BANG	6	/* heaviest crash, always in SFX */

static int gLumpSlot = -1;
static unsigned char gLatched[MAX_CARS];

void jer_carexplode_request_banks(void)
{
	if (gLumpSlot < 0)
		gLumpSlot = jer_sound_request_lump(SOUND_BANK_SFX, SBK_ID_MISSION_23, 2);
}

int jer_carexplode_bank(void)
{
	return SOUND_BANK_SFX;
}

static int JerCarExplodeBase(void)
{
	int base;

	if (gLumpSlot < 0)
		return -1;

	base = jer_sound_lump_base(gLumpSlot);
	if (base < 0 || jer_sound_lump_count(gLumpSlot) <= 0)
		return -1;

	return base;
}

int jer_carexplode_tick_sample(void)
{
	int base = JerCarExplodeBase();

	if (base >= 0 && jer_sound_lump_count(gLumpSlot) >= 1)
		return base + JER_CAREXPLODE_TICK;

	return JER_CAREXPLODE_FALLBACK_TICK;
}

int jer_carexplode_bang_sample(void)
{
	int base = JerCarExplodeBase();

	if (base >= 0 && jer_sound_lump_count(gLumpSlot) >= 2)
		return base + JER_CAREXPLODE_BANG;

	return JER_CAREXPLODE_FALLBACK_BANG;
}

int jer_carexplode_samples_loaded(void)
{
	return (JerCarExplodeBase() >= 0 && jer_sound_lump_count(gLumpSlot) >= 2) ? 1 : 0;
}

static void JerCarExplodeLock(CAR_DATA* cp)
{
	cp->thrust = 0;
	cp->wheel_angle = 0;
	cp->handbrake = 1;
	cp->wheelspin = 0;
}

/* After AddExplosion returns, force the new slot to the wreck profile
 * (collide 0, slightly larger than stock BIG_BANG). Matching on time==0
 * and the position we just passed, so a JER_EVENT_EXPLOSION_SPAWN hook
 * (Caine's Crossfire, if loaded in a dev tree) cannot leave collide on. */
static void JerCarExplodeQuiet(const VECTOR* pos)
{
	int i;

	for (i = 0; i < MAX_EXPLOSION_OBJECTS; i++)
	{
		if (explosion[i].time != 0)
			continue;

		if (explosion[i].pos.vx != pos->vx ||
		    explosion[i].pos.vy != pos->vy ||
		    explosion[i].pos.vz != pos->vz)
			continue;

		explosion[i].collide = 0;
		explosion[i].hscale = 6000;
		explosion[i].rscale = 6000;
		explosion[i].speed = 90;
		return;
	}
}

static void JerCarExplodeBlast(CAR_DATA* cp, int ox, int oz)
{
	VECTOR pos;

	pos.vx = cp->hd.where.t[0] + ox;
	pos.vy = cp->hd.where.t[1];
	pos.vz = cp->hd.where.t[2] + oz;
	pos.pad = 0;

	AddExplosion(pos, BIG_BANG);
	JerCarExplodeQuiet(&pos);
}

void jer_carexplode(void* car)
{
	CAR_DATA* cp = (CAR_DATA*)car;
	int id, i, pitch;
	VECTOR at;

	if (cp == NULL)
		return;

	id = cp->id;
	if (id < 0 || id >= MAX_CARS || gLatched[id])
		return;

	gLatched[id] = 1;

	for (i = 0; i < 6; i++)
		cp->ap.damage[i] = 0xfff;

	cp->ap.needsDenting = 1;
	cp->totalDamage = MaxPlayerDamage[0];
	DentCar(cp);

	cp->st.n.linearVelocity[1] += JER_CAREXPLODE_TUMBLE_LAUNCH;
	cp->st.n.angularVelocity[0] += (Random2(1) & 1) ? JER_CAREXPLODE_TUMBLE_SPIN
							: -JER_CAREXPLODE_TUMBLE_SPIN;
	cp->st.n.angularVelocity[2] += (Random2(2) & 1) ? JER_CAREXPLODE_TUMBLE_SPIN
							: -JER_CAREXPLODE_TUMBLE_SPIN;

	JerCarExplodeLock(cp);

	/* dedi / a replay: the dent and the tumble still happen, the blasts do not */
	if (NoPlayerControl)
		return;

	/* three slightly offset blasts, same spacing idea as the planted bombs
	 * in bomberman.c, all visual-only */
	JerCarExplodeBlast(cp, 170, 73);
	JerCarExplodeBlast(cp, -109, -147);
	JerCarExplodeBlast(cp, 0, 0);

	at.vx = cp->hd.where.t[0];
	at.vy = cp->hd.where.t[1];
	at.vz = cp->hd.where.t[2];
	pitch = ((FrameCnt * at.vx ^ Random2(4) * at.vz) & 0x3ff) + 3584;
	Start3DSoundVolPitch(-1, jer_carexplode_bank(), jer_carexplode_bang_sample(),
		at.vx, at.vy, at.vz, 0, pitch);
}

void jer_carexplode_hold(void* car)
{
	CAR_DATA* cp = (CAR_DATA*)car;
	int id;

	if (cp == NULL)
		return;

	id = cp->id;
	if (id < 0 || id >= MAX_CARS || !gLatched[id])
		return;

	JerCarExplodeLock(cp);
}

void jer_carexplode_reset(int carId)
{
	int i;

	if (carId >= 0 && carId < MAX_CARS)
	{
		gLatched[carId] = 0;
		return;
	}

	for (i = 0; i < MAX_CARS; i++)
		gLatched[i] = 0;
}
