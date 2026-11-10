#include "jer_carglow.h"

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"	/* carCos->cog: DrawCarObject places the body here */
#include "main.h"	/* ActiveCheats.cheat13: mini cars, same cog scale as DrawCarObject */
#include "camera.h"
#include "draw.h"
#include "system.h"
#include "debris.h"	/* smoke_texture: the stock explosion material */
#include "glaunch.h"	/* NoPlayerControl */
#include "ASM/rndrasm.h"
#include "dr2math.h"
#include "jericho.h"

/* ~1.12x about the body centre. DrawCarObject (cars.c:3964-3983) does not
 * draw verts about the model origin: it sets trans to pos, transforms
 * carCos->cog, and uses that as the model trans, so a vertex v appears at
 * pos + R*(v + cog). The car position / visual centre is model point -cog.
 * Scaling about 0 with trans=pos left the shell growing from the mesh
 * origin, off the body. */
#define JER_CARGLOW_SCALE	4588

static CAR_DATA* gGlowCar;
static int gGlowR, gGlowG, gGlowB;
static SVECTOR gGlowVerts[MAX_DENTING_VERTS];

void jer_carglow_set(void* car, int r, int g, int b)
{
	CAR_DATA* cp = (CAR_DATA*)car;

	if (cp == NULL || cp->id < 0 || cp->id >= MAX_CARS)
	{
		gGlowCar = NULL;
		return;
	}

	gGlowCar = cp;
	gGlowR = r < 0 ? 0 : (r > 255 ? 255 : r);
	gGlowG = g < 0 ? 0 : (g > 255 ? 255 : g);
	gGlowB = b < 0 ? 0 : (b > 255 ? 255 : b);
}

void jer_carglow_clear(void)
{
	gGlowCar = NULL;
}

/* Scale about model-space -cog, the point DrawCarObject places on the car
 * position. Then the same trans path (pos, then + R*cog) keeps the shell
 * centred on the body. */
static void JerCarGlowCopyVerts(CAR_DATA* cp, int scale, SVECTOR cog)
{
	int i;
	const SVECTOR* src = gTempCarVertDump[cp->id];
	int cx = -cog.vx, cy = -cog.vy, cz = -cog.vz;

	for (i = 0; i < MAX_DENTING_VERTS; i++)
	{
		gGlowVerts[i].vx = (short)(cx + (((src[i].vx - cx) * scale) >> 12));
		gGlowVerts[i].vy = (short)(cy + (((src[i].vy - cy) * scale) >> 12));
		gGlowVerts[i].vz = (short)(cz + (((src[i].vz - cz) * scale) >> 12));
		gGlowVerts[i].pad = src[i].pad;
	}
}

static void JerCarGlowTris(const CAR_POLY* src, int num, u_int rgb)
{
	POLY_FT3* prim;
	int Z, otz;
	int indices;
	const SVECTOR *v0, *v1, *v2;
	u_int u0, u1, u2;

	if (src == NULL || num <= 0 || current == NULL || current->primptr == NULL)
		return;

	/* one pair of smoke UVs, animated a little so the material loops */
	u0 = ((u_int)smoke_texture.clutid << 16) |
		((u_int)(u_char)(smoke_texture.coords.u0 + (CameraCnt & 15))) |
		((u_int)(u_char)(smoke_texture.coords.v0 + ((CameraCnt * 2) & 15)) << 8);
	u1 = ((u_int)(smoke_texture.tpageid | 0x20) << 16) |
		((u_int)(u_char)(smoke_texture.coords.u1 + (CameraCnt & 15))) |
		((u_int)(u_char)(smoke_texture.coords.v1 + ((CameraCnt * 2) & 15)) << 8);
	u2 = ((u_int)(u_char)(smoke_texture.coords.u2 + (CameraCnt & 15))) |
		((u_int)(u_char)(smoke_texture.coords.v2 + ((CameraCnt * 2) & 15)) << 8);

	while (num > 0)
	{
		indices = src->vindices;
		v0 = gGlowVerts + (indices & 0xff);
		v1 = gGlowVerts + ((indices >> 8) & 0xff);
		v2 = gGlowVerts + ((indices >> 16) & 0xff);

		/* stay inside the dent dump; a bad index would walk off the copy */
		if ((indices & 0xff) >= MAX_DENTING_VERTS ||
		    ((indices >> 8) & 0xff) >= MAX_DENTING_VERTS ||
		    ((indices >> 16) & 0xff) >= MAX_DENTING_VERTS)
		{
			num--;
			src++;
			continue;
		}

		if (current->primptr + sizeof(POLY_FT3) >= current->primtab + PRIMTAB_SIZE)
			return;

		prim = (POLY_FT3*)current->primptr;

		gte_ldv3(v0, v1, v2);
		gte_rtpt();
		gte_stsxy3(&prim->x0, &prim->x1, &prim->x2);
		gte_avsz3();
		gte_stotz(&Z);

		otz = Z >> 1;
		if (otz > 0 && otz < OTSIZE)
		{
			*(u_int*)&prim->r0 = rgb;
			*(u_int*)&prim->u0 = u0;
			*(u_int*)&prim->u1 = u1;
			*(u_short*)&prim->u2 = (u_short)u2;
			setPolyFT3(prim);
			setSemiTrans(prim, 1);
			addPrim(current->ot + otz, prim);
			current->primptr += sizeof(POLY_FT3);
		}

		num--;
		src++;
	}
}

int jer_carglow_on_draw_world(void* userdata, void* args)
{
	CAR_DATA* cp = gGlowCar;
	VECTOR pos;
	MATRIX workmatrix;
	CAR_MODEL* mdl;
	int model, wave, scale, pulse, r, g, b;
	u_int rgb;

	(void)userdata;
	(void)args;

	if (cp == NULL || NoPlayerControl || current == NULL)
		return JER_RESULT_CONTINUE;

	if (cp->id < 0 || cp->id >= MAX_CARS || !JerCarSlotDrawable(cp->id))
		return JER_RESULT_CONTINUE;

	model = cp->ap.model;
	if (model < 0 || model >= MAX_CAR_RESIDENT_MODELS)
		return JER_RESULT_CONTINUE;

	mdl = &NewLowCarModel[model];
	if ((mdl->numFT3 <= 0 && mdl->numGT3 <= 0) ||
	    (mdl->pFT3 == NULL && mdl->pGT3 == NULL))
		return JER_RESULT_CONTINUE;

	pos.vx = cp->hd.where.t[0];
	pos.vz = cp->hd.where.t[2];
	pos.vy = -cp->hd.where.t[1];

	if (FrustrumCheck(&pos, 800) == -1)
		return JER_RESULT_CONTINUE;

	pos.vx -= camera_position.vx;
	pos.vy -= camera_position.vy;
	pos.vz -= camera_position.vz;
	Apply_Inv_CameraMatrix(&pos);
	MulMatrix0(&inv_camera_matrix, &cp->hd.drawCarMat, &workmatrix);

	if (cp->ap.carCos == NULL)
		return JER_RESULT_CONTINUE;

	{
		VECTOR modelLocation;
		SVECTOR cog = cp->ap.carCos->cog;

		/* same mini-car adjustment DrawCarObject applies to cog (cars.c:3966-3971) */
		if (ActiveCheats.cheat13 != 0)
		{
			cog.vx <<= 1;
			cog.vy <<= 1;
			cog.vz <<= 1;
		}

		/* DrawCarObject cars.c:3973-3983 */
		gte_SetRotMatrix(&workmatrix);
		gte_SetTransVector(&pos);
		gte_ldv0(&cog);
		gte_rtv0tr();
		gte_stlvnl(&modelLocation);
		gte_SetTransVector(&modelLocation);

		wave = RSIN(CameraCnt * 96);
		scale = JER_CARGLOW_SCALE + ((wave * 200) >> 12);
		if (scale < 4096)
			scale = 4096;

		pulse = 160 + ((wave * 70) >> 12);
		if (pulse < 40)
			pulse = 40;
		else if (pulse > 255)
			pulse = 255;

		r = (gGlowR * pulse) / 255;
		g = (gGlowG * pulse) / 255;
		b = (gGlowB * pulse) / 255;
		/* packed B<<16 | G<<8 | R, same as DrawExplosion */
		rgb = ((u_int)b << 16) | ((u_int)g << 8) | (u_int)r;

		JerCarGlowCopyVerts(cp, scale, cog);
		JerCarGlowTris(mdl->pFT3, mdl->numFT3, rgb);
		JerCarGlowTris(mdl->pGT3, mdl->numGT3, rgb);
	}

	return JER_RESULT_CONTINUE;
}
