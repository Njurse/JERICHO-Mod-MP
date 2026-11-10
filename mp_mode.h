#ifndef MP_MODE_H
#define MP_MODE_H

/*
 * mp_mode.h -- the game-mode registry and the questions the session asks it.
 *
 * A GAME MODE is a row in a table (mp_mode.c): a descriptor that says what the mode
 * wants -- teams, how a death is answered, what a late joiner gets, whether the
 * pre-game wait-and-countdown is on (waitMinPlayers, waitForCars,
 * firstCountdownSec, roundCountdownSec; all zero = off), whether the car can
 * change mid-round, which cars may be offered, whether the lobby may cycle maps.
 * The session never reads a descriptor
 * field directly: it asks one of the MpMode* questions below at a fixed DECISION
 * POINT, and every answer for mode 0 (Take a Ride) is what the session already did
 * before modes existed. That is the contract this file exists to keep.
 *
 * The mode id is the session's existing `gamemode` byte (MP_GAMEMODE_*, mp_proto.h),
 * which already rides MP_WELCOME, MP_SESSION (inside JPST), MP_BEACON and the master
 * listing. Nothing here changes the wire; what a player is doing in a round (state,
 * score, team) rides MP_TAG_ROUND and lives in mp_round.h.
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
	MP_MODE_CARPOOL_ALL = 0,	/* everything the car mods offer (Take a Ride) */
	MP_MODE_CARPOOL_ROLE = 1	/* by round role: the marked player (the getaway) gets
					 * non-police cars, everyone else police cars
					 * (MpModeCarPoolWantsPolice, MpModeFilterCarPoolRole) */
} MP_MODE_CARPOOL;

/* The police car's model number: Driver 2's own rule (FEmain.c carNumLookup
 * lists it as model 0 in every city; the cop the engine pings in is resident
 * model slot 3, residentModels[3] == 0). A car mod's imported police car with
 * another number is not recognised as one. */
#define MP_MODE_POLICE_MODEL	0

/* Where a player's car is placed. */
typedef enum
{
	MP_MODE_SPAWN_SESSION = 0,	/* the session's own placement: the level start, the
					 * per-player road spot, x/z only (Take a Ride) */
	MP_MODE_SPAWN_CLUSTER = 1,	/* seated as SESSION, then at every round start
					 * (MP_MARK_PHASE_COUNTDOWN) each machine puts its own
					 * car on a road near one anchor car, clear of the
					 * others, facing along its lane (mp_session.c
					 * MpMarkClusterTick) */
	MP_MODE_SPAWN_CHASE = 2		/* at every round start the MARKED player (the getaway)
					 * lands on a random road anywhere on the map, facing
					 * along it; everyone else lands on the same road
					 * BEHIND it at random distances, facing the same way
					 * (mp_session.c MpMarkChaseTick) */
} MP_MODE_SPAWN;

/* What a mode's clock does (Fox N Hounds / Car Bomb Tag, mp_mark.c). */
typedef enum
{
	MP_MODE_TIMER_NONE = 0,		/* no clock, nobody marked (Take a Ride) */
	MP_MODE_TIMER_PERSONAL,		/* a clock per player, ticking only while that player
					 * is marked; the first to 0:00 wins (Fox N Hounds) */
	MP_MODE_TIMER_SHARED,		/* one clock; whoever is marked at 0:00 loses (Car Bomb Tag) */
	MP_MODE_TIMER_GETAWAY		/* one clock; the marked player is the getaway driver,
					 * chosen at the countdown (random first, then each
					 * player in turn); no tags. 0:00 or an escape = the
					 * getaway wins; the cops' damage meter full = busted
					 * (Cops n Robbers, mp_mark.c) */
} MP_MODE_TIMER;

/* The music a mode holds while its round is live. Same numbers as jer_music.h's
 * JER_MUSIC_*, kept here as plain ids so this file stays engine-free. */
typedef enum
{
	MP_MODE_MUSIC_AUTO = 0,		/* the engine decides (Take a Ride) */
	MP_MODE_MUSIC_CALM = 1,
	MP_MODE_MUSIC_PURSUIT = 2
} MP_MODE_MUSIC;

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

	/* Pre-game wait and countdown. All zero = off (Take a Ride): the match is
	 * live as soon as the session starts, no wait, no 3-2-1.
	 *
	 * waitMinPlayers:      sit in WAITING until this many players are present
	 *                      (0 = do not wait on a head-count).
	 * waitForCars:         1 = also wait until every present player has picked
	 *                      a car. A joiner without a car holds the gate, and a
	 *                      countdown already running goes back to WAITING.
	 * firstCountdownSec:   once per map load, after the wait, an N..1 count
	 *                      then GO (0 = skip). Later rounds on the same map
	 *                      never use this.
	 * roundCountdownSec:   before every later round, a one-second GET READY
	 *                      (the start cluster) then an N..1 count then GO
	 *                      (0 = go LIVE the moment the wait ends, when no
	 *                      first-load count applies).
	 *
	 * Fox N Hounds and Car Bomb Tag opt in (2 players, wait for cars, 5 s on
	 * a fresh map, 3 s each later round). Take a Ride stays off. A new mode
	 * copies those four numbers, or leaves them 0. */
	int waitMinPlayers;
	int waitForCars;
	int firstCountdownSec;
	int roundCountdownSec;

	int carChangeLocked;	/* 1 = no car change once a round is running */
	int carPool;		/* MP_MODE_CARPOOL_* */
	int spawn;		/* MP_MODE_SPAWN_* */
	int mapCycle;		/* 1 = the lobby may offer map cycling */
	int timer;		/* MP_MODE_TIMER_*: the marked-player clock (v23) */
	int music;		/* MP_MODE_MUSIC_*: held while a round is live */
	int keepScore;		/* 1 = scores carry from round to round for the whole
				 * match (MpRoundBegin keeps them); 0 = each round
				 * starts at 0 (every mode before Cops n Robbers) */
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
int         MpModeWantsCountdown(int mode);	/* 1 = a first-load or per-round count is set */
int         MpModeWaitMinPlayers(int mode);
int         MpModeWaitForCars(int mode);
int         MpModeFirstCountdownSec(int mode);
int         MpModeRoundCountdownSec(int mode);
int         MpModeHasPreGame(int mode);		/* 1 = any wait or countdown is set */
int         MpModeAllowsMapCycle(int mode);
int         MpModeTimerKind(int mode);			/* MP_MODE_TIMER_* */
int         MpModeUsesTimer(int mode);			/* 1 = marked player + clock (JPMK) */
int         MpModeMusic(int mode);			/* MP_MODE_MUSIC_* while live */
int         MpModeKeepsScore(int mode);		/* 1 = scores carry across rounds */

/* MP_MODE_CARPOOL_ROLE: 1 = this role drives police cars, 0 = non-police cars;
 * -1 = the mode has no role pool (any car). `isMarked` = the player is the
 * marked one (the getaway driver). */
int         MpModeCarPoolWantsPolice(int mode, int isMarked);

/* NULL = the change may go ahead; otherwise the reason, for the log/notice.
 * `roundRunning` is whether a round is live (a session that is running). */
const char* MpModeCarChangeRefusal(int mode, int roundRunning);

/* Narrow a picker's (slot, model) list in place; returns the new count. Mode 0
 * returns `count` untouched. */
int MpModeFilterCarPool(int mode, int city, int* slots, int* models, int count);

/* MP_MODE_CARPOOL_ROLE: keep only the cars for this role (police for a cop,
 * anything else for the getaway), in place, order kept. A list the filter would
 * EMPTY (no police car offered) comes back untouched: the session then swaps the
 * car itself at the round start. Other pools: untouched. Returns the count. */
int MpModeFilterCarPoolRole(int mode, int isMarked, int* slots, int* models, int count);

/* The team a player is on, -1 for none. Read from the round state (mp_round.h,
 * MP_TAG_ROUND); always -1 for a mode with no teams, such as Take a Ride, and for
 * a team index the mode does not have. */
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
