/*
 * mp_cmd.c -- pure listen-host chat command parser and authority check.
 */
#include "mp_cmd.h"
#include "mp_mode.h"
#include "mp_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int MpCmdEq(const char* a, const char* b)
{
	return strcmp(a, b) == 0;
}

static const char* MpCmdSkipWs(const char* s)
{
	while (s != NULL && (*s == ' ' || *s == '\t'))
		s++;
	return s;
}

int MpCmdIsCommand(const char* text)
{
	return text != NULL && (text[0] == '!' || text[0] == '/') && text[1] != '\0'
		&& text[1] != ' ' && text[1] != '\t' && text[1] != '!' && text[1] != '/';
}

int MpCmdParse(const char* text, MP_CMD_PARSED* out)
{
	const char* p;
	size_t n = 0;

	if (out != NULL)
		memset(out, 0, sizeof(*out));

	if (!MpCmdIsCommand(text))
		return 0;

	p = text + 1;

	while (*p != '\0' && *p != ' ' && *p != '\t')
	{
		if (out != NULL && n + 1 < sizeof(out->name))
			out->name[n++] = (char)tolower((unsigned char)*p);
		p++;
	}

	if (out == NULL)
		return 1;

	out->name[n] = '\0';
	p = MpCmdSkipWs(p);
	out->args = (p != NULL) ? p : "";

	if (MpCmdEq(out->name, "help") || MpCmdEq(out->name, "?"))
	{
		out->kind = MP_CMD_HELP;
		out->adminOnly = 0;
	}
	else if (MpCmdEq(out->name, "vote"))
	{
		out->kind = MP_CMD_VOTE;
		/* `!vote cancel` is admin-only; everything else in !vote is open */
		out->adminOnly = (strncmp(out->args, "cancel", 6) == 0 &&
			(out->args[6] == '\0' || out->args[6] == ' ' || out->args[6] == '\t')) ? 1 : 0;
	}
	else if (MpCmdEq(out->name, "map") || MpCmdEq(out->name, "changemap"))
	{
		out->kind = MP_CMD_MAP;
		out->adminOnly = 1;
	}
	else if (MpCmdEq(out->name, "mode") || MpCmdEq(out->name, "gamemode"))
	{
		out->kind = MP_CMD_MODE;
		/* bare !mode shows the current mode to anyone; switching requires admin */
		out->adminOnly = (out->args[0] != '\0') ? 1 : 0;
	}
	else if (MpCmdEq(out->name, "endvote") || MpCmdEq(out->name, "cancelvote"))
	{
		out->kind = MP_CMD_ENDVOTE;
		out->adminOnly = 1;
	}
	else if (MpCmdEq(out->name, "endround") || MpCmdEq(out->name, "skipround") || MpCmdEq(out->name, "skip"))
	{
		out->kind = MP_CMD_ENDROUND;
		out->adminOnly = 1;
	}
	else if (MpCmdEq(out->name, "restart") || MpCmdEq(out->name, "restartround"))
	{
		out->kind = MP_CMD_RESTART;
		out->adminOnly = 1;
	}
	else if (MpCmdEq(out->name, "kick"))
	{
		out->kind = MP_CMD_KICK;
		out->adminOnly = 1;
	}
	else
	{
		out->kind = MP_CMD_NONE;
		out->adminOnly = 0;
	}

	return 1;
}

int MpCmdAuthorize(int callerPlayerId, int hostPlayerId, int adminOnly)
{
	if (!adminOnly)
		return 1;
	return callerPlayerId >= 0 && hostPlayerId >= 0 && callerPlayerId == hostPlayerId;
}

static int MpCmdParseDuration(const char* tok, int* outSeconds)
{
	char* end = NULL;
	long val;

	if (tok == NULL || tok[0] == '\0' || !isdigit((unsigned char)tok[0]))
		return 0;

	val = strtol(tok, &end, 10);
	if (end == tok)
		return 0;

	if ((*end == 's' || *end == 'S') && end[1] == '\0')
	{
		if (val < MP_MARK_SECONDS_MIN) val = MP_MARK_SECONDS_MIN;
		if (val > MP_MARK_SECONDS_MAX) val = MP_MARK_SECONDS_MAX;
		*outSeconds = (int)val;
		return 1;
	}

	if ((*end == 'm' || *end == 'M') && end[1] == '\0')
	{
		if (val < MP_MARK_MINUTES_MIN) val = MP_MARK_MINUTES_MIN;
		if (val > MP_MARK_MINUTES_MAX) val = MP_MARK_MINUTES_MAX;
		*outSeconds = (int)val * 60;
		return 1;
	}

	if (*end != '\0')
		return 0;

	if (val >= MP_MARK_MINUTES_MIN && val <= MP_MARK_MINUTES_MAX)
	{
		*outSeconds = (int)val * 60;
		return 1;
	}

	if (val >= MP_MARK_SECONDS_MIN)
	{
		if (val > MP_MARK_SECONDS_MAX) val = MP_MARK_SECONDS_MAX;
		*outSeconds = (int)val;
		return 1;
	}

	return 0;
}

static void MpCmdNorm(const char* in, char* out, size_t cap)
{
	size_t n = 0;

	while (*in != '\0' && n + 1 < cap)
	{
		unsigned char c = (unsigned char)*in++;
		if (isalpha(c))
			out[n++] = (char)tolower(c);
	}
	out[n] = '\0';
}

int MpCmdParseMode(const char* args, int* outMode, int* outSeconds)
{
	char buf[96];
	char* words[8];
	int nw = 0;
	int sec = -1;
	int gid = -1;
	char joined[96];
	char norm[96];
	int i;

	if (outMode == NULL || outSeconds == NULL || args == NULL)
		return 0;

	snprintf(buf, sizeof(buf), "%s", args);
	{
		char* p = buf;
		while (*p != '\0' && nw < 8)
		{
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '\0')
				break;
			words[nw++] = p;
			while (*p != '\0' && *p != ' ' && *p != '\t')
				p++;
			if (*p != '\0')
				*p++ = '\0';
		}
	}

	if (nw == 0)
		return 0;

	if (nw >= 2 && MpCmdParseDuration(words[nw - 1], &sec))
		nw--;

	joined[0] = '\0';
	for (i = 0; i < nw; i++)
	{
		if (i > 0 && strlen(joined) + 1 < sizeof(joined))
			strcat(joined, " ");
		if (strlen(joined) + strlen(words[i]) < sizeof(joined))
			strcat(joined, words[i]);
	}

	if (nw == 1 && isdigit((unsigned char)joined[0]))
	{
		char* end = NULL;
		long v = strtol(joined, &end, 10);
		if (end != joined && *end == '\0')
			gid = (int)v;
	}

	if (gid < 0)
	{
		MpCmdNorm(joined, norm, sizeof(norm));

		if (MpCmdEq(norm, "takearide") || MpCmdEq(norm, "ride") || MpCmdEq(norm, "free") ||
			MpCmdEq(norm, "freeroam") || MpCmdEq(norm, "tar"))
			gid = MP_GAMEMODE_TAKEADRIDE;
		else if (MpCmdEq(norm, "foxnhounds") || MpCmdEq(norm, "foxandhounds") || MpCmdEq(norm, "fox") ||
			MpCmdEq(norm, "foxhounds") || MpCmdEq(norm, "fnh") || MpCmdEq(norm, "hounds"))
			gid = MP_GAMEMODE_FOXHOUNDS;
		else if (MpCmdEq(norm, "carbombtag") || MpCmdEq(norm, "bomb") || MpCmdEq(norm, "bombtag") ||
			MpCmdEq(norm, "cbt") || MpCmdEq(norm, "tag"))
			gid = MP_GAMEMODE_BOMBTAG;
		else if (MpCmdEq(norm, "copsnrobbers") || MpCmdEq(norm, "copsandrobbers") || MpCmdEq(norm, "cops") ||
			MpCmdEq(norm, "copsrobbers") || MpCmdEq(norm, "cnr") || MpCmdEq(norm, "robbers"))
			gid = MP_GAMEMODE_COPSROBBERS;
	}

	if (!MpModeIsSelectable(gid))
		return 0;

	*outMode = gid;
	*outSeconds = sec;
	return 1;
}
