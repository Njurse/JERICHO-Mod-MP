/*
 * test_mp_cmd.cpp -- standalone check for mp_cmd.c (listen-host command parser
 * and authority rules).
 *
 * Named .cpp, not .c, on purpose: the mod project globs **.c into the mod and
 * this file has a main().
 */
#include "mp_cmd.h"
#include "mp_proto.h"

#include <stdio.h>
#include <string.h>

static int gFails = 0;
static int gChecks = 0;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void CheckCommandDetection(void)
{
	CHECK(MpCmdIsCommand("/map chicago") == 1);
	CHECK(MpCmdIsCommand("!help") == 1);
	CHECK(MpCmdIsCommand("/restart") == 1);
	CHECK(MpCmdIsCommand("hello") == 0);
	CHECK(MpCmdIsCommand("/") == 0);
	CHECK(MpCmdIsCommand("!") == 0);
	CHECK(MpCmdIsCommand("/ map") == 0);
	CHECK(MpCmdIsCommand("//comment") == 0);
	CHECK(MpCmdIsCommand("!!bang") == 0);
	CHECK(MpCmdIsCommand("") == 0);
	CHECK(MpCmdIsCommand(NULL) == 0);
}

static void CheckParseAndAuth(void)
{
	MP_CMD_PARSED c;

	CHECK(MpCmdParse("just chatting", &c) == 0);

	CHECK(MpCmdParse("/help", &c) == 1);
	CHECK(c.kind == MP_CMD_HELP && strcmp(c.name, "help") == 0 && c.adminOnly == 0 && c.args[0] == '\0');
	CHECK(MpCmdAuthorize(1, 0, c.adminOnly) == 1);

	CHECK(MpCmdParse("!HELP map", &c) == 1);
	CHECK(c.kind == MP_CMD_HELP && strcmp(c.args, "map") == 0 && c.adminOnly == 0);

	CHECK(MpCmdParse("/MAP   vegas mp 2", &c) == 1);
	CHECK(c.kind == MP_CMD_MAP && strcmp(c.name, "map") == 0 && c.adminOnly == 1);
	CHECK(strcmp(c.args, "vegas mp 2") == 0);
	CHECK(MpCmdAuthorize(0, 0, c.adminOnly) == 1);
	CHECK(MpCmdAuthorize(1, 0, c.adminOnly) == 0);
	CHECK(MpCmdAuthorize(-1, 0, c.adminOnly) == 0);

	CHECK(MpCmdParse("!changemap rio", &c) == 1);
	CHECK(c.kind == MP_CMD_MAP && c.adminOnly == 1);

	CHECK(MpCmdParse("/mode", &c) == 1);
	CHECK(c.kind == MP_CMD_MODE && c.adminOnly == 0 && c.args[0] == '\0');
	CHECK(MpCmdAuthorize(2, 0, c.adminOnly) == 1);

	CHECK(MpCmdParse("/mode cnr 3", &c) == 1);
	CHECK(c.kind == MP_CMD_MODE && c.adminOnly == 1 && strcmp(c.args, "cnr 3") == 0);
	CHECK(MpCmdAuthorize(2, 0, c.adminOnly) == 0);
	CHECK(MpCmdAuthorize(0, 0, c.adminOnly) == 1);

	CHECK(MpCmdParse("!vote havana mp 1", &c) == 1);
	CHECK(c.kind == MP_CMD_VOTE && c.adminOnly == 0 && strcmp(c.args, "havana mp 1") == 0);

	CHECK(MpCmdParse("/vote cancel", &c) == 1);
	CHECK(c.kind == MP_CMD_VOTE && c.adminOnly == 1);
	CHECK(MpCmdAuthorize(1, 0, c.adminOnly) == 0);
	CHECK(MpCmdAuthorize(0, 0, c.adminOnly) == 1);

	CHECK(MpCmdParse("/endvote", &c) == 1);
	CHECK(c.kind == MP_CMD_ENDVOTE && c.adminOnly == 1);

	CHECK(MpCmdParse("/endround", &c) == 1);
	CHECK(c.kind == MP_CMD_ENDROUND && c.adminOnly == 1);
	CHECK(MpCmdParse("!skip", &c) == 1);
	CHECK(c.kind == MP_CMD_ENDROUND && c.adminOnly == 1);

	CHECK(MpCmdParse("/restart", &c) == 1);
	CHECK(c.kind == MP_CMD_RESTART && c.adminOnly == 1);

	CHECK(MpCmdParse("/kick Big Mouth too loud", &c) == 1);
	CHECK(c.kind == MP_CMD_KICK && c.adminOnly == 1 && strcmp(c.args, "Big Mouth too loud") == 0);

	CHECK(MpCmdParse("/mystery arg", &c) == 1);
	CHECK(c.kind == MP_CMD_NONE && strcmp(c.name, "mystery") == 0 && c.adminOnly == 0);
}

static void CheckModeParsing(void)
{
	int mode = -1, sec = -1;

	CHECK(MpCmdParseMode("0", &mode, &sec) == 1 && mode == MP_GAMEMODE_TAKEADRIDE && sec == -1);
	CHECK(MpCmdParseMode("ride", &mode, &sec) == 1 && mode == MP_GAMEMODE_TAKEADRIDE && sec == -1);
	CHECK(MpCmdParseMode("Take a Ride", &mode, &sec) == 1 && mode == MP_GAMEMODE_TAKEADRIDE && sec == -1);
	CHECK(MpCmdParseMode("fox 2", &mode, &sec) == 1 && mode == MP_GAMEMODE_FOXHOUNDS && sec == 120);
	CHECK(MpCmdParseMode("bomb 90s", &mode, &sec) == 1 && mode == MP_GAMEMODE_BOMBTAG && sec == 90);
	CHECK(MpCmdParseMode("Cops n Robbers 3", &mode, &sec) == 1 && mode == MP_GAMEMODE_COPSROBBERS && sec == 180);
	CHECK(MpCmdParseMode("cnr 45", &mode, &sec) == 1 && mode == MP_GAMEMODE_COPSROBBERS && sec == 45);
	CHECK(MpCmdParseMode("", &mode, &sec) == 0);
	CHECK(MpCmdParseMode("bogus", &mode, &sec) == 0);
	CHECK(MpCmdParseMode("99", &mode, &sec) == 0);
}

int main(void)
{
	CheckCommandDetection();
	CheckParseAndAuth();
	CheckModeParsing();
	printf("mp_cmd: %d check(s), %d failure(s)\n", gChecks, gFails);
	return gFails ? 1 : 0;
}
