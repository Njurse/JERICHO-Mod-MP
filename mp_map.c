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

/* The engine's own multiplayer-map helpers, declared here the way the rest of
 * the module declares the engine symbols it needs. */
extern void WorldToMultiplayerMap(VECTOR* in, VECTOR* out);
extern void DrawPlayerDot(VECTOR* pos, short rot, u_char r, u_char g, u_char b, int flags);

int MpOnDrawMap(void* userdata, void* args)
{
	JER_ARGS_DRAW_MAP* m = (JER_ARGS_DRAW_MAP*)args;
	u_char r = 255;
	u_char g = 0;
	int i, drawn = 0;

	(void)userdata;

	if (m == NULL || m->fullscreen || !gMp.running)
		return JER_RESULT_CONTINUE;


	/* Take the player blips over. NumPlayers is held at 1 in a multiplayer
	 * session, so without this the engine draws one blip -- ours -- and we can
	 * only add to it, leaving the local player with an arrow stuck on them. The
	 * map is now ours: every OTHER player gets a marker below, and none for us. */
	m->suppressStockBlip = 1;

	if (getenv("MP_DEBUG") != NULL && gMpCtx != NULL)
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

		WorldToMultiplayerMap(&target, &target);

		target.vx += gMapXOffset;
		target.vz += gMapYOffset;

		/* -hd.direction, exactly as the stock loop passes -pl->dir, and 0x8 is
		 * the flag the engine itself draws a player blip with */
		DrawPlayerDot(&target, (short)-cp->hd.direction, r, g, 0, 0x8);

		drawn++;

		r++;
		g--;
	}

	/* one line, so "did the hook run and how many blips" is answerable from the
	 * log rather than from a screenshot */
	if (drawn > 0 && getenv("MP_DEBUG") != NULL && gMpCtx != NULL)
	{
		static unsigned long lastLoggedMs;

		if ((MpNowMs() - lastLoggedMs) > 5000)
		{
			lastLoggedMs = MpNowMs();
			gMpCtx->jer_log(gMpCtx, "[mp] map: drew %d remote blip(s)\n", drawn);
		}
	}

	return JER_RESULT_CONTINUE;
}
