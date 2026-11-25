#ifndef MP_CMD_H
#define MP_CMD_H

/*
 * mp_cmd.h -- pure listen-host chat command parser and authority check.
 *
 * Pure on purpose (no engine headers, no session state) so it can be unit-tested
 * standalone by tools/mode_check/test_mp_cmd.cpp alongside mp_mode.c and mp_mark.c.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	MP_CMD_NONE = 0,
	MP_CMD_HELP,
	MP_CMD_VOTE,
	MP_CMD_MAP,
	MP_CMD_MODE,
	MP_CMD_ENDVOTE,
	MP_CMD_ENDROUND,
	MP_CMD_RESTART,
	MP_CMD_KICK
} MP_CMD_KIND;

typedef struct MP_CMD_PARSED
{
	MP_CMD_KIND kind;
	char        name[24];
	const char* args;	/* points into input text after command + spaces, or "" */
	int         adminOnly;	/* 1 = only the host (listen) or an admin may run it */
} MP_CMD_PARSED;

/* 1 when `text` starts with a command prefix ('!' or '/') followed by a word. */
int MpCmdIsCommand(const char* text);

/* Parse `text` into `out`. Returns 1 when `text` is a command (even unknown ->
 * MP_CMD_NONE with `out->name` filled), 0 when `text` is normal chat. */
int MpCmdParse(const char* text, MP_CMD_PARSED* out);

/* Listen-server authority: 1 when `!adminOnly`, or when `callerPlayerId >= 0`
 * matches `hostPlayerId`. */
int MpCmdAuthorize(int callerPlayerId, int hostPlayerId, int adminOnly);

/* Parse `<id|name> [minutes|seconds]` into a selectable mode id (0..3) and
 * optional round duration in seconds (-1 if omitted). Returns 1 on success. */
int MpCmdParseMode(const char* args, int* outMode, int* outSeconds);

#ifdef __cplusplus
}
#endif

#endif /* MP_CMD_H */
