/* Configuration, module identity and the build/manifest hashes.
 *
 * Split out of mp.c: that file is the module's engine-facing entry point and had
 * grown into a grab-bag of everything the module owns. */
#include "jericho.h"
#include "jer_config.h"

/* A stored colour must survive being hand-edited: 0..255 or it is not a colour. */
#define MP_CLAMP_CONFIG_COLOR(v) do { if ((v) < 0) (v) = 0; if ((v) > 255) (v) = 255; } while (0)
#include "mp.h"
#include "driver2.h"
#include "system.h"		/* CITY_COUNT: car_city is a CITY, not a Driver 2 level */

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

	/* Silence that drops a peer. 0 = never drop: for playtesting, so a stall (a
	 * freeze, a long pause, a machine busy in another window) no longer ejects
	 * the other seat. Default is the old 30 s grace. */
	gMp.config.idleDropMs = jer_config_get_int("mp", "idle_drop_ms", MP_CONN_TIMEOUT_MS);
	if (gMp.config.idleDropMs < 0 || gMp.config.idleDropMs > 600000)
		gMp.config.idleDropMs = MP_CONN_TIMEOUT_MS;

	gMp.config.car = jer_config_get_int("mp", "car", -1);
	if (gMp.config.car < -1 || gMp.config.car > 255)
		gMp.config.car = -1;

	/* The city `car`'s model number belongs to, for a cross-city pick (a slot is
	 * always the session's city, so it does not apply then). -1 = the session's. */
	gMp.config.carCity = jer_config_get_int("mp", "car_city", -1);
	if (gMp.config.carCity < -1 || gMp.config.carCity >= CITY_COUNT)
		gMp.config.carCity = -1;

	/* 1 = `car` is a 1..10 frontend SLOT to resolve per city, not a model number
	 * (set by `-mpcar slotN`, or by the car menu). Without this the choice does not
	 * survive a restart: the slot number would be read back as a model. */
	gMp.config.carIsSlot = jer_config_get_int("mp", "car_is_slot", 0) ? 1 : 0;

	gMp.config.modCheck = jer_config_get_int("mp", "mod_check", MP_MODCHECK_OFF);
	if (gMp.config.modCheck < 0 || gMp.config.modCheck > MP_MODCHECK_EXACT)
		gMp.config.modCheck = MP_MODCHECK_OFF;

	gMp.config.strictVersion = jer_config_get_int("mp", "strict_version", 0);

	/* COLOUR. Off by default, and that default is deliberate: see MP_CONFIG. */
	gMp.config.colorOn = jer_config_get_int("mp", "custom_color", 0);
	gMp.config.colorR = jer_config_get_int("mp", "color_r", 255);
	gMp.config.colorG = jer_config_get_int("mp", "color_g", 255);
	gMp.config.colorB = jer_config_get_int("mp", "color_b", 255);

	gMp.config.colorOn = gMp.config.colorOn ? 1 : 0;
	MP_CLAMP_CONFIG_COLOR(gMp.config.colorR);
	MP_CLAMP_CONFIG_COLOR(gMp.config.colorG);
	MP_CLAMP_CONFIG_COLOR(gMp.config.colorB);
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
	jer_config_set_int("mp", "idle_drop_ms", gMp.config.idleDropMs);
	jer_config_set_int("mp", "mod_check", gMp.config.modCheck);
	jer_config_set_int("mp", "car", gMp.config.car);
	jer_config_set_int("mp", "car_city", gMp.config.carCity);
	jer_config_set_int("mp", "car_is_slot", gMp.config.carIsSlot);
	jer_config_set_int("mp", "strict_version", gMp.config.strictVersion);

	jer_config_set_int("mp", "custom_color", gMp.config.colorOn);
	jer_config_set_int("mp", "color_r", gMp.config.colorR);
	jer_config_set_int("mp", "color_g", gMp.config.colorG);
	jer_config_set_int("mp", "color_b", gMp.config.colorB);
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

/* The RELEASE SERIES of the build string, which is what "the same build" has to
 * mean for two peers to be able to play together at all.
 *
 * JERICHO_BUILD_VERSION is `git describe --tags --always --dirty`, so one
 * release reads differently depending on who built it and how:
 *
 *   "0.9.0"             the tagged release
 *   "v0.9.0"            the same, before the leading v is stripped
 *   "0.9.0-dirty"       CI, when the index stat cache was stale
 *   "0.9.0-3-gabc1234"  a tree three commits past the tag
 *
 * Hashing THAT refused every pair that was not identical in provenance - which
 * is every cross-platform pair not built in the same CI run, and every pair
 * where one side built the game locally. The series is the leading version, and
 * ONLY when there really is one:
 *
 *   "0.9.0"             -> "0.9.0"
 *   "v0.9.0"            -> "0.9.0"
 *   "0.9.0-dirty"       -> "0.9.0"
 *   "0.9.0-3-gabc1234"  -> "0.9.0"
 *   "8.0-978-gf36979a7" -> "8.0"
 *   "alpha-2-g26b6fa4a" -> "alpha-2-g26b6fa4a"   no version to compare
 *   "217642a"           -> "217642a"             bare sha, no tags were fetched
 *
 * A string with no version in front is left WHOLE, so strict stays as strict as
 * it can be when there is no release to be strict about: the rolling
 * prerelease's alpha tag, or a tree with no tags at all, must still match
 * exactly.
 *
 * Only the HOST applies this (see the hello handler in mp_session.c), so a peer
 * running an older build still sends a hash of the raw string and will keep
 * refusing - both sides need a build that knows what a series is. */
void MpBuildSeries(char* out, size_t outSize)
{
	const char* s = JERICHO_BUILD_VERSION;
	size_t i = 0;

	if (out == NULL || outSize == 0)
		return;

	if (s == NULL)
		s = "";

	if (*s == 'v' || *s == 'V')	/* release tags are spelled v0.9.0 */
		s++;

	while (i + 1 < outSize && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.'))
		i++;

	if (i > 0 && s[0] >= '0' && s[0] <= '9' && memchr(s, '.', i) != NULL)
	{
		memcpy(out, s, i);
		out[i] = '\0';
		return;
	}

	/* no version to compare: keep the whole string, so this stays exact */
	snprintf(out, outSize, "%s", JERICHO_BUILD_VERSION);
}

/* Digest of the RELEASE SERIES, so peers on the same release are admitted
 * whatever their platform or build provenance, and peers on a different
 * release are refused before anything else happens. */
unsigned short MpBuildHash(void)
{
	char series[64];
	const char* s;
	unsigned int h = 2166136261u;

	MpBuildSeries(series, sizeof(series));

	for (s = series; *s != '\0'; s++)
	{
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}

	return (unsigned short)((h ^ (h >> 16)) & 0xFFFF);
}
