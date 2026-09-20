/* Configuration, module identity and the build/manifest hashes.
 *
 * Split out of mp.c: that file is the module's engine-facing entry point and had
 * grown into a grab-bag of everything the module owns. */
#include "jericho.h"
#include "jer_config.h"
#include "mp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const char* MpDefaultPlayerName(void)
{
	static char name[MP_NAME_MAX];
	const char* env = getenv("USERNAME");

	if (env == NULL || env[0] == '\0')
		env = getenv("USER");

	if (env != NULL && env[0] != '\0')
		snprintf(name, sizeof(name), "%s", env);
	else
		snprintf(name, sizeof(name), "Player");

	return name;
}

void MpConfigLoad(void)
{
	const char* s;

	memset(&gMp.config, 0, sizeof(gMp.config));

	gMp.config.port = jer_config_get_int("mp", "port", MP_DEFAULT_PORT);
	if (gMp.config.port < 1024 || gMp.config.port > 65535)
		gMp.config.port = MP_DEFAULT_PORT;

	gMp.config.beaconMs = jer_config_get_int("mp", "beacon_ms", MP_BEACON_INTERVAL_MS);
	if (gMp.config.beaconMs < 250 || gMp.config.beaconMs > 10000)
		gMp.config.beaconMs = MP_BEACON_INTERVAL_MS;

	gMp.config.keepaliveMs = jer_config_get_int("mp", "keepalive_ms", MP_KEEPALIVE_INTERVAL_MS);
	if (gMp.config.keepaliveMs < MP_KEEPALIVE_MIN_MS || gMp.config.keepaliveMs > 30000)
		gMp.config.keepaliveMs = MP_KEEPALIVE_INTERVAL_MS;

	gMp.config.modCheck = jer_config_get_int("mp", "mod_check", MP_MODCHECK_OFF);
	if (gMp.config.modCheck < 0 || gMp.config.modCheck > MP_MODCHECK_EXACT)
		gMp.config.modCheck = MP_MODCHECK_OFF;

	gMp.config.strictVersion = jer_config_get_int("mp", "strict_version", 0);
	if (gMp.config.strictVersion < 0 || gMp.config.strictVersion > 1)
		gMp.config.strictVersion = 0;

	/* An empty default means "not set yet" -> first run: prompt for a name. */
	s = jer_config_get_str("mp", "player_name", "");
	if (s != NULL && s[0] != '\0')
	{
		snprintf(gMp.config.playerName, sizeof(gMp.config.playerName), "%s", s);
		gMp.config.firstNameSet = 1;
	}
	else
	{
		snprintf(gMp.config.playerName, sizeof(gMp.config.playerName), "%s", MpDefaultPlayerName());
		gMp.config.firstNameSet = 0;
	}

	s = jer_config_get_str("mp", "host_name", "");
	if (s != NULL && s[0] != '\0')
		snprintf(gMp.config.hostName, sizeof(gMp.config.hostName), "%s", s);
	else
		snprintf(gMp.config.hostName, sizeof(gMp.config.hostName), "%s's game", gMp.config.playerName);
}

void MpConfigSave(void)
{
	jer_config_set_int("mp", "port", gMp.config.port);
	jer_config_set_int("mp", "beacon_ms", gMp.config.beaconMs);
	jer_config_set_int("mp", "keepalive_ms", gMp.config.keepaliveMs);
	jer_config_set_int("mp", "mod_check", gMp.config.modCheck);
	jer_config_set_int("mp", "strict_version", gMp.config.strictVersion);
	jer_config_set_str("mp", "player_name", gMp.config.playerName);
	jer_config_set_str("mp", "host_name", gMp.config.hostName);
}

/* A short digest of the enabled-module manifest (id + version), used in
 * beacons and the handshake so mismatched lobbies are visible up front. */
unsigned short MpModHash(void)
{
	JER_MODULE_INFO info[32];
	int n = jer_module_list(info, 32);
	int i;
	unsigned int h = 2166136261u;	/* FNV-1a */

	for (i = 0; i < n; i++)
	{
		const char* s;

		if (!info[i].enabled)
			continue;

		for (s = info[i].id; s != NULL && *s != '\0'; s++)
		{
			h ^= (unsigned char)*s;
			h *= 16777619u;
		}

		h ^= '@';
		h *= 16777619u;

		for (s = info[i].version; s != NULL && *s != '\0'; s++)
		{
			h ^= (unsigned char)*s;
			h *= 16777619u;
		}

		h ^= ';';
		h *= 16777619u;
	}

	return (unsigned short)((h ^ (h >> 16)) & 0xFFFF);
}

/* The enabled-module manifest (id + version) exchanged in the handshake. */
int MpBuildManifest(MP_MOD_INFO* out, int max)
{
	JER_MODULE_INFO info[32];
	int n = jer_module_list(info, 32);
	int i, count = 0;

	for (i = 0; i < n && count < max; i++)
	{
		if (!info[i].enabled)
			continue;

		memset(&out[count], 0, sizeof(out[count]));
		snprintf(out[count].id, MP_MOD_ID_MAX, "%s", info[i].id != NULL ? info[i].id : "");
		snprintf(out[count].version, MP_MOD_VER_MAX, "%s", info[i].version != NULL ? info[i].version : "");
		out[count].enabled = 1;
		count++;
	}

	return count;
}

/* Digest of the game build (JERICHO_BUILD_VERSION) so peers on different
 * exes are refused before anything else happens. */
unsigned short MpBuildHash(void)
{
	const char* s = JERICHO_BUILD_VERSION;
	unsigned int h = 2166136261u;

	for (; s != NULL && *s != '\0'; s++)
	{
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}

	return (unsigned short)((h ^ (h >> 16)) & 0xFFFF);
}
