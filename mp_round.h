#ifndef MP_ROUND_H
#define MP_ROUND_H

/*
 * mp_round.h -- the round lifecycle and the per-player round state (MP_TAG_ROUND).
 *
 * ONE copy of the round state per machine. On the HOST (a listen host here, or the
 * dedicated server's Python twin, dedi/roundstate.py) it is the authority: the host
 * begins and ends rounds, tracks who is present, and builds the 'JPRD' snapshot.
 * On a CLIENT it is a mirror: each snapshot replaces it whole, and nothing on a
 * client writes it any other way.
 *
 * What it carries (mp_proto.h MP_ROUND / MP_ROUND_ENTRY):
 *   - the round number (1 = the match's first round, 0 = none yet) and its phase,
 *     MP_ROUND_PHASE_IDLE / _LIVE / _ENDED;
 *   - per player id: present or not, MP_ROUND_PLAYER_* state, score, team.
 *
 * THE TAKE A RIDE ANSWER is the default everywhere: a player with no row, or a
 * client that has not had a snapshot yet, reads as playing, no team (-1), score 0.
 * Take a Ride's match is one round, live from START until the session ends.
 *
 * Pure on purpose, like mp_mode.c: no engine headers, no session state, no logging
 * (the session logs from what these return), so a standalone check can build it
 * (tools/mode_check/test_mp_round.cpp) and the Python servers can mirror it.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle: the host --------------------------------------------------- */
void MpRoundReset(void);		/* a new session: idle, round 0, nobody present */

/* Start the next round of `mode`: round number + 1, phase LIVE, every present
 * player MP_ROUND_PLAYER_PLAYING with score 0 and the mode's team (none for a
 * mode without teams). */
void MpRoundBegin(int mode);

void MpRoundEnd(void);			/* phase ENDED; players and scores are kept */

/* Add `points` to a present player's score (host only; a mode's referee says how
 * many). Returns 1 when the row exists. MpRoundBegin zeroes scores unless the
 * mode keeps them (MpModeKeepsScore: Cops n Robbers). */
int MpRoundAddScore(int playerId, int points);

/* Make the present-set match `present[id]` (MP_MAX_PLAYERS entries, nonzero =
 * in the session). A player who arrives gets a row: during a LIVE round the state
 * the mode's late-join policy gives (Take a Ride: playing), otherwise WAITING.
 * A player who leaves loses theirs. Returns 1 when anything changed. */
int MpRoundSyncPresent(const int* present, int mode);

/* v24, host: a present player's state (MP_ROUND_PLAYER_*: SPECTATING when the
 * mode's no-respawn put them out), and the ADMIN bit (MP_ROUND_ENTRY_F_ADMIN: a
 * listen host is its own admin; a dedicated server sets its admins). An arrival
 * starts with no flags. Return 1 when anything changed. */
int MpRoundSetState(int playerId, int state);
int MpRoundSetAdmin(int playerId, int admin);

/* The snapshot to send: header + one row per present player, in ascending id.
 * Returns the byte count written, 0 if `cap` is too small. */
int MpRoundBuild(unsigned char* out, int cap);

/* Bumped by every change above, so the host can send "whenever it changes". */
unsigned MpRoundRevision(void);

/* ---- the client ------------------------------------------------------------ */
/* Replace the local copy with a host snapshot. Returns 1 when taken; 0 when the
 * frame is short or its count is out of range (the copy is left as it was). */
int MpRoundApply(const unsigned char* p, int len);

/* ---- what anyone may ask --------------------------------------------------- */
int MpRoundHaveState(void);		/* 1 = a host built or sent one this session */
int MpRoundMode(void);			/* the mode in the last state (0 before any) */
int MpRoundNumber(void);		/* 0 = no round yet */
int MpRoundPhase(void);			/* MP_ROUND_PHASE_* */
int MpRoundIsLive(void);
int MpRoundPlayerPresent(int playerId);	/* 1 = has a row in the round state */
int MpRoundPlayerState(int playerId);	/* MP_ROUND_PLAYER_*; PLAYING when unknown */
int MpRoundPlayerScore(int playerId);	/* 0 when unknown */
int MpRoundPlayerTeam(int playerId);	/* team index, -1 for none / unknown */
int MpRoundPlayerIsAdmin(int playerId);	/* v24: the host says they are an admin */

#ifdef __cplusplus
}
#endif

#endif /* MP_ROUND_H */
