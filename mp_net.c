/*
 * mp_net.c -- JERICHO Multiplayer transport.
 *
 * Non-blocking-ish TCP session + UDP LAN discovery beacon. The socket
 * scaffolding mirrors the proven gaildrv2.c approach:
 *
 *   - Windows: winsock2 (included BEFORE windows.h; the PSX libapi.h
 *     OpenEvent clash is defused), ws2_32 linked via #pragma.
 *   - Linux: plain BSD sockets.
 *   - sockets stay BLOCKING for recv (we only recv when FIONREAD reports
 *     bytes), with SO_SNDTIMEO so a wedged peer cannot freeze the game;
 *     the listen socket is non-blocking so accept() never hangs.
 *
 * Framing and the message catalog live in mp_proto.h. Complete messages
 * are handed to MpHandleMessage() (mp_session.c) which owns the semantics.
 */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   /* The Windows SDK renames OpenEvent -> OpenEventW under UNICODE, which
    * would clash with the PSX shim; kill the macro. */
#  undef OpenEvent
#else
#  include <sys/socket.h>
#  include <sys/ioctl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <time.h>
#  include <errno.h>
   typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define closesocket close
#  define ioctlsocket(s, cmd, argp) ioctl((s), (cmd), (argp))
#endif

#include "jericho.h"
#include "mp.h"

#include <string.h>
#include <stdio.h>

#define MP_RECV_BUF		8192
#define MP_SEND_BUF		2048
#define MP_ACCEPT_BACKLOG	8
#define MP_SND_TIMEOUT_MS	250
#define MP_CONN_TIMEOUT_MS	10000	/* drop a peer after 10 s of silence (~10
					 * missed keepalive pings) */
#define MP_CONNECT_TIMEOUT_MS	5000

/* ------------------------------------------------------------------ */
/* Connection table                                                    */
/* ------------------------------------------------------------------ */
typedef struct MP_CONN
{
	int           used;
	SOCKET        sock;
	int           hostSide;	/* 1 = a peer connected to us (host role) */
	int           playerId;	/* assigned during the handshake, -1 until then */
	unsigned long lastRecvMs;
	unsigned char rbuf[MP_RECV_BUF];
	int           rbufLen;
} MP_CONN;

static MP_CONN gConn[MP_MAX_PLAYERS];
static SOCKET  gListen = INVALID_SOCKET;	/* host listener */
static int     gNetStarted;
static unsigned long gLastPollMs;	/* when we last serviced the sockets */

/* ------------------------------------------------------------------ */
/* Platform helpers                                                    */
/* ------------------------------------------------------------------ */
static unsigned long MpClockMs(void)
{
#ifdef _WIN32
	return GetTickCount();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
#endif
}

unsigned long MpNowMs(void)
{
	return MpClockMs();
}

static int MpPendingBytes(SOCKET s)
{
#ifdef _WIN32
	unsigned long n = 0;
	ioctlsocket(s, FIONREAD, &n);
	return (int)n;
#else
	int n = 0;
	ioctl(s, FIONREAD, &n);
	return n;
#endif
}

static void MpSetNonBlocking(SOCKET s, int on)
{
#ifdef _WIN32
	unsigned long mode = on ? 1 : 0;
	ioctlsocket(s, FIONBIO, &mode);
#else
	int fl = fcntl(s, F_GETFL, 0);
	if (on) fcntl(s, F_SETFL, fl | O_NONBLOCK);
	else    fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

static void MpTuneConn(SOCKET s)
{
	int one = 1;
	int sndTimeout = MP_SND_TIMEOUT_MS;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));
}

static int MpSendRaw(SOCKET s, const void* data, int len)
{
	const char* p = (const char*)data;
	int sent = 0;

	while (sent < len)
	{
		int n = send(s, p + sent, len - sent, 0);

		if (n <= 0)
			return 0;

		sent += n;
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
int MpNetStart(void)
{
#ifdef _WIN32
	WSADATA wsa;

	if (gNetStarted)
		return 1;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return 0;
#endif
	gNetStarted = 1;
	memset(gConn, 0, sizeof(gConn));

	return 1;
}

static void MpCloseSock(SOCKET* s)
{
	if (*s != INVALID_SOCKET)
	{
		closesocket(*s);
		*s = INVALID_SOCKET;
	}
}

void MpNetShutdown(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gConn[i].used)
		{
			MpCloseSock(&gConn[i].sock);
			gConn[i].used = 0;
			gConn[i].rbufLen = 0;
		}
	}

	MpDiscoveryStop();
	MpCloseSock(&gListen);
#ifdef _WIN32
	if (gNetStarted)
		WSACleanup();
#endif
	gNetStarted = 0;
}

/* ------------------------------------------------------------------ */
/* Host: listen / accept                                               */
/* ------------------------------------------------------------------ */
/* Tell every connected player we are ending the match, before the sockets
 * close, so they can go back to their frontend with a proper notice. */
void MpHostByeAll(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gConn[i].used && gConn[i].hostSide)
			MpSendConn(i, MP_TAG_LEAVE, MP_FLAG_RELIABLE, NULL, 0);
	}
}

int MpHostBegin(void)
{
	struct sockaddr_in addr;
	int one = 1;

	if (!gNetStarted && !MpNetStart())
		return 0;

	if (gListen != INVALID_SOCKET)
		return 1;	/* already listening */

	gListen = socket(AF_INET, SOCK_STREAM, 0);
	if (gListen == INVALID_SOCKET)
		return 0;

	setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)gMp.config.port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(gListen, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		MpCloseSock(&gListen);
		return 0;
	}

	if (listen(gListen, MP_ACCEPT_BACKLOG) == SOCKET_ERROR)
	{
		MpCloseSock(&gListen);
		return 0;
	}

	MpSetNonBlocking(gListen, 1);

	gMp.listenersUp = 1;
	if (gMpCtx) gMpCtx->jer_log(gMpCtx, "[mp] listening on TCP/%d\n", gMp.config.port);

	return 1;
}

void MpHostEnd(void)
{
	MpCloseSock(&gListen);
	gMp.listenersUp = 0;
}

static int MpAllocConn(void)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (!gConn[i].used)
		{
			memset(&gConn[i], 0, sizeof(gConn[i]));
			gConn[i].used = 1;
			gConn[i].playerId = -1;
			return i;
		}
	}

	return -1;
}

static void MpAcceptPeers(void)
{
	for (;;)
	{
		SOCKET s = accept(gListen, NULL, NULL);
		int idx;

		if (s == INVALID_SOCKET)
			break;

		idx = MpAllocConn();
		if (idx < 0)
		{
			closesocket(s);	/* full */
			continue;
		}

		MpTuneConn(s);
		gConn[idx].sock = s;
		gConn[idx].hostSide = 1;
		gConn[idx].lastRecvMs = MpClockMs();

		if (gMpCtx) gMpCtx->jer_log(gMpCtx, "[mp] peer connected (awaiting handshake)\n");
	}
}

/* ------------------------------------------------------------------ */
/* Client: join state + the asynchronous connect plumbing              */
/*                                                                     */
/* The frontend must never freeze on a join, so the UI path starts the  */
/* connect and lets MpClientConnectPoll() finish it on a later frame,   */
/* while the menu shows "Connecting to <host>:<port>...".               */
/* ------------------------------------------------------------------ */
static SOCKET gConnectingSock = INVALID_SOCKET;
static char   gConnectingHost[64];
static int    gConnectingPort;
static unsigned long gConnectingSinceMs;
static int    gJoinState = MP_JOIN_IDLE;

int MpJoinState(void)
{
	return gJoinState;
}

void MpJoinStateSet(int state)
{
	gJoinState = state;
}

const char* MpJoinTarget(void)
{
	return gConnectingHost;
}

int MpJoinTargetPort(void)
{
	return gConnectingPort;
}

static void MpClientConnectCancel(void)
{
	MpCloseSock(&gConnectingSock);
}

/* ------------------------------------------------------------------ */
/* Client: connect (blocking variant, used by -join / MP_AUTOSTART)     */
/* ------------------------------------------------------------------ */
/* NOTE: the join is always asynchronous now (MpClientConnectBegin +
 * MpClientConnectPoll) so the frontend can show "Connecting to ..." instead
 * of freezing. The blocking variant was removed with the last caller. */

void MpClientDisconnect(void)
{
	int i;

	MpClientConnectCancel();	/* abandon an in-flight connect too */

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gConn[i].used && !gConn[i].hostSide)
		{
			MpCloseSock(&gConn[i].sock);
			gConn[i].used = 0;
			gConn[i].rbufLen = 0;
		}
	}

	gMp.connected = 0;
	gJoinState = MP_JOIN_IDLE;
}

/* A socket set up for `host:port` (already non-blocking, ready for connect). */
static SOCKET MpClientSocket(const char* host, int port, struct sockaddr_in* out)
{
	struct sockaddr_in addr;
	SOCKET s;
	int one = 1;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
		return INVALID_SOCKET;

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)(port > 0 ? port : gMp.config.port));
	addr.sin_addr.s_addr = inet_addr(host != NULL ? host : "");
	if (addr.sin_addr.s_addr == INADDR_NONE)
	{
		/* not a dotted-quad: try the loopback, then give up */
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}

	*out = addr;

	MpSetNonBlocking(s, 1);

	return s;
}

/* The connect succeeded: adopt the socket as the host connection and start
 * the handshake. Still CONNECTING until the host answers with a WELCOME. */
static int MpClientAdopt(SOCKET s, const char* host, int port)
{
	int idx;

	MpSetNonBlocking(s, 0);	/* back to blocking recv (guarded by FIONREAD) */
	MpTuneConn(s);

	idx = MpAllocConn();
	if (idx < 0)
	{
		closesocket(s);
		return 0;
	}

	gConn[idx].sock = s;
	gConn[idx].hostSide = 0;
	gConn[idx].playerId = 0;	/* the host */
	gConn[idx].lastRecvMs = MpClockMs();

	gMp.connected = 1;
	gJoinState = MP_JOIN_CONNECTING;

	if (gMpCtx) gMpCtx->jer_log(gMpCtx, "[mp] connected to %s:%d\n", host, port);

	MpSendHello();

	return 1;
}

/* The attempt is over and it failed: drop the socket, say so plainly (the
 * UI shows the toast and stops showing "Connecting..."). */
static void MpJoinFail(const char* why)
{
	MpClientConnectCancel();

	gJoinState = MP_JOIN_FAILED;

	if (gMp.role == MP_ROLE_CLIENT)
		gMp.role = MP_ROLE_NONE;

	jer_error("Could not join the server at %s:%d", gConnectingHost, gConnectingPort);

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] join FAILED: could not reach %s:%d (%s)\n",
			gConnectingHost, gConnectingPort, why);
}

int MpClientConnectBegin(const char* host, int port)
{
	struct sockaddr_in addr;
	SOCKET s;
	int rc;

	if (!gNetStarted && !MpNetStart())
		return 0;

	MpClientDisconnect();	/* also cancels any earlier attempt */

	s = MpClientSocket(host, port, &addr);
	if (s == INVALID_SOCKET)
		return 0;

	snprintf(gConnectingHost, sizeof(gConnectingHost), "%s", host != NULL ? host : "");
	gConnectingPort = port > 0 ? port : gMp.config.port;
	gConnectingSinceMs = MpClockMs();
	gJoinState = MP_JOIN_CONNECTING;

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] connecting to %s:%d\n", gConnectingHost, gConnectingPort);

	rc = connect(s, (struct sockaddr*)&addr, sizeof(addr));

	if (rc == 0)
	{
		/* connected on the spot (loopback usually does) */
		MpClientAdopt(s, gConnectingHost, gConnectingPort);
		return 1;
	}

	/* refused/timeout is NOT an error here: a non-blocking connect reports it
	 * through select() in a moment, which is what keeps the frontend alive */
	gConnectingSock = s;

	return 1;
}

void MpClientConnectPoll(void)
{
	fd_set wfds, efds;
	struct timeval tv;
	int err = 0;
#ifdef _WIN32
	int elen = (int)sizeof(err);
#else
	socklen_t elen = sizeof(err);
#endif

	if (gConnectingSock == INVALID_SOCKET)
		return;

	FD_ZERO(&wfds);
	FD_SET(gConnectingSock, &wfds);
	FD_ZERO(&efds);
	FD_SET(gConnectingSock, &efds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (select((int)gConnectingSock + 1, NULL, &wfds, &efds, &tv) <= 0)
	{
		/* not resolved yet -- only give up after the bounded wait */
		if ((MpClockMs() - gConnectingSinceMs) > (unsigned long)MP_CONNECT_TIMEOUT_MS)
			MpJoinFail("timed out");

		return;
	}

	if (FD_ISSET(gConnectingSock, &wfds) || FD_ISSET(gConnectingSock, &efds))
	{
		if (getsockopt(gConnectingSock, SOL_SOCKET, SO_ERROR, (char*)&err, &elen) == 0 && err == 0)
		{
			SOCKET done = gConnectingSock;
			gConnectingSock = INVALID_SOCKET;
			MpClientAdopt(done, gConnectingHost, gConnectingPort);
		}
		else
		{
			MpJoinFail("connection refused");
		}
	}
}

/* ------------------------------------------------------------------ */
/* Framed send                                                         */
/* ------------------------------------------------------------------ */
int MpSendConn(int idx, const char* tag, int flags, const void* payload, int len)
{
	unsigned char buf[MP_ENVELOPE_SIZE + MP_SEND_BUF];
	MP_ENVELOPE env;

	if (idx < 0 || idx >= MP_MAX_PLAYERS || !gConn[idx].used)
		return 0;

	if (len < 0) len = 0;
	if (len > MP_SEND_BUF)
		return 0;

	env.len = (uint32_t)(8 + len);
	memcpy(env.tag, tag, 4);
	env.version = (uint8_t)MP_PROTO_VERSION;
	env.flags = (uint8_t)flags;
	env.spare = 0;

	memcpy(buf, &env, MP_ENVELOPE_SIZE);
	if (len > 0)
		memcpy(buf + MP_ENVELOPE_SIZE, payload, len);

	return MpSendRaw(gConn[idx].sock, buf, MP_ENVELOPE_SIZE + len);
}

int MpHostBroadcast(const char* tag, int flags, const void* payload, int len)
{
	int i, sent = 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gConn[i].used && gConn[i].hostSide)
			sent += MpSendConn(i, tag, flags, payload, len);
	}

	return sent;
}

int MpSendToHost(const char* tag, int flags, const void* payload, int len)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gConn[i].used && !gConn[i].hostSide)
			return MpSendConn(i, tag, flags, payload, len);
	}

	return 0;
}

int MpConnFindByPlayer(int playerId)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gConn[i].used && gConn[i].hostSide && gConn[i].playerId == playerId)
			return i;
	}

	return -1;
}

int MpSendToPlayer(int playerId, const char* tag, int flags, const void* payload, int len)
{
	return MpSendConn(MpConnFindByPlayer(playerId), tag, flags, payload, len);
}

void MpConnAssignPlayer(int connIndex, int playerId)
{
	if (connIndex >= 0 && connIndex < MP_MAX_PLAYERS)
		gConn[connIndex].playerId = playerId;
}

int MpConnPlayerId(int connIndex)
{
	if (connIndex >= 0 && connIndex < MP_MAX_PLAYERS && gConn[connIndex].used)
		return gConn[connIndex].playerId;

	return -1;
}

int MpPeerCount(void)
{
	int i, n = 0;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gConn[i].used && gConn[i].hostSide)
			n++;
	}

	return n;
}

/* ------------------------------------------------------------------ */
/* Framed recv + dispatch                                              */
/* ------------------------------------------------------------------ */
static void MpDropConn(int idx, const char* why)
{
	/* on a client, losing the server connection is worth telling the player
	 * about -- and it means the match is over, so go back to the frontend.
	 * `gMp.leaving` marks a deliberate leave, which must stay quiet. */
	if (gMp.role == MP_ROLE_CLIENT && !gMp.leaving)
	{
		jer_error("Connection to the server lost!");
		MpReturnToFrontend();
	}

	if (gConn[idx].used && gConn[idx].hostSide && gConn[idx].playerId >= 0)
		MpRemovePlayer(gConn[idx].playerId);

	MpCloseSock(&gConn[idx].sock);
	gConn[idx].used = 0;
	gConn[idx].rbufLen = 0;

	if (gMpCtx) gMpCtx->jer_log(gMpCtx, "[mp] peer dropped (%s)\n", why);
}

void MpConnClose(int connIndex)
{
	if (connIndex >= 0 && connIndex < MP_MAX_PLAYERS && gConn[connIndex].used)
		MpDropConn(connIndex, "closed by us");
}

static void MpProcessConn(int idx)
{
	MP_CONN* c = &gConn[idx];
	char buf[2048];

	/* Drain what is readable. select() with a zero timeout is used rather
	 * than FIONREAD because a peer that hung up becomes *readable with 0
	 * bytes* -- FIONREAD never reports that, so a clean disconnect would go
	 * unnoticed until the next send. */
	for (;;)
	{
		fd_set rd;
		struct timeval tv;
		int n;

		FD_ZERO(&rd);
		FD_SET(c->sock, &rd);
		tv.tv_sec = 0;
		tv.tv_usec = 0;

		if (select(0, &rd, NULL, NULL, &tv) <= 0 || !FD_ISSET(c->sock, &rd))
			break;

		n = recv(c->sock, buf, (int)sizeof(buf), 0);
		if (n <= 0)
		{
			MpDropConn(idx, "closed");
			return;
		}

		if (c->rbufLen + n > MP_RECV_BUF)
		{
			MpDropConn(idx, "overflow");
			return;
		}

		memcpy(c->rbuf + c->rbufLen, buf, n);
		c->rbufLen += n;
		c->lastRecvMs = MpClockMs();
	}

	/* parse whole messages */
	for (;;)
	{
		MP_ENVELOPE env;
		int total, payloadLen;

		if (c->rbufLen < MP_ENVELOPE_SIZE)
			break;

		memcpy(&env, c->rbuf, MP_ENVELOPE_SIZE);

		if (env.len < 8 || env.len > (uint32_t)MP_RECV_BUF)
		{
			MpDropConn(idx, "bad frame");
			return;
		}

		total = 4 + (int)env.len;	/* len field + env.len bytes */
		if (c->rbufLen < total)
			break;			/* wait for the rest */

		payloadLen = (int)env.len - 8;

		if (env.version != MP_PROTO_VERSION)
		{
			MpDropConn(idx, "version mismatch");
			return;
		}

		MpHandleMessage(idx, env.tag, c->rbuf + MP_ENVELOPE_SIZE, payloadLen);

		/* the handler may have dropped us (e.g. a rejected join closed the
		 * connection and zeroed rbufLen) -- bail BEFORE the shift, otherwise
		 * rbufLen - total is negative and memmove() gets a huge length. */
		if (!gConn[idx].used)
			return;

		memmove(c->rbuf, c->rbuf + total, c->rbufLen - total);
		c->rbufLen -= total;
	}
}

/* ------------------------------------------------------------------ */
/* Keepalive                                                           */
/* ------------------------------------------------------------------ */
static unsigned long gLastPingMs;

/* Both sides ping on a timer, so an idle session (frontend lobby, a quiet
 * stretch, a paused game) keeps proving it is alive -- the silence timeout
 * then means what it says: the peer really is gone. */
static void MpKeepaliveTick(void)
{
	unsigned long now = MpClockMs();
	MP_PING pg;
	int ms;

	if (gMp.role == MP_ROLE_NONE)
		return;

	if (MpIsHost())
	{
		if (MpPeerCount() <= 0)
			return;	/* nobody to keep alive */
	}
	else if (!gMp.connected)
	{
		return;		/* still connecting / already gone */
	}

	ms = gMp.config.keepaliveMs;
	if (ms < MP_KEEPALIVE_MIN_MS)
		ms = MP_KEEPALIVE_MIN_MS;

	if (gLastPingMs != 0 && (now - gLastPingMs) < (unsigned long)ms)
		return;

	gLastPingMs = now;
	pg.tick = (uint32_t)now;

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_PING, 0, &pg, sizeof(pg));
	else
		MpSendToHost(MP_TAG_PING, 0, &pg, sizeof(pg));
}

void MpNetPoll(int waitMs)
{
	unsigned long now = MpClockMs();
	int i;

	(void)waitMs;

	if (gListen != INVALID_SOCKET)
		MpAcceptPeers();

	MpDiscoveryPoll();
	MpClientConnectPoll();	/* finish an asynchronous join */
	MpKeepaliveTick();	/* prove we are alive to our peers */

	/* A poll gap longer than the timeout means WE were not running -- a level
	 * load, a long pause, a debugger. The peers' recv timers went stale because
	 * we were not listening, not because they went away, so credit that time
	 * back rather than dropping a healthy connection. */
	if (gLastPollMs != 0 && (now - gLastPollMs) > (unsigned long)MP_CONN_TIMEOUT_MS)
	{
		for (i = 0; i < MP_MAX_PLAYERS; i++)
		{
			if (gConn[i].used)
				gConn[i].lastRecvMs = now;
		}
	}

	gLastPollMs = now;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (!gConn[i].used)
			continue;

		MpProcessConn(i);

		if (gConn[i].used && (now - gConn[i].lastRecvMs) > MP_CONN_TIMEOUT_MS)
			MpDropConn(i, "timeout");
	}
}

/* ------------------------------------------------------------------ */
/* UDP LAN discovery -- beacon + server list                           */
/* ------------------------------------------------------------------ */
#define MP_MAX_SERVERS 16

static SOCKET gBeaconSock = INVALID_SOCKET;
static int    gBeaconAdvertise;
static unsigned long gLastBeaconMs;
static MP_SERVER gServers[MP_MAX_SERVERS];
static unsigned gDiscoveryRev;	/* bumped when the VISIBLE server set changes */

#define MP_SERVER_SLOTS ((int)(sizeof(gServers) / sizeof(gServers[0])))

void MpDiscoveryStart(int advertise)
{
	struct sockaddr_in addr;
	int one = 1;

	if (gBeaconSock == INVALID_SOCKET)
	{
		gBeaconSock = socket(AF_INET, SOCK_DGRAM, 0);
		if (gBeaconSock == INVALID_SOCKET)
			return;

		setsockopt(gBeaconSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
		setsockopt(gBeaconSock, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((unsigned short)gMp.config.port);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);

		if (bind(gBeaconSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			MpCloseSock(&gBeaconSock);
			return;
		}

		MpSetNonBlocking(gBeaconSock, 1);
	}

	gBeaconAdvertise = advertise;
	gLastBeaconMs = 0;

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] discovery %s on UDP/%d\n",
			advertise ? "advertising" : "browsing", gMp.config.port);
}

void MpDiscoveryStop(void)
{
	MpCloseSock(&gBeaconSock);
	gBeaconAdvertise = 0;
	gLastBeaconMs = 0;

	/* forget the servers we had, so a later browse starts from empty rather
	 * than showing rows that went silent while we were not listening */
	if (MpDiscoveryCount() > 0)
	{
		memset(gServers, 0, sizeof(gServers));
		gDiscoveryRev++;
	}
}

static void MpBeaconSend(void)
{
	MP_BEACON b;
	struct sockaddr_in addr;

	memset(&b, 0, sizeof(b));
	b.magic = MP_UDP_MAGIC;
	b.protoVersion = (uint16_t)MP_PROTO_VERSION;
	b.port = (uint16_t)gMp.config.port;
	snprintf(b.hostName, sizeof(b.hostName), "%s", gMp.config.hostName);
	b.players = (uint8_t)gMp.playerCount;
	b.maxPlayers = MP_MAX_PLAYERS;
	b.gamemode = (uint8_t)gMp.gamemode;
	b.modsEnforced = (uint8_t)gMp.config.modCheck;
	b.city = (uint8_t)gMp.city;
	b.inProgress = (uint8_t)gMp.running;
	b.modHash = MpModHash();

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)gMp.config.port);
	addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	sendto(gBeaconSock, (const char*)&b, (int)sizeof(b), 0,
		(struct sockaddr*)&addr, sizeof(addr));
}

static void MpServerTouch(const char* ip, const MP_BEACON* b)
{
	char name[MP_NAME_MAX];
	int i, freeIdx = -1;

	/* the wire name is a fixed 32-byte field with no guaranteed NUL, so it is
	 * normalised once here (and compared against the stored copy below) */
	snprintf(name, sizeof(name), "%s", b->hostName);

	for (i = 0; i < MP_SERVER_SLOTS; i++)
	{
		if (gServers[i].used)
		{
			if (strcmp(gServers[i].ip, ip) == 0 && gServers[i].port == (int)b->port)
			{
				/* only count a change the browser can actually show: the
				 * beacon repeats every second and bumping the revision for an
				 * identical beacon would refresh the menu once a second */
				if (gServers[i].players != b->players ||
				    gServers[i].maxPlayers != b->maxPlayers ||
				    gServers[i].gamemode != b->gamemode ||
				    gServers[i].city != b->city ||
				    gServers[i].modsEnforced != b->modsEnforced ||
				    gServers[i].inProgress != b->inProgress ||
				    gServers[i].modHash != b->modHash ||
				    strcmp(gServers[i].hostName, name) != 0)
					gDiscoveryRev++;

				snprintf(gServers[i].hostName, MP_NAME_MAX, "%s", name);
				gServers[i].players = b->players;
				gServers[i].maxPlayers = b->maxPlayers;
				gServers[i].gamemode = b->gamemode;
				gServers[i].city = b->city;
				gServers[i].modsEnforced = b->modsEnforced;
				gServers[i].inProgress = b->inProgress;
				gServers[i].modHash = b->modHash;
				gServers[i].lastSeenMs = MpClockMs();
				return;
			}
		}
		else if (freeIdx < 0)
		{
			freeIdx = i;
		}
	}

	if (freeIdx < 0)
		return;		/* list full */

	memset(&gServers[freeIdx], 0, sizeof(gServers[freeIdx]));
	gServers[freeIdx].used = 1;
	snprintf(gServers[freeIdx].ip, sizeof(gServers[freeIdx].ip), "%s", ip);
	gServers[freeIdx].port = b->port;
	snprintf(gServers[freeIdx].hostName, MP_NAME_MAX, "%s", name);
	gServers[freeIdx].players = b->players;
	gServers[freeIdx].maxPlayers = b->maxPlayers;
	gServers[freeIdx].gamemode = b->gamemode;
	gServers[freeIdx].city = b->city;
	gServers[freeIdx].modsEnforced = b->modsEnforced;
	gServers[freeIdx].inProgress = b->inProgress;
	gServers[freeIdx].modHash = b->modHash;
	gServers[freeIdx].lastSeenMs = MpClockMs();
	gDiscoveryRev++;		/* a new row appeared in the browser */

	if (gMpCtx)
		gMpCtx->jer_log(gMpCtx, "[mp] server found: '%s' %s:%d (%d/%d players, mode %d, city %d)\n",
			gServers[freeIdx].hostName, ip, b->port,
			b->players, b->maxPlayers, b->gamemode, b->city);
}

static void MpBeaconRecv(void)
{
	for (;;)
	{
		MP_BEACON b;
		struct sockaddr_in from;
#ifdef _WIN32
		int fl = (int)sizeof(from);
#else
		socklen_t fl = sizeof(from);
#endif
		int n = recvfrom(gBeaconSock, (char*)&b, (int)sizeof(b), 0,
			(struct sockaddr*)&from, &fl);

		if (n <= 0)
			break;

		if (n < (int)sizeof(MP_BEACON))
			continue;

		if (b.magic != MP_UDP_MAGIC || b.protoVersion != (uint16_t)MP_PROTO_VERSION)
			continue;

		MpServerTouch(inet_ntoa(from.sin_addr), &b);
	}
}

void MpDiscoveryPoll(void)
{
	unsigned long now;
	int i;

	if (gBeaconSock == INVALID_SOCKET)
		return;

	now = MpClockMs();

	if (gBeaconAdvertise &&
	    (gLastBeaconMs == 0 || (now - gLastBeaconMs) >= (unsigned long)gMp.config.beaconMs))
	{
		MpBeaconSend();
		gLastBeaconMs = now;
	}

	MpBeaconRecv();

	/* drop silent servers */
	for (i = 0; i < MP_SERVER_SLOTS; i++)
	{
		if (gServers[i].used && (now - gServers[i].lastSeenMs) > MP_BEACON_TIMEOUT_MS)
		{
			if (gMpCtx)	/* say which one left -- it is gone from here */
				gMpCtx->jer_log(gMpCtx, "[mp] server gone: '%s' %s:%d\n",
					gServers[i].hostName, gServers[i].ip, gServers[i].port);

			memset(&gServers[i], 0, sizeof(gServers[i]));
			gDiscoveryRev++;	/* a row left the browser */
		}
	}
}

int MpDiscoveryCount(void)
{
	int i, n = 0;

	for (i = 0; i < MP_SERVER_SLOTS; i++)
		if (gServers[i].used)
			n++;

	return n;
}

MP_SERVER* MpDiscoveryGet(int index)
{
	int i, n = 0;

	for (i = 0; i < MP_SERVER_SLOTS; i++)
	{
		if (gServers[i].used)
		{
			if (n == index)
				return &gServers[i];
			n++;
		}
	}

	return NULL;
}

/* Monotonic counter for the visible server set (mp_ui.c rebuilds the LAN
 * browser only when this changes, so a live list stays stable otherwise). */
int MpDiscoveryRevision(void)
{
	return (int)gDiscoveryRev;
}
