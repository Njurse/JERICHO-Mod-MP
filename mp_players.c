/* The player registry, and the car accessors built on it.
 *
 * Split out of mp.c: the registry is what keeps remote players apart from the
 * game's own traffic, and it has nothing to do with the engine hook plumbing. */
#include "jericho.h"
#include "mp.h"

#include "driver2.h"
#include "cars.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

MP_PLAYER* MpLocalPlayer(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gMp.players[i].active && gMp.players[i].isLocal)
			return &gMp.players[i];
	}

	return NULL;
}

MP_PLAYER* MpGetPlayer(int id)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gMp.players[i].active && gMp.players[i].id == id)
			return &gMp.players[i];
	}

	return NULL;
}

MP_PLAYER* MpGetPlayerByCar(int carId)
{
	int i;

	if (carId < 0)
		return NULL;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gMp.players[i].active && gMp.players[i].carId == carId)
			return &gMp.players[i];
	}

	return NULL;
}

int MpIsPlayerCar(int carId)
{
	return MpGetPlayerByCar(carId) != NULL;
}

/* The overlay needs a car's pose, but only mp.c pulls in cars.h -- expose a
 * tiny accessor rather than leaking CAR_DATA into the UI file. */
void MpCarPose(int carId, int* x, int* y, int* z, int* heading)
{
	if (carId < 0 || carId >= MAX_CARS)
	{
		if (x != NULL) *x = 0;
		if (y != NULL) *y = 0;
		if (z != NULL) *z = 0;
		if (heading != NULL) *heading = 0;
		return;
	}

	if (x != NULL) *x = car_data[carId].hd.where.t[0];
	if (y != NULL) *y = car_data[carId].hd.where.t[1];
	if (z != NULL) *z = car_data[carId].hd.where.t[2];
	if (heading != NULL) *heading = car_data[carId].hd.direction;
}

MP_PLAYER* MpAddPlayer(int id, const char* name, int isLocal)
{
	int i;
	MP_PLAYER* p = NULL;

	/* reuse the row with this id if present */
	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gMp.players[i].active && gMp.players[i].id == id)
		{
			p = &gMp.players[i];
			break;
		}
	}

	if (p == NULL)
	{
		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			if (!gMp.players[i].active)
			{
				p = &gMp.players[i];
				memset(p, 0, sizeof(*p));
				p->active = 1;
				p->carId = -1;
				p->padId = -1;
				p->id = id;
				gMp.playerCount++;
				break;
			}
		}
	}

	if (p == NULL)
		return NULL;	/* registry full */

	snprintf(p->name, sizeof(p->name), "%s", name != NULL ? name : "Player");
	p->isLocal = isLocal;
	p->connected = 1;

	return p;
}

void MpRemovePlayer(int id)
{
	MP_PLAYER* p = MpGetPlayer(id);

	if (p == NULL)
		return;

	/* let the player know who left (never ourselves) */
	if (!p->isLocal)
		MpNotifyf("%s left", p->name);

	/* take their car out of the world so it does not sit there parked with
	 * nobody driving it; the slot is recycled by the engine */
	if (!p->isLocal && p->carId >= 0 && p->carId < MAX_CARS)
	{
		car_data[p->carId].controlType = CONTROL_TYPE_NONE;

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] removed player %d's car (slot %d)\n", p->id, p->carId);
	}

	memset(p, 0, sizeof(*p));
	p->carId = -1;
	p->padId = -1;

	if (gMp.playerCount > 0)
		gMp.playerCount--;
}

void MpResetPlayers(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		memset(&gMp.players[i], 0, sizeof(gMp.players[i]));
		gMp.players[i].carId = -1;
		gMp.players[i].padId = -1;
	}

	gMp.playerCount = 0;
}

int MpIsActive(void)
{
	return gMp.role != MP_ROLE_NONE;
}

int MpIsHost(void)
{
	return gMp.role == MP_ROLE_HOST;
}
