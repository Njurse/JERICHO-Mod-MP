/*
 * mp_bridge.c -- the game-side implementation of the JERICHO addon network
 * bridge (jer_net.h).
 *
 * Channels are multiplexed over the mp session as MP_CHANNEL messages. The
 * host relays: a client's payload is broadcast to the other clients, the
 * host's is broadcast to everyone. Inbound payloads are surfaced to modules
 * through JER_EVENT_NET_RECV. With no session every call is a safe no-op.
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_net.h"
#include "mp.h"

#include <string.h>
#include <stdio.h>

#define MP_MAX_CHANNELS		16
#define MP_CHANNEL_MAX_PAYLOAD	1024

typedef struct MP_CHANNEL_REG
{
	int  used;
	char name[MP_CHANNEL_NAME_MAX];
	int  reliability;
} MP_CHANNEL_REG;

static MP_CHANNEL_REG gChannels[MP_MAX_CHANNELS];

#define MP_CHANNEL_SLOTS ((int)(sizeof(gChannels) / sizeof(gChannels[0])))

int jer_net_register_channel(const char* name, int reliability)
{
	int i, freeIdx = -1;

	if (name == NULL || name[0] == '\0' || strlen(name) >= MP_CHANNEL_NAME_MAX)
		return -1;

	for (i = 0; i < MP_CHANNEL_SLOTS; i++)
	{
		if (gChannels[i].used && strcmp(gChannels[i].name, name) == 0)
			return i;

		if (!gChannels[i].used && freeIdx < 0)
			freeIdx = i;
	}

	if (freeIdx < 0)
		return -1;

	gChannels[freeIdx].used = 1;
	snprintf(gChannels[freeIdx].name, sizeof(gChannels[freeIdx].name), "%s", name);
	gChannels[freeIdx].reliability = (reliability == JER_NET_RELIABLE) ? 1 : 0;

	return freeIdx;
}

static int MpChannelReliability(const char* name)
{
	int i;

	for (i = 0; i < MP_CHANNEL_SLOTS; i++)
	{
		if (gChannels[i].used && strcmp(gChannels[i].name, name) == 0)
			return gChannels[i].reliability;
	}

	return 0;
}

int jer_net_send(const char* name, const void* data, int len)
{
	unsigned char buf[sizeof(MP_CHANNEL) + MP_CHANNEL_MAX_PAYLOAD];
	MP_CHANNEL h;
	int reliable, total;

	if (!MpIsActive() || name == NULL || name[0] == '\0')
		return 0;

	if (len < 0)
		len = 0;
	if (len > MP_CHANNEL_MAX_PAYLOAD)
		len = MP_CHANNEL_MAX_PAYLOAD;

	reliable = MpChannelReliability(name);

	memset(&h, 0, sizeof(h));
	snprintf(h.name, sizeof(h.name), "%s", name);
	h.len = (uint16_t)len;
	h.frame = reliable ? gMp.frame : 0;
	h.reliable = (uint8_t)reliable;
	h.fromPlayer = (uint8_t)(gMp.localPlayerId < 0 ? 0 : gMp.localPlayerId);

	memcpy(buf, &h, sizeof(h));
	if (len > 0 && data != NULL)
		memcpy(buf + sizeof(h), data, (size_t)len);

	total = (int)(sizeof(h) + (size_t)len);

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_CHANNEL, reliable ? MP_FLAG_RELIABLE : 0, buf, total);
	else
		MpSendToHost(MP_TAG_CHANNEL, reliable ? MP_FLAG_RELIABLE : 0, buf, total);

	return 1;
}

int jer_net_is_active(void) { return MpIsActive(); }
int jer_net_is_host(void)   { return MpIsHost(); }
int jer_net_peer_count(void) { return MpPeerCount(); }
int jer_net_local_player(void) { return MpIsActive() ? gMp.localPlayerId : -1; }

/* Called by mp_session.c for each inbound MP_CHANNEL. */
void MpBridgeDeliver(const char* channel, int peer, const void* data, int len)
{
	JER_ARGS_NET_RECV a;

	a.channel = channel;
	a.peer = peer;
	a.data = data;
	a.len = len;

	if (gMpCtx != NULL)
		gMpCtx->jer_fire(gMpCtx, JER_EVENT_NET_RECV, &a);
}
