#ifndef MP_MODE_H
#define MP_MODE_H

/*
 * mp_mode.h -- the game-mode registry and the questions the session asks it.
 *
 * A GAME MODE is a row in a table (mp_mode.c): a descriptor that says what the mode
 * wants -- teams, how a death is answered, what a late joiner gets, whether a round
 * starts with a countdown, whether the car can change mid-round, which cars may be
 * offered, whether the lobby may cycle maps. The session never reads a descriptor
 * field directly: it asks one of the MpMode* questions below at a fixed DECISION
 * POINT, and every answer for mode 0 (Take a Ride) is what the session already did
 * before modes existed. That is the contract this file exists to keep.
 *
 * The mode id is the session's existing `gamemode` byte (MP_GAMEMODE_*, mp_proto.h),
 * which already rides MP_WELCOME, MP_SESSION (inside JPST), MP_BEACON and the master
 * listing. Nothing here changes the wire.
 *
 * An id this build does not know (a newer server) is NAMED "-" and BEHAVES as mode 0:
 * that is exactly what an unknown id did before this file, because nothing read it.
 *
 * Pure on purpose: no engine headers, no session state, so the table and every
 * answer can be checked by a standalone test (tools/mode_check/test_mp_mode.cpp) and
 * mirrored by the Python servers (tools/mp_test.py GAMEMODE_NAMES).
 *
 * How to add a mode: see docs/GAMEMODES.md.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_MODE_MAX_TEAMS	4	/* the framework's ceiling; a mode declares 0..4 */
#define MP_MODE_TEAM_NAME_MAX	16

/* How a death in a session is answered. The answer is ALWAYS "no game over" -- a
 * session never ends on one machine because one player died; these only choose what
 * happens instead. */
typedef enum
{
	MP_MODE_RESPAWN_INSTANT = 0,	/* back on the map at once (Take a Ride) */
	MP_MODE_RESPAWN_DELAY,		/* after respawnDelayFrames (not implemented yet) */
	MP_MODE_RESPAWN_NONE		/* out until the next round (not implemented yet) */
} MP_MODE_RESPAWN;

/* What a player who joins while a round is running gets. */
typedef enum
{
	MP_MODE_LATEJOIN_LIVE = 0,	/* a car at once (Take a Ride) */
	MP_MODE_LATEJOIN_SPECTATE	/* watch until the next round (not implemented yet) */
} MP_MODE_LATEJOIN;

/* Which cars the pickers may offer. Filters are named by id so a descriptor stays
 * plain data; MpModeFilterCarPool owns what each id means. */
typedef enum
{
	MP_MODE_CARPOOL_ALL = 0		/* everything the car mods offer (Take a Ride) */
} MP_MODE_CARPOOL;

/* Where a player's car is placed. */
typedef enum
{
	MP_MODE_SPAWN_SESSION = 0	/* the session's own placement: the level start, the
					 * per-player road spot, x/z only (Take a Ride) */
} MP_MODE_SPAWN;

typedef struct MP_MODE_TEAM
{
	char    name[MP_MODE_TEAM_NAME_MAX];
	uint8_t r, g, b;		/* as the colour should LOOK (plain bytes) */
} MP_MODE_TEAM;

typedef struct MP_MODE_DESC
{
	int         id;		/* MP_GAMEMODE_*: the byte on the wire */
	const char* name;	/* what browsers and the lobby show, e.g. "TAKE A RIDE" */
	int         selectable;	/* 1 = a host may pick it in the lobby */

	int          teamCount;	/* 0 = free-for-all, else 1..MP_MODE_MAX_TEAMS */
	MP_MODE_TEAM teams[MP_MODE_MAX_TEAMS];
	int          teamColours;	/* 1 = name + suit forced to the team colour */

	int respawn;		/* MP_MODE_RESPAWN_* */
	int respawnDelayFrames;	/* MP_MODE_RESPAWN_DELAY only */
	int lateJoin;		/* MP_MODE_LATEJOIN_* */
	int countdown;		/* 1 = a 3-2-1 before each round */
	int carChangeLocked;	/* 1 = no car change once a round is running */
	int carPool;		/* MP_MODE_CARPOOL_* */
	int spawn;		/* MP_MODE_SPAWN_* */
	int mapCycle;		/* 1 = the lobby may offer map cycling */
} MP_MODE_DESC;

/* ---- the registry ------------------------------------------------------- */
int                 MpModeCount(void);			/* rows in the table */
const MP_MODE_DESC* MpModeAt(int index);		/* NULL past the end */
const MP_MODE_DESC* MpModeFind(int id);			/* NULL = unknown id */
const MP_MODE_DESC* MpModeBehaviour(int id);		/* never NULL: unknown -> mode 0 */
int                 MpModeIsSelectable(int id);
int                 MpModeNextSelectable(int id, int dir);	/* lobby cycler; stays put if alone */
const char*         MpModeDisplayName(int id);		/* "-" for an unknown id */

/* ---- the decision points ------------------------------------------------- */
/* Each takes the session's gamemode id. Mode 0 answers are today's behaviour. */
int         MpModeRespawnPolicy(int mode);		/* MP_MODE_RESPAWN_* */
int         MpModeLateJoinPolicy(int mode);		/* MP_MODE_LATEJOIN_* */
int         MpModeSpawnRule(int mode);			/* MP_MODE_SPAWN_* */
int         MpModeWantsCountdown(int mode);
int         MpModeAllowsMapCycle(int mode);

/* NULL = the change may go ahead; otherwise the reason, for the log/notice.
 * `roundRunning` is whether a round is live (a session that is running). */
const char* MpModeCarChangeRefusal(int mode, int roundRunning);

/* Narrow a picker's (slot, model) list in place; returns the new count. Mode 0
 * returns `count` untouched. */
int MpModeFilterCarPool(int mode, int city, int* slots, int* models, int count);

/* The team a player is on, -1 for none. Team membership does not travel yet, so
 * this is -1 for everybody until the round state lands (docs/GAMEMODES.md). */
int MpModePlayerTeam(int mode, int playerId);

/* THE colour seam: the colour a player's name and suit are DRAWN in. In: the
 * player's own choice (on + r,g,b). Out: the effective one. Returns the effective
 * `on`. Without a team colour (mode 0) the output is the input, unchanged. */
int MpModePlayerColour(int mode, int playerId, int on, int r, int g, int b,
	int* outR, int* outG, int* outB);

#ifdef __cplusplus
}
#endif

#endif /* MP_MODE_H */
