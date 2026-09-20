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
#  include <windows.h>
   /* Adapter list, for the per-interface broadcast addresses below. */
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
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
#  include <ifaddrs.h>
#  include <net/if.h>
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
#include <stdlib.h>		/* malloc/free for the adapter list */

#define MP_RECV_BUF		8192
#define MP_SEND_BUF		2048
#define MP_SEND_QUEUE	8192	/* frames waiting for a busy socket */
#define MP_ACCEPT_BACKLOG	8
#ifdef _WIN32
#  define MP_SHUT_WR		SD_SEND
#else
#  define MP_SHUT_WR		SHUT_WR
#endif

#define MP_SND_TIMEOUT_MS	250
#define MP_CLOSE_GRACE_MS	2000	/* how long a half-closed (refused) peer may
								 * take to read its rejection before we
								 * stop waiting and close the socket */
#define MP_HANDSHAKE_TIMEOUT_MS	5000	/* a peer that connects and then says
								 * nothing is not a player: give up on it
								 * long before the idle timeout */
#define MP_CONN_TIMEOUT_MS	30000	/* drop a peer after 30 s of silence. It was 10 s, and the busy guard that excuses a SILENT LEVEL LOAD is OUR OWN flag (MpBusy) -- the peer's is not visible to us, so a peer that was loading (or just hitching) for longer than 10 s got dropped by the OTHER side: the "it disconnects after a while" report. Real liveness still comes from the 1 s keepalive, which is why this grace can be generous. */
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
	int           hsDone;	/* HELLO/WELCOME exchanged: no longer a stranger */
	unsigned long acceptedMs;	/* when the connection was adopted */
	int           closing;	/* we half-closed this peer (refused it): drain, then close */
	unsigned long closingSinceMs;
	unsigned long lastRecvMs;
	unsigned char rbuf[MP_RECV_BUF];
	int           rbufLen;
	unsigned long pingMs;	/* round trip, from the PING/PONG tick */
	unsigned char sbuf[MP_SEND_QUEUE];	/* frames waiting for the socket */
	int           sbufLen;
	int           sbufOff;	/* bytes already written out */
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

static void MpDropConn(int idx, const char* why);	/* used by the send queue */

/* Would this send only have had to wait? Non-blocking sockets say so this way,
 * and it is NOT an error -- it is the normal case on a busy link. */
static int MpWouldBlock(void)
{
#ifdef _WIN32
	int e = WSAGetLastError();

	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/* Write whatever is queued for this peer. 1 = still fine (possibly with bytes
 * left to send), 0 = the peer is gone. */
static int MpFlushConn(int idx)
{
	MP_CONN* c;

	if (idx < 0 || idx >= MP_MAX_PLAYERS || !gConn[idx].used)
		return 0;

	c = &gConn[idx];

	while (c->sbufOff < c->sbufLen)
	{
		int n = send(c->sock, (const char*)c->sbuf + c->sbufOff, c->sbufLen - c->sbufOff, 0);

		if (n > 0)
		{
			if (getenv("MP_DEBUG") != NULL && gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx, "[mp] wrote %d byte(s) conn=%d\n", n, idx);

			c->sbufOff += n;
			continue;
		}

		if (n < 0 && MpWouldBlock())
			return 1;		/* it will take more next poll */

		return 0;			/* a real error: this peer is gone */
	}

	c->sbufLen = 0;
	c->sbufOff = 0;

	return 1;
}

/* Queue a frame and write what the socket will take now.
 *
 * This used to hand the bytes straight to send() and DROP the frame whenever the
 * non-blocking socket was not ready -- and, worse, give up after a partial write,
 * leaving half a frame on the wire. Both are fatal for a reliable frame: a lost
 * WELCOME leaves the joining player sitting there until their timeout expires,
 * and a half-written frame desynchronises the stream for good. Bytes the socket
 * will not take yet wait in the queue and go out on the next poll instead. */
static int MpSendRaw(int idx, const void* data, int len)
{
	MP_CONN* c;

	if (idx < 0 || idx >= MP_MAX_PLAYERS || !gConn[idx].used)
		return 0;

	c = &gConn[idx];

	/* whatever was already written is finished with: close the gap */
	if (c->sbufOff > 0)
	{
		int left = c->sbufLen - c->sbufOff;

		if (left > 0)
			memmove(c->sbuf, c->sbuf + c->sbufOff, (size_t)left);

		c->sbufLen = left;
		c->sbufOff = 0;
	}

	if (len > 0 && c->sbufLen + len > (int)sizeof(c->sbuf))
	{
		/* a peer that will not read even this much is not coming back */
		MpDropConn(idx, "peer is not reading");
		return 0;
	}

	if (len > 0)
	{
		memcpy(c->sbuf + c->sbufLen, data, (size_t)len);
		c->sbufLen += len;
	}

	MpFlushConn(idx);

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
			gConn[i].acceptedMs = MpClockMs();
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

/* Is this a dotted-quad IPv4 address? There is no DNS anywhere in this module,
 * so anything else is a mistake worth reporting rather than something to
 * silently aim at the loopback. */
int MpIsValidAddress(const char* host)
{
	return host != NULL && host[0] != '\0' && inet_addr(host) != INADDR_NONE;
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
		/* not a dotted quad -- fail rather than quietly dial the loopback, so
		 * a typo is reported instead of looking like a dead server */
		closesocket(s);
		return INVALID_SOCKET;
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
/* The round trip to the peer playing `playerId`, or 0 if unknown. The host is
 * the only side that has one for everybody, so it is the host's measurement that
 * travels in the roster. */
int MpPingForPlayer(int playerId)
{
	int i;

	for (i = 0; i < MP_MAX_PLAYERS; i++)
	{
		if (gConn[i].used && gConn[i].playerId == playerId)
			return (int)gConn[i].pingMs;
	}

	return 0;
}

/* Record a PONG's round trip against the peer that owns this connection. */
void MpConnSetPing(int idx, unsigned long ms)
{
	if (idx >= 0 && idx < MP_MAX_PLAYERS && gConn[idx].used)
		gConn[idx].pingMs = ms;
}

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

	return MpSendRaw(idx, buf, MP_ENVELOPE_SIZE + len);
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

/* Refuse a peer without throwing the refusal away. closesocket() immediately
 * after send() can RST and discard what we just wrote, so half-close and let
 * the peer read the FIN; the poll closes the socket once the peer is done or
 * the grace period expires. */
void MpConnShutdownGraceful(int connIndex)
{
	MP_CONN* c;

	if (connIndex < 0 || connIndex >= MP_MAX_PLAYERS || !gConn[connIndex].used)
		return;

	c = &gConn[connIndex];

	if (!c->closing)
	{
		shutdown(c->sock, MP_SHUT_WR);
		c->closing = 1;
		c->closingSinceMs = MpClockMs();
	}
}

/* A HELLO or a WELCOME has been seen on this connection, so it is no longer
 * within the handshake deadline. */
void MpConnHandshakeDone(int connIndex)
{
	if (connIndex >= 0 && connIndex < MP_MAX_PLAYERS && gConn[connIndex].used)
		gConn[connIndex].hsDone = 1;
}

static void MpProcessConn(int idx)
{
	MP_CONN* c = &gConn[idx];
	char buf[2048];
	int eof = 0;

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

		{
			int sel = select(0, &rd, NULL, NULL, &tv);
			int isset = FD_ISSET(c->sock, &rd);

			if (getenv("MP_DEBUG") != NULL && gMpCtx != NULL)


			if (sel <= 0 || !isset)
				break;
		}

		n = recv(c->sock, buf, (int)sizeof(buf), 0);
		if (n <= 0 && getenv("MP_DEBUG") != NULL && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] recv n=%d\n", n);

		if (n > 0 && getenv("MP_DEBUG") != NULL && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] read %d byte(s) conn=%d\n", n, idx);

		if (n == 0)
		{
			/* The peer hung up WITH BYTES STILL QUEUED: TCP hands over the
			 * data and the FIN together. Dropping here would throw away a
			 * REJECT, a WELCOME or a LEAVE sitting in the buffer -- which is
			 * exactly how "refused, your build differs" reached the player as
			 * "Connection to the server lost!". Remember the EOF and drain
			 * the buffer first. */
			eof = 1;
			break;
		}

		if (n < 0)
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

	/* Now act on the EOF. Whatever is left is a half-frame we can never
	 * complete, so say so rather than pretending it was a clean close. */
	if (eof && gConn[idx].used)
		MpDropConn(idx, gConn[idx].rbufLen > 0 ? "closed mid-frame" : "closed");
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

		/* anything that had to wait for the socket last poll goes out now */
		if (gConn[i].used && !MpFlushConn(i))
		{
			MpDropConn(i, "send failed");
			continue;
		}

		/* a peer we refused: give it time to read the refusal, then let it
		 * go even if it never closes its side */
		if (gConn[i].used && gConn[i].closing &&
			(now - gConn[i].closingSinceMs) > MP_CLOSE_GRACE_MS)
		{
			MpDropConn(i, "refusal not acknowledged");
			continue;
		}

		/* A peer that connects and then says nothing is not a player. The idle
		 * timeout would eventually catch it, but 10 s of silence is a long time
		 * to hold a slot -- and on the client side the player is staring at
		 * "Connecting to ..." with no idea anything is wrong. */
		if (gConn[i].used && !gConn[i].hsDone &&
			(now - gConn[i].acceptedMs) > MP_HANDSHAKE_TIMEOUT_MS)
		{
			if (!MpIsHost())
				jer_error("The server did not answer at %s", gConnectingHost);

			MpDropConn(i, "no handshake reply");
			continue;
		}

		/* ...but not while a level is loading: both sides go silent for the whole
		 * load, and that is longer than the idle timeout. */
		if (gConn[i].used && (now - gConn[i].lastRecvMs) > MP_CONN_TIMEOUT_MS && !MpBusy())
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

/* Every broadcast address worth trying, limited broadcast first.
 *
 * Sending only to 255.255.255.255 is the classic reason a host is invisible
 * on a machine with more than one adapter (Wi-Fi + Ethernet + VPN + Hyper-V
 * or WSL): Windows picks one adapter to send it out of, and it is regularly
 * the wrong one. A directed broadcast per adapter (ip | ~mask) removes the
 * guesswork. */
static int MpBroadcastTargets(uint32_t* out, int max)
{
	int n = 0;

	if (max <= 0)
		return 0;

	out[n++] = INADDR_BROADCAST;

#ifdef _WIN32
	{
		IP_ADAPTER_INFO* list = NULL;
		ULONG len = 0;

		if (GetAdaptersInfo(NULL, &len) == ERROR_BUFFER_OVERFLOW && len > 0)
		{
			list = (IP_ADAPTER_INFO*)malloc(len);

			if (list != NULL && GetAdaptersInfo(list, &len) == ERROR_SUCCESS)
			{
				IP_ADAPTER_INFO* a;

				for (a = list; a != NULL; a = a->Next)
				{
					IP_ADDR_STRING* ipa;

					for (ipa = &a->IpAddressList; ipa != NULL; ipa = ipa->Next)
					{
						uint32_t ip = inet_addr(ipa->IpAddress.String);
						uint32_t mask = inet_addr(ipa->IpMask.String);
						uint32_t bcast;
						int i, seen = 0;

						if (ip == INADDR_NONE || mask == INADDR_NONE)
							continue;

						if ((ip & 0xFF000000u) == 0x7F000000u)	/* loopback */
							continue;

						bcast = htonl((ntohl(ip) & ntohl(mask)) | ~ntohl(mask));

						for (i = 0; i < n; i++)			/* no duplicates */
							if (out[i] == bcast)
								seen = 1;

						if (!seen && n < max)
							out[n++] = bcast;
					}
				}
			}

			free(list);
		}
	}
#else
	{
		struct ifaddrs* ifa = NULL;

		if (getifaddrs(&ifa) == 0)
		{
			struct ifaddrs* p;

			for (p = ifa; p != NULL; p = p->ifa_next)
			{
				uint32_t bcast;
				int i, seen = 0;

				if (p->ifa_addr == NULL || p->ifa_netmask == NULL ||
					p->ifa_addr->sa_family != AF_INET)
					continue;

				{
					uint32_t ip = ntohl(((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr);
					uint32_t mask = ntohl(((struct sockaddr_in*)p->ifa_netmask)->sin_addr.s_addr);

					if ((ip & 0xFF000000u) == 0x7F000000u)
						continue;

					bcast = htonl((ip & mask) | ~mask);
				}

				for (i = 0; i < n; i++)
					if (out[i] == bcast)
						seen = 1;

				if (!seen && n < max)
					out[n++] = bcast;
			}

			freeifaddrs(ifa);
		}
	}
#endif

	return n;
}

/* LAN discovery fails silently by nature: if the UDP socket or its bind fails,
 * nothing else in the module notices, and the player just gets an empty server
 * list with no explanation. Say so -- in the log and on screen.
 *
 * NOTE: this cannot open the Windows Firewall for you. Inbound UDP/TCP 1318 has
 * to be allowed, or a host on another machine stays invisible even though
 * everything here succeeded. */
static void MpDiscoveryFailed(const char* why)
{
	char text[128];

	snprintf(text, sizeof(text), "%s (UDP/%d)", why, MP_DISCOVERY_PORT);

	if (gMpCtx != NULL)
		gMpCtx->jer_log(gMpCtx, "[mp] discovery unavailable: %s\n", text);

	jer_error("LAN discovery is off: %s", text);
}

void MpDiscoveryStart(int advertise)
{
	struct sockaddr_in addr;
	int one = 1;

	if (gBeaconSock == INVALID_SOCKET)
	{
		gBeaconSock = socket(AF_INET, SOCK_DGRAM, 0);
		if (gBeaconSock == INVALID_SOCKET)
		{
			MpDiscoveryFailed("could not open a discovery socket");
			return;
		}

		setsockopt(gBeaconSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
		setsockopt(gBeaconSock, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((unsigned short)MP_DISCOVERY_PORT);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);

		if (bind(gBeaconSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			MpCloseSock(&gBeaconSock);
			MpDiscoveryFailed("the port is already taken by another program");
			return;
		}

		MpSetNonBlocking(gBeaconSock, 1);
	}

	gBeaconAdvertise = advertise;
	gLastBeaconMs = 0;

	if (gMpCtx)
	{
		uint32_t targets[8];

		gMpCtx->jer_log(gMpCtx, "[mp] discovery %s on UDP/%d (session TCP/%d), %d broadcast target(s)\n",
			advertise ? "advertising" : "browsing", MP_DISCOVERY_PORT, gMp.config.port,
			MpBroadcastTargets(targets, (int)(sizeof(targets) / sizeof(targets[0]))));
	}
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
	uint32_t targets[8];
	int n, i;

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

	/* the beacon goes to the FIXED discovery port; b.port carries the session
	 * port so the browser knows where to connect. Sent once per broadcast
	 * address -- see MpBroadcastTargets. */
	n = MpBroadcastTargets(targets, (int)(sizeof(targets) / sizeof(targets[0])));

	for (i = 0; i < n; i++)
	{
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((unsigned short)MP_DISCOVERY_PORT);
		addr.sin_addr.s_addr = targets[i];

		sendto(gBeaconSock, (const char*)&b, (int)sizeof(b), 0,
			(struct sockaddr*)&addr, sizeof(addr));
	}
}

static void MpServerTouch(const char* ip, const MP_BEACON* b)
{
	char name[MP_NAME_MAX];
	int i, freeIdx = -1;

	/* A host beacons and also receives its own beacon, so on a single machine
	 * it lists itself and offers to join a game it is already running. We hold
	 * our own session port, so a beacon advertising it is us: skip it.
	 *
	 * A second copy of the game cannot bind the same session port, so this
	 * cannot hide a genuine companion instance -- which is exactly what
	 * same-machine testing uses. */
	if (MpIsHost() && (int)b->port == gMp.config.port)
		return;

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
