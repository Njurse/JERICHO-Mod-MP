/* The multiplayer map: a blip for every other player.
 *
 * Split out of mp.c: this is drawing, and it was the largest single reason that
 * file kept growing. */
#include "jericho.h"
#include "jer_events.h"
#include "mp.h"

#include "driver2.h"
#include "cars.h"
#include "overmap.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* The engine's own map helpers, declared here the way the rest of the module
 * declares the engine symbols it needs. DrawPlayerDot picks the transform from the
 * flags it is given, which is why the hook's flags have to travel with the
 * position -- see the comment in the loop. */
extern void DrawPlayerDot(VECTOR* pos, short rot, u_char r, u_char g, u_char b, int flags);

int MpOnDrawMap(void* userdata, void* args)
{
	JER_ARGS_DRAW_MAP* m = (JER_ARGS_DRAW_MAP*)args;
	u_char r = 255;
	u_char g = 0;
	int i, drawn = 0;
	int firstId = -1, firstVx = 0, firstVz = 0;

	(void)userdata;

	if (m == NULL || m->fullscreen || !gMp.running)
		return JER_RESULT_CONTINUE;


	/* Keep the engine's own blip for OUR car. It used to be suppressed ("the
	 * local player was left with an arrow stuck on them"), but without it the
	 * local player has NO marker at all on the map -- which is worse. The remote
	 * players are added by the loop below; ours is the engine's. */
	m->suppressStockBlip = 0;

	if (MpDebugOn() && gMpCtx != NULL)
	{
		static int announced;

		if (!announced)
		{
			announced = 1;
			gMpCtx->jer_log(gMpCtx, "[mp] map: took over the player blips\n");
		}
	}
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		MP_PLAYER* p = &gMp.players[i];
		CAR_DATA* cp;
		VECTOR target;

		if (!p->active || p->carId < 0 || p->carId >= MAX_CARS)
			continue;

		/* The arrows mark the OTHER players, the host included. Our own car is
		 * already the engine's own blip (it draws one for its single player),
		 * so drawing one for ourselves here would double it up. */
		if (p->isLocal || p->id == gMp.localPlayerId)
			continue;

		cp = &car_data[p->carId];

		target.vx = cp->hd.where.t[0];
		target.vy = 0;
		target.vz = cp->hd.where.t[2];

		/* Hand DrawPlayerDot the hook's OWN flags and let the ENGINE place the
		 * blip, exactly as the stock loops do -- the flags are what say which map
		 * this is, and the transform belongs to the map.
		 *
		 * This used to transform here with WorldToMultiplayerMap, which is WRONG on
		 * a single-player level: that function has no maths for region 0 and
		 * returns a CONSTANT (32,32), so every remote player's arrow was handed the
		 * same wrong point and none of them showed. DrawPlayerDot already handles
		 * 0x20 (the multiplayer map) and 0x1 (the overhead/single-player map, which
		 * it also clips to its own rect).
		 *
		 * -hd.direction, exactly as the stock loop passes -pl->dir. */
		if (drawn == 0)
		{
			firstId = p->id;
			firstVx = target.vx;
			firstVz = target.vz;
		}

		DrawPlayerDot(&target, (short)-cp->hd.direction, r, g, 0, m->flags);

		drawn++;

		r++;
		g--;
	}

	/* one line, so "did the hook run and how many blips" is answerable from the
	 * log rather than from a screenshot */
	if (drawn > 0 && MpDebugOn() && gMpCtx != NULL)
	{
		static unsigned long lastLoggedMs;

		if ((MpNowMs() - lastLoggedMs) > 5000)
		{
			lastLoggedMs = MpNowMs();
			gMpCtx->jer_log(gMpCtx, "[mp] map: drew %d remote blip(s)\n", drawn);

			/* WHICH map this is (the flags), and the first blip's INPUT world
			 * position -- logged so the single-player placement is checkable from the
			 * log. Deliberately NOT a second copy of the transform: that would test
			 * the copy rather than what the engine is actually given. */
			gMpCtx->jer_log(gMpCtx,
				"[mp] map: flags 0x%x; first remote blip is player %d at world %d,%d\n",
				m->flags, firstId, firstVx, firstVz);
		}
	}

	return JER_RESULT_CONTINUE;
}
