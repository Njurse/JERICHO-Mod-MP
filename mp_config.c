/* Configuration, module identity and the build/manifest hashes.
 *
 * Split out of mp.c: that file is the module's engine-facing entry point and had
 * grown into a grab-bag of everything the module owns. */
#if defined(_WIN32)
#define _CRT_RAND_S	/* rand_s: the OS CSPRNG, for the player id */
#endif
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
#if defined(_WIN32)
#include <direct.h>	/* _mkdir */
#else
#include <sys/stat.h>	/* mkdir */
#endif

/* The per-install player id: 16 random bytes, generated once and kept as 32 hex
 * digits in [mp] player_id. A dedicated server recognises a player by it (its
 * admins list), so it comes from the OS generator, not rand(). It is a bearer
 * secret: whoever copies the config is that player to a server. */
static int MpOsRandom(unsigned char* out, int n)
{
#if defined(_WIN32)
	int i;

	for (i = 0; i < n; i += 4)
	{
		unsigned int r;
		int k;

		if (rand_s(&r) != 0)
			return 0;

		for (k = 0; k < 4 && i + k < n; k++)
			out[i + k] = (unsigned char)(r >> (k * 8));
	}

	return 1;
#else
	FILE* f = fopen("/dev/urandom", "rb");
	int ok;

	if (f == NULL)
		return 0;

	ok = (fread(out, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	return ok;
#endif
}

static int MpHexNibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* The id as a standard dashed UUID (8-4-4-4-12), the form every tool shows. */
void MpFormatPlayerId(const unsigned char* id, char* out /* >= 37 */)
{
	static const int dash[] = { 4, 6, 8, 10 };
	int i, k = 0, d = 0;

	for (i = 0; i < MP_PLAYER_ID_LEN; i++)
	{
		if (d < 4 && i == dash[d])
		{
			out[k++] = '-';
			d++;
		}

		snprintf(out + k, 3, "%02x", id[i]);
		k += 2;
	}

	out[k] = '\0';
}

/* ---- the identity file: <root>/CONFIG/player.id ------------------------------
 *
 * 32 bytes, little-endian:
 *    0  4  magic "JRID"
 *    4  1  version (1)
 *    5  3  zero
 *    8  4  nonce (random per write)
 *   12 16  the player id, XORed with a keystream (splitmix64 seeded by
 *          MP_ID_KEY ^ nonce, 8 bytes per step, low byte first)
 *   28  4  check: FNV-1a 32 over MP_ID_KEY's 8 bytes, bytes 0..11, then the PLAIN id
 *
 * The key ships in the game, so this is obfuscation and tamper detection (a
 * hand-edited or damaged file fails the check), not secrecy. It is deliberately
 * NOT bound to the machine: copying the CONFIG folder or reinstalling keeps the id.
 * tools/player_id.py reads the same format. */
#define MP_ID_FILE	"player.id"
#define MP_ID_FILE_SIZE	32
#define MP_ID_KEY	0x4A45524943484F31ULL	/* "JERICHO1" */
#ifndef JER_CONFIG_PATH
#define JER_CONFIG_PATH	"CONFIG"
#endif

static unsigned long long MpSplitMix(unsigned long long* x)
{
	unsigned long long z = (*x += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static void MpIdCrypt(unsigned char* id, unsigned int nonce)
{
	unsigned long long st = MP_ID_KEY ^ (unsigned long long)nonce, r = 0;
	int i;

	for (i = 0; i < MP_PLAYER_ID_LEN; i++)
	{
		if ((i & 7) == 0)
			r = MpSplitMix(&st);
		id[i] ^= (unsigned char)(r >> ((i & 7) * 8));
	}
}

static unsigned int MpIdCheck(const unsigned char* head12, const unsigned char* plain)
{
	unsigned int h = 2166136261u;
	int i;

	for (i = 0; i < 8; i++)
		h = (h ^ (unsigned char)(MP_ID_KEY >> (i * 8))) * 16777619u;
	for (i = 0; i < 12; i++)
		h = (h ^ head12[i]) * 16777619u;
	for (i = 0; i < MP_PLAYER_ID_LEN; i++)
		h = (h ^ plain[i]) * 16777619u;
	return h;
}

static void MpIdPath(char* out, size_t n, const char* suffix)
{
	const char* root = jer_root_dir();

	snprintf(out, n, "%s/%s/%s%s", (root != NULL && root[0]) ? root : ".", JER_CONFIG_PATH,
		MP_ID_FILE, suffix);
}

static void MpPut32(unsigned char* p, unsigned int v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static unsigned int MpGet32(const unsigned char* p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
		((unsigned int)p[3] << 24);
}

/* 1 = read a valid id, 0 = no file, -1 = a file that is not a valid identity */
static int MpIdFileRead(unsigned char* id)
{
	char path[512];
	unsigned char b[MP_ID_FILE_SIZE + 1];
	FILE* f;
	size_t n;

	MpIdPath(path, sizeof(path), "");
	f = fopen(path, "rb");

	if (f == NULL)
		return 0;

	n = fread(b, 1, sizeof(b), f);
	fclose(f);

	if (n != MP_ID_FILE_SIZE || memcmp(b, "JRID", 4) != 0 || b[4] != 1)
		return -1;

	memcpy(id, b + 12, MP_PLAYER_ID_LEN);
	MpIdCrypt(id, MpGet32(b + 8));

	return (MpIdCheck(b, id) == MpGet32(b + 28)) ? 1 : -1;
}

/* Atomic: written to player.id.tmp, then renamed over. 1 = written. */
static int MpIdFileWrite(const unsigned char* id)
{
	char path[512], tmp[520];
	unsigned char b[MP_ID_FILE_SIZE];
	unsigned int nonce = 0;
	FILE* f;
	int ok;

	memset(b, 0, sizeof(b));
	memcpy(b, "JRID", 4);
	b[4] = 1;

	if (!MpOsRandom((unsigned char*)&nonce, 4))
		nonce = (unsigned int)MpNowMs() ^ 0x5A5A1234u;

	MpPut32(b + 8, nonce);
	memcpy(b + 12, id, MP_PLAYER_ID_LEN);
	MpPut32(b + 28, MpIdCheck(b, id));
	MpIdCrypt(b + 12, nonce);

	MpIdPath(path, sizeof(path), "");
	MpIdPath(tmp, sizeof(tmp), ".tmp");

	f = fopen(tmp, "wb");
	if (f == NULL)
	{
		/* CONFIG/ may not exist yet on a fresh install: make it and retry */
		char dir[512];
		const char* root = jer_root_dir();

		snprintf(dir, sizeof(dir), "%s/%s", (root != NULL && root[0]) ? root : ".", JER_CONFIG_PATH);
#if defined(_WIN32)
		_mkdir(dir);
#else
		mkdir(dir, 0755);
#endif
		f = fopen(tmp, "wb");
		if (f == NULL)
			return 0;
	}

	ok = (fwrite(b, 1, sizeof(b), f) == sizeof(b));
	ok = (fflush(f) == 0) && ok;
	fclose(f);

	if (!ok)
	{
		remove(tmp);
		return 0;
	}

#if defined(_WIN32)
	remove(path);	/* rename() will not replace on Windows */
#endif
	if (rename(tmp, path) != 0)
	{
		remove(tmp);
		return 0;
	}

	return 1;
}

static int MpParseIdText(const char* s, unsigned char* out)
{
	int i, n = 0;

	for (i = 0; s != NULL && s[i] != '\0'; i++)
	{
		int v;

		if (s[i] == '-')
			continue;

		v = MpHexNibble(s[i]);

		if (v < 0 || n >= MP_PLAYER_ID_LEN * 2)
			return 0;

		if ((n & 1) == 0)
			out[n / 2] = (unsigned char)(v << 4);
		else
			out[n / 2] |= (unsigned char)v;
		n++;
	}

	return n == MP_PLAYER_ID_LEN * 2;
}

/* Loaded (or created) at module activation, i.e. at game launch, so the id exists
 * before any multiplayer use. Lives in CONFIG/player.id (above). Order:
 *   1. a valid player.id                         -> use it
 *   2. a player.id that fails its check          -> keep it as player.id.bad, log
 *   3. the old [mp] player_id ini key (earlier builds) -> import it, same id
 *   4. otherwise                                  -> a new random (v4) id
 * then (re)write player.id if it was not valid, and blank the old ini key once the
 * file holds the id. */
static void MpLoadPlayerId(void)
{
	char text[40], path[512], bad[520];
	const char* how = "";
	int st = MpIdFileRead(gMp.config.playerId);

	if (st < 0)
	{
		MpIdPath(path, sizeof(path), "");
		MpIdPath(bad, sizeof(bad), ".bad");
		remove(bad);
		rename(path, bad);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] %s is damaged or not an identity file: kept as %s\n",
				path, bad);
	}

	if (st != 1)
	{
		const char* old = jer_config_get_str("mp", "player_id", "");

		if (old != NULL && old[0] != '\0' && MpParseIdText(old, gMp.config.playerId))
			how = " (imported from [mp] player_id)";
		else
		{
			int i;

			if (!MpOsRandom(gMp.config.playerId, MP_PLAYER_ID_LEN))
			{
				/* no OS generator: still unique enough to tell installs apart */
				srand((unsigned int)(size_t)&text ^ (unsigned int)(size_t)&gMp ^ (unsigned int)MpNowMs());
				for (i = 0; i < MP_PLAYER_ID_LEN; i++)
					gMp.config.playerId[i] = (unsigned char)(rand() >> 3);
			}

			how = " (new for this install)";
		}

		gMp.config.playerId[6] = (unsigned char)((gMp.config.playerId[6] & 0x0F) | 0x40);	/* version 4 */
		gMp.config.playerId[8] = (unsigned char)((gMp.config.playerId[8] & 0x3F) | 0x80);	/* RFC 4122 variant */

		if (MpIdFileWrite(gMp.config.playerId))
		{
			if (old != NULL && old[0] != '\0')
				jer_config_set_str("mp", "player_id", "");	/* the file is the store now */
		}
		else if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] could not write %s/%s; the id is kept for this run only\n",
				JER_CONFIG_PATH, MP_ID_FILE);
	}

	MpFormatPlayerId(gMp.config.playerId, text);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] player id %s%s\n", text, how);
}

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

	/* SESSION RULES. Defaults are the game's own behaviour -- cops on, the
	 * engine's default difficulty (1), nothing invulnerable -- so a host who
	 * never opens the settings gets what the game would have done anyway. */
	gMp.config.cops = jer_config_get_int("mp", "cops", 1) ? 1 : 0;

	gMp.config.copDifficulty = jer_config_get_int("mp", "cop_difficulty", 1);
	if (gMp.config.copDifficulty < 0 || gMp.config.copDifficulty > MP_COP_DIFF_MAX)
		gMp.config.copDifficulty = 1;

	gMp.config.invuln = jer_config_get_int("mp", "invuln", 0) ? 1 : 0;
	gMp.config.immune = jer_config_get_int("mp", "immune", 0) ? 1 : 0;

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

	MpLoadPlayerId();

	s = jer_config_get_str("mp", "host_name", "");
	if (s != NULL && s[0] != '\0')
		snprintf(gMp.config.hostName, sizeof(gMp.config.hostName), "%s", s);
	else
		snprintf(gMp.config.hostName, sizeof(gMp.config.hostName), "%s's game", gMp.config.playerName);

	/* The discovery server a stock JERICHO talks to. It is what makes the Online
	 * browser show anything, and what a hosted game lists itself on. Set it EMPTY
	 * to run LAN-only: no listing, no advertisement, the beacon still works. */
	s = jer_config_get_str("mp", "master_url", MP_DEFAULT_MASTER_URL);
	snprintf(gMp.config.masterUrl, sizeof(gMp.config.masterUrl), "%s", (s != NULL) ? s : "");
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
	jer_config_set_int("mp", "cops", gMp.config.cops);
	jer_config_set_int("mp", "cop_difficulty", gMp.config.copDifficulty);
	jer_config_set_int("mp", "invuln", gMp.config.invuln);
	jer_config_set_int("mp", "immune", gMp.config.immune);

	jer_config_set_int("mp", "custom_color", gMp.config.colorOn);
	jer_config_set_int("mp", "color_r", gMp.config.colorR);
	jer_config_set_int("mp", "color_g", gMp.config.colorG);
	jer_config_set_int("mp", "color_b", gMp.config.colorB);
	jer_config_set_str("mp", "player_name", gMp.config.playerName);
	jer_config_set_str("mp", "host_name", gMp.config.hostName);
	jer_config_set_str("mp", "master_url", gMp.config.masterUrl);
}

/* A short digest of the enabled-module manifest (id + version), used in
 * beacons and the handshake so mismatched lobbies are visible up front. */
unsigned short MpModHash(void)
{
	JER_MODULE_INFO info[MP_MAX_MODS];
	int n = jer_module_list(info, MP_MAX_MODS);
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
	JER_MODULE_INFO info[MP_MAX_MODS];
	int n = jer_module_list(info, MP_MAX_MODS);
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
