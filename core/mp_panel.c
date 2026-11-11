/*
 * core/mp_panel.c -- the pause panel: the in-game player list.
 *
 * A live match must NOT open the engine pause (it freezes THIS machine while
 * everyone else keeps driving), so the player list is a non-freezing overlay
 * drawn from the pause-menu path. This file owns the panel's text, colours and
 * the two row producers: MpDrawPlayerList (draws it) and MpLogPlayerList (the
 * same rows as a log line, for headless checks).
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_frontend.h"
#include "jer_pause_menu.h"
#include "jer_colour.h"
#include "jer_ped_palette.h"
#include "jer_npc.h"
#include "jer_carexplode.h"
#include "jer_carglow.h"
#include "jer_carquery.h"
#include "jer_sound.h"

#include "driver2.h"
#include "system.h"
#include "main.h"
#include "pause.h"
#include "mission.h"
#include "pad.h"
#include "players.h"
#include "cars.h"
#include "camera.h"
#include "glaunch.h"
#include "state.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mp.h"
#include "jer_console.h"

/* The engine's text primitives (declared in Game/C/pres.h, which pulls in the
 * full PSX type set -- declare just what the panel needs instead). */
extern void SetTextColour(unsigned char Red, unsigned char Green, unsigned char Blue);
extern int  PrintStringHiresScaledSpaced(char* string, int x, int y, float scale, float extra);

/* The same rows the pause menu draws, without drawing anything: enough to check
 * names, slots and ping from a log, and safe to call from anywhere. */
void MpLogPlayerList(void)
{
	int id;

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		CAR_DATA* cp;
		int veh = -1;

		if (p == NULL)
			continue;

		if (p->carId >= 0 && p->carId < MAX_CARS)
		{
			cp = &car_data[p->carId];

			if (cp->controlType != CONTROL_TYPE_NONE)
				veh = cp->ap.model;
		}

		if (gMpCtx != NULL)
		{
			MP_PEER_STATS st;
			char net[72];

			if (!p->isLocal && MpPeerStats(p->id, &st) && st.linkMs > 0)
			{
				unsigned long rxS = st.rxBytes * 1000UL / st.linkMs;
				unsigned long txS = st.txBytes * 1000UL / st.linkMs;

				snprintf(net, sizeof(net), "%d ms  rx %luB/s  tx %luB/s  loss %d%%",
					st.pingMs, rxS, txS, st.lossPct);
			}
			else
			{
				snprintf(net, sizeof(net), "%d ms", p->pingMs);
			}

			gMpCtx->jer_log(gMpCtx, "[mp] list: (build %04x mods %04x) %s #%d  car %d  slot %d  %s%s\n",
				MpBuildHash(), MpModHash(),
				p->name, p->id, veh, p->carId, net, p->isHost ? "  (host)" : "");
		}
	}
}

/* ------------------------------------------------------------------ */
/* The pause panel                                                     */
/*                                                                     */
/* This readout wears its OWN colours, deliberately not the chat's:     */
/* chat is a conversation -- a speaker's name in their colour, the      */
/* message in a flat near-white -- while this is a status panel, so it  */
/* gets an accent for its title, a colour per ROLE, and dim greys for   */
/* its labels and chrome.                                              */
/*                                                                     */
/* Every value below is the colour as it should LOOK, because MpInk     */
/* halves it: the HQ font is drawn with the PSX texture filter on (x2), */
/* the same trap jer_console's jerConsoleInk handles. Without it every  */
/* grey clips to white -- which is exactly what this panel used to be:  */
/* its 200/170/150/140 were one flat white, and only the saturated cyan */
/* of the host survived.                                               */
static u_char MpInk(int v)
{
	if (v <= 0)
		return 0;
	if (v >= 255)
		return 128;		/* 128 * 2 = 256, clipped back to 255 */

	return (u_char)((v * 128) / 255);
}

#define MP_INK(r, g, b)		MpInk(r), MpInk(g), MpInk(b)

#define MP_PANEL_ACCENT_R	130	/* the title: a cool accent, so the panel reads as its own thing */
#define MP_PANEL_ACCENT_G	190
#define MP_PANEL_ACCENT_B	255
#define MP_PANEL_IDENT_R	150	/* the build/addons line */
#define MP_PANEL_IDENT_G	160
#define MP_PANEL_IDENT_B	185
#define MP_PANEL_LABEL_R	150	/* column headers and the columns that are not the point */
#define MP_PANEL_LABEL_G	150
#define MP_PANEL_LABEL_B	150
#define MP_PANEL_SELF_R		150	/* your own row */
#define MP_PANEL_SELF_G		245
#define MP_PANEL_SELF_B		170
#define MP_PANEL_HOST_R		0	/* the host, cyan, as it always was */
#define MP_PANEL_HOST_G		255
#define MP_PANEL_HOST_B		255
#define MP_PANEL_PEER_R		210	/* everyone else */
#define MP_PANEL_PEER_G		210
#define MP_PANEL_PEER_B		210
#define MP_PANEL_WARN_R		255	/* a link that is dropping frames */
#define MP_PANEL_WARN_G		190
#define MP_PANEL_WARN_B		90

/* Half size, like the status console. This is what makes ONE line per player
 * possible: at the default 0.275 only about 33 characters fit, which is why a
 * player used to take two rows -- a name row and an indented link row. */
#define MP_PANEL_SCALE		0.138f
#define MP_PANEL_TRACKING	0.75f

/* Column x positions in PSX screen units (the screen is 320 wide). A full row is
 * about 50 half-size characters, which fits with room to spare. */
#define MP_COL_NAME		6
#define MP_COL_CAR		92
#define MP_COL_PING		130
#define MP_COL_RX		164
#define MP_COL_TX		206
#define MP_COL_LOSS		250
#define MP_COL_PTS		280	/* a timer mode's points (MpRoundPlayerScore) */

#define MP_PANEL_Y_TITLE	6
#define MP_PANEL_Y_IDENT	15
#define MP_PANEL_Y_HEAD		26
#define MP_PANEL_Y_FIRST	36
#define MP_PANEL_Y_STEP		10

static void MpPanelText(const char* text, int x, int y)
{
	PrintStringHiresScaledSpaced((char*)text, x, y, MP_PANEL_SCALE, MP_PANEL_TRACKING);
}

/* A byte rate short enough for its column: "12.3K/s", "999B/s". The value is
 * capped so the widest string the column can ever hold is 7 characters -- the
 * column is 42 px, and a row that grew past it would run into the next one. */
static void MpFormatRate(char* out, size_t max, unsigned long bytesPerSec)
{
	if (bytesPerSec > 99999UL)
		bytesPerSec = 99999UL;

	if (bytesPerSec >= 1000UL)
		snprintf(out, max, "%.1fK/s", (double)bytesPerSec / 1000.0);
	else
		snprintf(out, max, "%luB/s", bytesPerSec);
}

/* The name, plus what the player IS. The name column is about 17 half-size
 * characters, so a long name is cut rather than allowed to run into the car
 * column. The player id stays in the log: on screen the role word is what a
 * player can act on. */
static void MpFormatPanelName(char* out, size_t max, const MP_PLAYER* p)
{
	char name[MP_NAME_MAX];

	snprintf(name, sizeof(name), "%s", (p->name[0] != '\0') ? p->name : "?");

	if (strlen(name) > 11)
		name[11] = '\0';

	snprintf(out, max, "%s%s", name,
		p->isLocal ? " (you)" : (p->isHost ? " (host)" : ""));
}

void MpDrawPlayerList(void)
{
	char ident[80];
	int id, row = 0;

	/* TOP-LEFT corner and high up: the engine's own pause panel is centred
	 * around the middle of the screen (Bound.y = MAX(48, (SCREEN_H -
	 * (n+1)*15)/2) in pause.c), so the room is up here. Half size is what makes
	 * the room, not the corner. */
	SetTextColour(MP_INK(MP_PANEL_ACCENT_R, MP_PANEL_ACCENT_G, MP_PANEL_ACCENT_B));
	MpPanelText("PLAYERS", MP_COL_NAME, MP_PANEL_Y_TITLE);

	/* The build identity, said in plain words. These are the same numbers the
	 * startup log prints and the ones strict_version compares, and a mismatch is
	 * the single most common cause of "we cannot see each other". */
	snprintf(ident, sizeof(ident), "all players must match:  build %04x  addons %04x",
		MpBuildHash(), MpModHash());

	SetTextColour(MP_INK(MP_PANEL_IDENT_R, MP_PANEL_IDENT_G, MP_PANEL_IDENT_B));
	MpPanelText(ident, MP_COL_NAME, MP_PANEL_Y_IDENT);

	SetTextColour(MP_INK(MP_PANEL_LABEL_R, MP_PANEL_LABEL_G, MP_PANEL_LABEL_B));
	MpPanelText("PLAYER", MP_COL_NAME, MP_PANEL_Y_HEAD);
	MpPanelText("CAR",    MP_COL_CAR,  MP_PANEL_Y_HEAD);
	MpPanelText("PING",   MP_COL_PING, MP_PANEL_Y_HEAD);
	MpPanelText("RX",     MP_COL_RX,   MP_PANEL_Y_HEAD);
	MpPanelText("TX",     MP_COL_TX,   MP_PANEL_Y_HEAD);
	MpPanelText("LOSS",   MP_COL_LOSS, MP_PANEL_Y_HEAD);

	if (MpModeUsesTimer(gMp.gamemode))
		MpPanelText("PTS", MP_COL_PTS, MP_PANEL_Y_HEAD);

	for (id = 0; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* p = MpGetPlayer(id);
		CAR_DATA* cp;
		char name[MP_NAME_MAX + 12];
		char car[20];
		char ping[16];
		char rx[16];
		char tx[16];
		char loss[8];
		char net[72];
		int lossPct = -1;
		int veh = -1;
		int vehCity = -1;
		int y;

		if (p == NULL)
			continue;

		/* -1 means on foot: either we have no car slot for them, or the car is
		 * standing there with nobody driving it */
		if (p->carId >= 0 && p->carId < MAX_CARS)
		{
			cp = &car_data[p->carId];

			/* WHICH car, the way car selection names it: the city whose car list
			 * it comes from and its model number in that city (MP_CAR's model +
			 * modelCity). ap.model is only the RESIDENT SLOT the level loaded it
			 * into, which says nothing about the car itself. */
			if (cp->controlType != CONTROL_TYPE_NONE &&
			    cp->ap.model >= 0 && cp->ap.model < MAX_CAR_RESIDENT_MODELS)
			{
				veh = residentCarModels[cp->ap.model];
				vehCity = GetCarModelSourceCity(cp->ap.model);

				if (vehCity < 0)
					vehCity = GameLevel;
			}
		}

		MpFormatPanelName(name, sizeof(name), p);

		if (veh >= 0)
			snprintf(car, sizeof(car), "%d:%d", vehCity, veh);
		else
			snprintf(car, sizeof(car), "on foot");

		{
			MP_PEER_STATS st;

			if (!p->isLocal && MpPeerStats(p->id, &st) && st.linkMs > 0)
			{
				unsigned long rxS = st.rxBytes * 1000UL / st.linkMs;
				unsigned long txS = st.txBytes * 1000UL / st.linkMs;

				/* loss is the fraction of sim frames in which NOTHING arrived
				 * from them -- the closest honest proxy TCP gives us */
				lossPct = st.lossPct;

				snprintf(ping, sizeof(ping), "%dms", st.pingMs);
				MpFormatRate(rx, sizeof(rx), rxS);
				MpFormatRate(tx, sizeof(tx), txS);
				snprintf(loss, sizeof(loss), "%d%%", st.lossPct);
			}
			else
			{
				/* Nothing measured (this machine, or a link that has not
				 * reported yet): keep every column, so the table does not
				 * shift about as rows come and go. */
				snprintf(ping, sizeof(ping), "%dms", p->pingMs);
				snprintf(rx, sizeof(rx), "--");
				snprintf(tx, sizeof(tx), "--");
				snprintf(loss, sizeof(loss), "--");

				/* A row we have no link of our own to (a client looking at another
				 * client): the HOST's measurement of that player's link, when it
				 * sent one lately (MP_TAG_STATS). */
				if (!p->isLocal && p->netLastMs != 0 && MpNowMs() - p->netLastMs < 10000UL)
				{
					MpFormatRate(rx, sizeof(rx), p->netRxBps);
					MpFormatRate(tx, sizeof(tx), p->netTxBps);

					if (p->netLoss != MP_STATS_LOSS_UNKNOWN)
					{
						lossPct = p->netLoss;
						snprintf(loss, sizeof(loss), "%d%%", p->netLoss);
					}
				}
			}
		}

		y = MP_PANEL_Y_FIRST + row * MP_PANEL_Y_STEP;
		row++;

		if (p->isLocal)
			SetTextColour(MP_INK(MP_PANEL_SELF_R, MP_PANEL_SELF_G, MP_PANEL_SELF_B));
		else if (p->isHost)
			SetTextColour(MP_INK(MP_PANEL_HOST_R, MP_PANEL_HOST_G, MP_PANEL_HOST_B));
		else
			SetTextColour(MP_INK(MP_PANEL_PEER_R, MP_PANEL_PEER_G, MP_PANEL_PEER_B));

		MpPanelText(name, MP_COL_NAME, y);
		MpPanelText(car,  MP_COL_CAR,  y);
		MpPanelText(ping, MP_COL_PING, y);
		MpPanelText(rx,   MP_COL_RX,   y);
		MpPanelText(tx,   MP_COL_TX,   y);

		/* A dropping link is the one figure worth colouring; the rest is detail. */
		if (lossPct >= 5)
			SetTextColour(MP_INK(MP_PANEL_WARN_R, MP_PANEL_WARN_G, MP_PANEL_WARN_B));
		else
			SetTextColour(MP_INK(MP_PANEL_LABEL_R, MP_PANEL_LABEL_G, MP_PANEL_LABEL_B));

		MpPanelText(loss, MP_COL_LOSS, y);

		/* the mode's points, from the round state (the host's JPRD) */
		if (MpModeUsesTimer(gMp.gamemode))
		{
			char pts[16];

			snprintf(pts, sizeof(pts), "%d", MpRoundPlayerScore(p->id));
			SetTextColour(MP_INK(MP_PANEL_LABEL_R, MP_PANEL_LABEL_G, MP_PANEL_LABEL_B));
			MpPanelText(pts, MP_COL_PTS, y);
		}

		/* so the list can be checked without eyes on the screen */
		if (MpDebugOn() && gMpCtx != NULL)
		{
			static unsigned long lastListMs;

			if ((MpNowMs() - lastListMs) > 2000)
			{
				lastListMs = MpNowMs();

				if (p->isLocal)
					snprintf(net, sizeof(net), "%s  (local)", ping);
				else
					snprintf(net, sizeof(net), "%s  %s  %s  loss %s", ping, rx, tx, loss);

				gMpCtx->jer_log(gMpCtx, "[mp] list: (build %04x mods %04x) %s | %s\n",
					MpBuildHash(), MpModHash(), name, net);
			}
		}
	}

	/* The HOST also gets where to be found: the address a friend types into
	 * Manual and whether the Online list really shows this game. */
	if (gMp.role == MP_ROLE_HOST)
	{
		char addr[112], list[112];
		int y = MP_PANEL_Y_FIRST + row * MP_PANEL_Y_STEP + 4;
		int adv = MpMasterAdvertiseStatus(NULL, NULL, NULL);
		int bad = (adv == MP_ADV_FAILED || adv == MP_ADV_MISSING ||
			(adv == MP_ADV_LISTED && MpMasterAdvertiseReachable() == 0));

		MpHostStatusLines(addr, sizeof(addr), list, sizeof(list), 0);

		SetTextColour(MP_INK(MP_PANEL_HOST_R, MP_PANEL_HOST_G, MP_PANEL_HOST_B));
		MpPanelText(addr, MP_COL_NAME, y);

		if (bad)
			SetTextColour(MP_INK(MP_PANEL_WARN_R, MP_PANEL_WARN_G, MP_PANEL_WARN_B));
		else
			SetTextColour(MP_INK(MP_PANEL_IDENT_R, MP_PANEL_IDENT_G, MP_PANEL_IDENT_B));

		MpPanelText(list, MP_COL_NAME, y + MP_PANEL_Y_STEP);
	}
}
