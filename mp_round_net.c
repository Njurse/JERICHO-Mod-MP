/*
 * mp_round_net.c -- split out of mp_session.c
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"

/* ---- THE ROUND STATE (MP_TAG_ROUND, mp_round.c) ----------------------------
 *
 * The host is the only writer. It keeps the round's present-set equal to the
 * player registry (the same rows the roster names), begins round 1 at START, and
 * sends the snapshot after a WELCOME, on START, whenever it changes, and on the
 * roster's 120-frame cadence. A client applies each one whole and never sends.
 *
 * Take a Ride: one round, live from START until the session ends, everyone
 * playing, no teams, score 0. Nothing displays or acts on it yet (Phase 2+). */
static unsigned gMpRoundSentRev;	/* the revision last broadcast */
static int gMpRoundNoSnapshotLogged;	/* client: "no snapshot yet" said once */
static int gMpRoundRefusedLogged;	/* "refused a snapshot" said once */
static int gMpRoundModeLogged;		/* client: "snapshot names another mode" said once */

void MpRoundHostSync(void);
void MpHostSendRound(int connIndex);

/* A CLIENT ON ANOTHER MODE. A client names the mode it plays in its own carstate
 * (MP_CARSTATE.reserved[0] = gamemode + 1, MpSendOwnCarState; 0 = an older build
 * that does not say). One that disagrees with the host is sent the round state
 * again -- it names the mode and MpHandleRound adopts it -- every
 * MP_MODE_RESEND_MS, and kicked if it still disagrees after
 * MP_MODE_MISMATCH_KICK_MS. */
#define MP_MODE_RESEND_MS		500
#define MP_MODE_MISMATCH_KICK_MS	5000

static int gMpClientMode[MP_MAX_PLAYERS];		/* the reported mode + 1, 0 = none yet */
static unsigned long gMpModeBadSince[MP_MAX_PLAYERS];	/* 0 = agrees with the host */
static unsigned long gMpModeResendAt[MP_MAX_PLAYERS];

/* Called from MpSessionReset: this module's state is per session, so
 * the next match starts from the same values the module loaded with. */
void MpRoundNetReset(void)
{
	gMpRoundModeLogged = 0;
	gMpRoundNoSnapshotLogged = 0;
	gMpRoundRefusedLogged = 0;
	gMpRoundSentRev = 0;
	memset(gMpClientMode, 0, sizeof(gMpClientMode));
	memset(gMpModeBadSince, 0, sizeof(gMpModeBadSince));
	memset(gMpModeResendAt, 0, sizeof(gMpModeResendAt));
}

void MpRoundHostSync(void)
{
	int present[MP_MAX_PLAYERS];
	int id;

	for (id = 0; id < MP_MAX_PLAYERS; id++)
		present[id] = MpGetPlayer(id) != NULL;

	MpRoundSyncPresent(present, gMp.gamemode);

	/* ...and the session's mode. gRound.mode stayed 0 (Take a Ride, MpRoundReset)
	 * until a timer mode's round 1 began, and a client follows the mode a snapshot
	 * names (MpHandleRound): a joiner played the first countdown as Take a Ride (no
	 * role car), and a late joiner's car select asked for no role cars. */
	MpRoundSetMode(gMp.gamemode);

	/* v24: a listen host is its own session's admin (MP_ROUND_ENTRY_F_ADMIN) */
	{
		MP_PLAYER* me = MpLocalPlayer();

		if (MpIsHost() && me != NULL)
			MpRoundSetAdmin(me->id, 1);
	}
}

/* Send the host's snapshot to one connection, or to everyone with connIndex < 0. */
void MpHostSendRound(int connIndex)
{
	unsigned char buf[sizeof(MP_ROUND) + MP_MAX_PLAYERS * sizeof(MP_ROUND_ENTRY)];
	int len;

	if (!MpIsHost())
		return;

	MpRoundHostSync();
	len = MpRoundBuild(buf, (int)sizeof(buf));

	if (len <= 0)
		return;

	if (connIndex >= 0)
	{
		MpSendConn(connIndex, MP_TAG_ROUND, 0, buf, len);
		return;
	}

	MpHostBroadcast(MP_TAG_ROUND, 0, buf, len);
	gMpRoundSentRev = MpRoundRevision();
}

/* Host: a client's carstate said it plays `mode` (MpHandleCarState). */
void MpHostNoteClientMode(int playerId, int mode)
{
	if (playerId > 0 && playerId < MP_MAX_PLAYERS)
		gMpClientMode[playerId] = mode + 1;
}

/* Host, once a frame in a match: re-send the mode to a client on another one,
 * and kick it when that has not worked for MP_MODE_MISMATCH_KICK_MS. */
static void MpHostModeCheckTick(void)
{
	unsigned long now = MpNowMs();
	int id;

	for (id = 1; id < MP_MAX_PLAYERS; id++)
	{
		MP_PLAYER* pl = MpGetPlayer(id);
		int conn;

		if (pl == NULL)
			gMpClientMode[id] = 0;

		if (pl == NULL || pl->isLocal || gMpClientMode[id] == 0 || gMpClientMode[id] - 1 == gMp.gamemode)
		{
			gMpModeBadSince[id] = 0;
			continue;
		}

		conn = MpConnFindByPlayer(id);

		if (conn < 0)
			continue;

		if (gMpModeBadSince[id] == 0)
		{
			gMpModeBadSince[id] = now ? now : 1;
			gMpModeResendAt[id] = now;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] mode check: player %d plays mode %d, the session is mode %d (%s) - re-sending it every %d ms\n",
					id, gMpClientMode[id] - 1, gMp.gamemode, MpModeName(gMp.gamemode), MP_MODE_RESEND_MS);
		}

		if ((long)(now - gMpModeBadSince[id]) >= MP_MODE_MISMATCH_KICK_MS)
		{
			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] mode check: player %d still plays mode %d after %d ms - kicking\n",
					id, gMpClientMode[id] - 1, MP_MODE_MISMATCH_KICK_MS);

			gMpClientMode[id] = 0;
			gMpModeBadSince[id] = 0;
			MpKickPlayer(id);
			continue;
		}

		if ((long)(now - gMpModeResendAt[id]) >= 0)
		{
			MpHostSendRound(conn);
			gMpModeResendAt[id] = now + MP_MODE_RESEND_MS;
		}
	}
}

/* Host, once a frame in a match: broadcast when the state changed (a join, a leave),
 * and unconditionally when `periodic`, so a lost snapshot is repaired. */
void MpHostRoundTick(int periodic)
{
	if (!MpIsHost())
		return;

	MpRoundHostSync();
	MpHostModeCheckTick();

	if (periodic || MpRoundRevision() != gMpRoundSentRev)
		MpHostSendRound(-1);
}

/* Host: the match starts, and with it round 1. A timer mode (Fox N Hounds, Car Bomb
 * Tag) does not: its referee starts round 1 once two players are in (MpHostMarkTick),
 * so until then the round state stays round 0, idle, everyone waiting. */
void MpHostRoundBegin(void)
{
	if (!MpIsHost())
		return;

	if (MpModeUsesTimer(gMp.gamemode))
	{
		MpHostMarkBegin();
		MpHostSendRound(-1);
		return;
	}

	MpRoundHostSync();
	MpRoundBegin(gMp.gamemode);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] round %d live (mode %d %s)\n",
			MpRoundNumber(), gMp.gamemode, MpModeName(gMp.gamemode));

	MpHostSendRound(-1);
}

/* Client: a host snapshot. Clients never originate one, so a HOST drops any it is
 * sent rather than relaying it -- a peer must not be able to write scores or teams. */
void MpHandleRound(int connIndex, const unsigned char* p, int len)
{
	if (gMp.role != MP_ROLE_CLIENT)
	{
		if (!gMpRoundRefusedLogged && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] round state from conn %d ignored: only the host writes it\n", connIndex);
		gMpRoundRefusedLogged = 1;
		return;
	}

	if (!MpRoundApply(p, len))
	{
		if (!gMpRoundRefusedLogged && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] round state refused: malformed (%d bytes) - keeping the last one\n", len);
		gMpRoundRefusedLogged = 1;
		return;
	}

	/* The round state is the HOST's truth, and it names the mode the session is
	 * actually playing. A client that has just loaded the level has had its mode
	 * reset to Take a Ride by MpSessionReset, so take the mode from here. Without
	 * this a dedicated-server client sat in Take a Ride while the server refereed
	 * Fox N Hounds ("round state names mode 1, the session is mode 0 - playing it
	 * as TAKE A RIDE") and no round was ever played on the client. An id this
	 * build does not know still falls back, and still says so once. */
	if (MpRoundMode() != gMp.gamemode)
	{
		if (MpModeFind(MpRoundMode()) != NULL)
		{
			gMp.gamemode = MpRoundMode();
			MpModeNoteAdopted(gMp.gamemode, "the round state");

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] round state names mode %d - the session follows the host (%s)\n",
					MpRoundMode(), MpModeName(gMp.gamemode));
		}
		else if (!gMpRoundModeLogged && gMpCtx != NULL)
		{
			gMpCtx->jer_log(gMpCtx, "[mp] round state names mode %d, the session is mode %d - playing it as %s\n",
				MpRoundMode(), gMp.gamemode, MpModeName(gMp.gamemode));
			gMpRoundModeLogged = 1;
		}
	}

	if (MpDebugOn() && gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] round state: round %d phase %d mode %d\n",
			MpRoundNumber(), MpRoundPhase(), MpRoundMode());
}

/* Client, in a match: no snapshot has come yet. Play as Take a Ride (mp_round.c's
 * defaults) and say so once. 600 frames is ~10 s, far past the snapshot that follows
 * the WELCOME and the START, so this only fires against a host that never sends one. */
void MpClientRoundCheck(void)
{
	if (gMp.role != MP_ROLE_CLIENT || !gMp.running || MpRoundHaveState() ||
		gMpRoundNoSnapshotLogged || gMp.frame < 600)
		return;

	gMpRoundNoSnapshotLogged = 1;

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] no round state from the host yet - playing as %s (all playing, no teams)\n",
			MpModeName(MP_GAMEMODE_TAKEADRIDE));
}
