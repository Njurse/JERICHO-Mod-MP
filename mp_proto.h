/*
 * mp_proto.h -- JERICHO Multiplayer wire protocol.
 *
 * The single source of truth for the binary schema exchanged between the
 * game instances over the network. Same conventions as gaildrv2_proto.h:
 * all multi-byte fields are LITTLE-ENDIAN and every struct is
 * #pragma pack(1) with a compile-time layout check in C++.
 *
 * Two transports:
 *   - TCP (default port 1318) carries the session: handshake, lobby,
 *     per-frame input, state-resync snapshots and the addon channel.
 *   - UDP MP_DISCOVERY_PORT (broadcast) carries discovery beacons only, and
 *     names the host's session port in the payload.
 *
 * TCP framing (identical shape to gaildrv2):
 *
 *     u32 len          -- number of bytes that follow this field (>= 8)
 *     char tag[4]      -- message type (MP_TAG_*)
 *     u8  version      -- MP_PROTO_VERSION
 *     u8  flags        -- per-message flags (MP_FLAG_*)
 *     u16 spare        -- reserved, zero
 *     ... payload      -- depends on tag
 *
 * A message is read by first recv()ing the envelope (12 bytes), then `len`
 * more bytes (the payload, which includes nothing of the envelope). The
 * stream has no framing otherwise: both sides must tolerate partial reads
 * and coalesced writes.
 *
 * Bump MP_PROTO_VERSION on any incompatible change; a peer with a
 * different version is rejected with MP_REJECT_VERSION.
 */
#ifndef MP_PROTO_H
#define MP_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_PROTO_VERSION	1

/* Default UDP+TCP port. 1318 is IANA-unassigned (the neighbour 1319 is
 * amx-icsp), so it is a safe, non-reserved choice for a game. Configurable
 * per instance via JERICHO/CONFIG/mp.ini (`port`). */
#define MP_DEFAULT_PORT		1318
/* UDP beacon port. FIXED and shared by every instance: it is the address a
 * browser listens on and a host beacons to, so neither side can be moved by
 * changing the session port. The session port travels in the beacon payload
 * (MP_BEACON.port), which is why a host that overrides its port stays
 * discoverable. */
#define MP_DISCOVERY_PORT	1318

/* Capacity. The engine renders at most 2 local player views, but a LAN
 * session can carry more remote players as world cars. */
#define MP_MAX_PLAYERS		8

/* Fixed field widths for the handshake manifest. */
#define MP_MOD_ID_MAX		24
#define MP_MOD_VER_MAX		16
#define MP_MAX_MODS		32
#define MP_NAME_MAX		32
#define MP_REJECT_TEXT_MAX	64
#define MP_NOTIFY_MAX		6	/* lines kept in the lower-left info log */
#define MP_NOTIFY_TEXT_MAX	96
#define MP_NOTIFY_MS		6000	/* how long a line stays on screen (ms) */
#define MP_CHANNEL_NAME_MAX	16

/* ------------------------------------------------------------------ */
/* Tags                                                                */
/* ------------------------------------------------------------------ */
#define MP_TAG_HELLO	"JPHL"	/* client -> host: identity + mod manifest */
#define MP_TAG_WELCOME	"JPWL"	/* host -> client: accepted (id + lobby) */
#define MP_TAG_REJECT	"JPRJ"	/* host -> client: refused (reason text) */
#define MP_TAG_SESSION	"JPSS"	/* host -> client: session config broadcast */
#define MP_TAG_START	"JPST"	/* host -> client: begin the level launch */
#define MP_TAG_INPUT	"JPIN"	/* client -> host, host -> client: input set */
#define MP_TAG_CARSTATE	"JPCS"	/* host -> client: resync snapshot */
#define MP_TAG_PING	"JPPN"
#define MP_TAG_PONG	"JPPO"
#define MP_TAG_CHANNEL	"JPCH"	/* addon net bridge payload */
#define MP_TAG_LEAVE	"JPLV"	/* either side: leaving the session */
#define MP_TAG_SPAWN	"JPSW"	/* host -> all: where everyone lines up */
#define MP_TAG_ROSTER	"JPRS"	/* host -> all: who is in the match */
#define MP_TAG_HIT	"JPHI"	/* either side: "my car bumped yours, you push yourself" */

/* How far apart the player cars stand at the meeting point: close enough that
 * everybody is on one screen, far enough not to spawn inside each other. */
#define MP_SPAWN_SLOT_DIST	260

typedef struct MP_SPAWN
{
	int32_t x, y, z;
	int32_t heading;
} MP_SPAWN;

/* The host's view of the match: one row per player, in ASCENDING PLAYER ID.
 *
 * That order is the point. Car slots are handed out by the engine in the order
 * the player cars are created, so if each machine walked its own registry (rows
 * are given out first-free) the two sides could put the same two players in
 * opposite slots and each would then drive the other's car. Walking player ids
 * instead gives both machines the same answer.
 *
 * It is also sent BEFORE the launch, so every machine knows how many cars to
 * spawn: a client that did not know about the host yet added no car at all and
 * left the level's own AI car sitting in the remote player's slot. */
#define MP_ROSTER_NAME_MAX	20
#define MP_ROSTER_FLAG_HOST	1

typedef struct MP_ROSTER_ENTRY
{
	uint8_t  id;
	uint8_t  carId;		/* that machine's local CAR_DATA slot, informational */
	uint8_t  model;		/* car model, 0xFF = on foot */
	uint8_t  flags;		/* MP_ROSTER_FLAG_* */
	int32_t  x, y, z;	/* where that player's car is now */
	uint16_t ping;		/* round trip in ms, as the host measured it */
	uint16_t reserved;
	char     name[MP_ROSTER_NAME_MAX];
} MP_ROSTER_ENTRY;

typedef struct MP_ROSTER
{
	uint8_t        count;
	uint8_t        reserved[3];
	MP_ROSTER_ENTRY entries[MP_MAX_PLAYERS];
} MP_ROSTER;
#define MP_TAG_CHAT	"JPCX"	/* either side: a chat line (scaffolding) */

/* Envelope flags */
#define MP_FLAG_RELIABLE	0x01	/* sender wants an ordered/reliable channel */

/* ------------------------------------------------------------------ */
/* Role / lifecycle                                                    */
/* ------------------------------------------------------------------ */
enum
{
	MP_ROLE_NONE = 0,
	MP_ROLE_HOST,
	MP_ROLE_CLIENT
};

/* Reject reasons (MP_REJECT.reason) */
enum
{
	MP_REJECT_NONE = 0,
	MP_REJECT_FULL,
	MP_REJECT_VERSION,	/* protocol / SDK / game build mismatch */
	MP_REJECT_MODS,		/* mod manifest mismatch while enforcement is on */
	MP_REJECT_INPROGRESS,	/* the host has already launched the level */
	MP_REJECT_CUSTOM
};

/* Lobby mod-match strictness (host setting) */
enum
{
	MP_MODCHECK_OFF = 0,	/* admit any client */
	MP_MODCHECK_VERSION,	/* admit if enabled-mod ids match (versions ignored) */
	MP_MODCHECK_EXACT	/* admit only if enabled-mod ids AND versions match */
};

/* Gamemode ids for the lobby (module-side; the engine GAMETYPE is mapped
 * separately). Take a Ride is the only one implemented for now. */
enum
{
	MP_GAMEMODE_TAKEADRIDE = 0,
	MP_GAMEMODE_COUNT
};

/* ------------------------------------------------------------------ */
/* Envelope (12 bytes before the payload)                              */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)
typedef struct MP_ENVELOPE
{
	uint32_t len;		/* bytes following this field (>= 8) */
	char     tag[4];
	uint8_t  version;
	uint8_t  flags;
	uint16_t spare;
} MP_ENVELOPE;

#define MP_ENVELOPE_SIZE 12

/* ------------------------------------------------------------------ */
/* Discovery beacon (UDP broadcast, host -> LAN)                       */
/* ------------------------------------------------------------------ */
#define MP_BEACON_INTERVAL_MS	1000
#define MP_BEACON_TIMEOUT_MS	4000	/* drop a server after 4s of silence */

typedef struct MP_BEACON
{
	uint32_t magic;			/* MP_UDP_MAGIC */
	uint16_t protoVersion;
	uint16_t port;			/* TCP port the host listens on */
	char     hostName[MP_NAME_MAX];
	uint8_t  players;
	uint8_t  maxPlayers;
	uint8_t  gamemode;
	uint8_t  modsEnforced;		/* MP_MODCHECK_* */
	uint8_t  city;
	uint8_t  inProgress;		/* 1 once the level has launched */
	uint16_t modHash;		/* short digest of the host manifest */
	uint16_t spare;
} MP_BEACON;

/* UDP datagram magic -- 'JMP1' little-endian. */
#define MP_UDP_MAGIC 0x31504D4A

/* Liveness: both sides ping on this cadence, so an idle session (frontend
 * lobby, quiet stretch, paused game) never looks like a dead peer. */
#define MP_KEEPALIVE_INTERVAL_MS	1000
#define MP_KEEPALIVE_MIN_MS		250	/* clamp for the config value */

/* ------------------------------------------------------------------ */
/* Handshake                                                           */
/* ------------------------------------------------------------------ */

/* One installed-mod row in the manifest. */
typedef struct MP_MOD_INFO
{
	char     id[MP_MOD_ID_MAX];
	char     version[MP_MOD_VER_MAX];
	uint8_t  enabled;
	uint8_t  reserved;
} MP_MOD_INFO;

/* 'JPHL' -- client -> host. Followed by modCount MP_MOD_INFO rows. */
typedef struct MP_HELLO
{
	uint16_t protoVersion;
	uint16_t sdkVersion;
	uint16_t gameBuild;	/* hash of JERICHO_BUILD_VERSION */
	uint16_t car;		/* this machine's vehicle (car id), 0xFFFF = unset */
	char     playerName[MP_NAME_MAX];
	uint8_t  modCount;
	uint8_t  reserved[3];
} MP_HELLO;

/* 'JPWL' -- host -> client, accepted. */
typedef struct MP_WELCOME
{
	uint8_t  playerId;	/* assigned slot, 1..MP_MAX_PLAYERS-1 (0 = host) */
	uint8_t  maxPlayers;
	uint8_t  modsEnforced;	/* MP_MODCHECK_* */
	uint8_t  modsMatched;	/* 1 if the client's manifest matched the host */
	uint8_t  running;	/* 1 = the host's match is already live (join in progress) */
	uint8_t  subGame;	/* the multiplayer sub-level (gSubGameNumber) */
	uint8_t  mpLevel;	/* 1 = the host is on a multiplayer map, 0 = the full city */
	uint8_t  arena;		/* which multiplayer map, 0/1 (gBootMpArena) - keeps the struct 16 bytes */
	uint8_t  gamemode;
	uint8_t  city;
	uint8_t  timeOfDay;
	uint8_t  weather;
	uint32_t seed;
	uint8_t  hostCar;	/* the host's own vehicle (car id), 0xFF = unset */
} MP_WELCOME;

/* 'JPRJ' -- host -> client, refused. */
typedef struct MP_REJECT
{
	uint8_t  reason;	/* MP_REJECT_* */
	uint8_t  reserved[3];
	char     text[MP_REJECT_TEXT_MAX];
} MP_REJECT;

/* 'JPSS' -- host -> client: the lobby's pending session config, also used
 * to keep clients' lobby readouts in sync before the start. */
typedef struct MP_SESSION
{
	uint8_t  gamemode;
	uint8_t  city;
	uint8_t  timeOfDay;
	uint8_t  weather;
	uint32_t seed;
	uint8_t  numPlayers;
	uint8_t  state;		/* MP_SESSION_STATE_* */
	uint16_t spare;
} MP_SESSION;

enum
{
	MP_SESSION_LOBBY = 0,
	MP_SESSION_STARTING
};

/* 'JPST' -- host -> client: launch the level with the agreed config. */
typedef struct MP_START
{
	MP_SESSION session;
} MP_START;

/* ------------------------------------------------------------------ */
/* Per-frame input (input lockstep)                                    */
/* ------------------------------------------------------------------ */
typedef struct MP_PLAYER_INPUT
{
	uint8_t  playerId;
	uint16_t pad;		/* engine-native mapped pad bits */
	uint8_t  spare;
} MP_PLAYER_INPUT;

/* 'JPIN' -- one frame's input set. Header followed by count rows. */
typedef struct MP_INPUT
{
	uint32_t frame;
	uint8_t  count;
	uint8_t  reserved[3];
} MP_INPUT;

/* ------------------------------------------------------------------ */
/* State resync (fallback when lockstep diverges)                      */
/* ------------------------------------------------------------------ */
/* 'JPCS' -- host -> client periodic snapshot. Header + count rows. */
typedef struct MP_CARSTATE
{
	uint32_t frame;
	uint8_t  count;
	uint8_t  reserved[3];
} MP_CARSTATE;

/* A snapshot row carries the car's WHOLE rigid body, not just a position and a
 * heading: hd.direction is an OUTPUT the engine re-derives from the orientation,
 * so a snap that writes only that leaves the receiver's car at the wrong ATTITUDE
 * -- which is how a remote car ends up driving around upside down. Orientation
 * and velocities come straight from st.n (the handling state). */
#define MP_CARSTATE_HAS_BODY	1

typedef struct MP_CARSTATE_ENTRY
{
	uint8_t  playerId;
	uint8_t  flags;		/* MP_CARSTATE_HAS_BODY */
	uint8_t  palette;	/* owner's car colour (cp->ap.palette) -- owner-authoritative */
	int16_t  orient[4];	/* st.n.orientation */
	int32_t  x, y, z;	/* world units */
	int32_t  heading;	/* hd.direction */
	int16_t  angVel[3];	/* st.n.angularVelocity */
	int32_t  vel[3];	/* st.n.linearVelocity */
} MP_CARSTATE_ENTRY;

/* ------------------------------------------------------------------ */
/* Addon network bridge ('JPCH')                                       */
/* ------------------------------------------------------------------ */

/* A registered channel id is a short name; the payload follows the header. */
typedef struct MP_CHANNEL
{
	char     name[MP_CHANNEL_NAME_MAX];
	uint16_t len;
	uint32_t frame;		/* 0 = out-of-band (latest-wins) */
	uint8_t  reliable;
	uint8_t  fromPlayer;
	uint8_t  reserved[2];
} MP_CHANNEL;

/* Ping/pong carry a tick the peer echoes. */
typedef struct MP_PING
{
	uint32_t tick;
} MP_PING;

#define MP_PONG MP_PING

/* A chat line (scaffolding for the eventual chat feature). */
#define MP_CHAT_TEXT_MAX	96

typedef struct MP_CHAT
{
	uint8_t playerId;
	uint8_t reserved[3];
	char    text[MP_CHAT_TEXT_MAX];
} MP_CHAT;

/* A contact report. Cars are owner-authoritative, so a machine can only move
 * the ONE car it owns: when MY car touches YOURS I push MINE, and I send you
 * this so you push YOURS. Both cars move, and each stays the owner's truth. */
typedef struct MP_HIT
{
	uint8_t  targetId;	/* the player whose car should receive the impulse */
	uint8_t  reserved[3];
	int32_t  impulse[3];	/* velocity delta to ADD to the target's car */
} MP_HIT;

#pragma pack(pop)

/* Compile-time layout checks (modules compile as C++). */
#ifdef __cplusplus
static_assert(sizeof(MP_ENVELOPE) == 12, "MP_ENVELOPE layout");
static_assert(sizeof(MP_BEACON) == 50, "MP_BEACON layout");
static_assert(sizeof(MP_MOD_INFO) == 42, "MP_MOD_INFO layout");
static_assert(sizeof(MP_HELLO) == 44, "MP_HELLO layout");
static_assert(sizeof(MP_WELCOME) == 17, "MP_WELCOME layout");
static_assert(sizeof(MP_REJECT) == 68, "MP_REJECT layout");
static_assert(sizeof(MP_SESSION) == 12, "MP_SESSION layout");
static_assert(sizeof(MP_PLAYER_INPUT) == 4, "MP_PLAYER_INPUT layout");
static_assert(sizeof(MP_INPUT) == 8, "MP_INPUT layout");
static_assert(sizeof(MP_CARSTATE_ENTRY) == 45, "MP_CARSTATE_ENTRY layout");
static_assert(sizeof(MP_HIT) == 16, "MP_HIT layout");
static_assert(sizeof(MP_CARSTATE) == 8, "MP_CARSTATE layout");
static_assert(sizeof(MP_CHANNEL) == 26, "MP_CHANNEL layout");

#endif

#ifdef __cplusplus
}
#endif

#endif /* MP_PROTO_H */
