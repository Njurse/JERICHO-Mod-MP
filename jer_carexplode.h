#ifndef JER_CAREXPLODE_H
#define JER_CAREXPLODE_H

/* A self-contained "this car just wrecked" presentation: dent, tumble, lock
 * the controls, three visual-only BIG_BANG blasts, and a bang sample.
 *
 * Copied from the Caine's Crossfire wreck recipe and the stock bomberman
 * dent, but it does not call or include anything from either. It is meant
 * to be easy to lift out of this module later.
 *
 * Dedi never runs this C. A machine with NoPlayerControl also skips the
 * blasts and the sound (replays / a cutscene). Dent, tumble and the lock
 * still run if a car is passed in.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Ask the core to append, at every following level load (before the
 * car-import memtop is saved):
 *   SBK_ID_COUNTDOWN (lump 44) -- the short beep, two samples
 *   SBK_ID_MISSION_23 (lump 54) -- Car Bomb bang at sample 1 (sample 0 is
 *     the clock tick; do not use it for the bomb warning)
 * Call once at module entry. */
void jer_carexplode_request_banks(void);

/* Fire once per car. A second call on the same car id is a no-op until
 * jer_carexplode_reset. `car` is a CAR_DATA*. */
void jer_carexplode(void* car);

/* Re-apply the control lock every frame while that car is still latched. */
void jer_carexplode_hold(void* car);

/* Clear the latch. carId < 0 clears every slot. */
void jer_carexplode_reset(int carId);

/* SOUND_BANK_SFX, always. Beep is COUNTDOWN lump sample 0 (fallback SFX 1);
 * bang is mission-23 lump sample 1 (fallback SFX 6). */
int  jer_carexplode_bank(void);
int  jer_carexplode_beep_sample(void);
int  jer_carexplode_bang_sample(void);
int  jer_carexplode_samples_loaded(void);	/* 1 when beep and bang lumps landed */

#ifdef __cplusplus
}
#endif

#endif /* JER_CAREXPLODE_H */
