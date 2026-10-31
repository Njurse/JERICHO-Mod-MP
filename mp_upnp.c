/*
 * mp_upnp.c -- JERICHO Multiplayer: automatic UPnP port mapping.
 *
 * On host start the module tries to open a port mapping on the local Internet
 * Gateway Device (IGD) for the session's TCP and UDP port, so a friend outside
 * the LAN can reach the game without touching the router. The whole dance --
 * SSDP discovery, the device description, GetExternalIPAddress and
 * AddPortMapping -- runs on a short-lived worker thread, because SSDP and the
 * router's HTTP replies can take seconds and none of it may stall a frame.
 *
 *   - Discovery: SSDP M-SEARCH on 239.255.255.250:1900 for
 *     urn:schemas-upnp-org:device:InternetGatewayDevice:1; the LOCATION header
 *     of each reply names the device's description document.
 *   - Description: an HTTP GET of that document, scanned (no XML parser -- a
 *     router description is small and a fixed-subset read is all we trust) for
 *     a WANIPConnection / WANPPPConnection service and its controlURL.
 *   - Mapping: SOAP AddPortMapping for TCP then UDP, plus GetExternalIPAddress
 *     so the host can show a copyable public address for Direct Connect.
 *   - Teardown: DeletePortMapping for anything we mapped, best effort.
 *
 * Nothing here is required for a session: a failure costs one console line and
 * the session still works on the LAN and by direct address, exactly like a
 * master server that cannot be reached. There is deliberately no retry storm:
 * one attempt per host, one line about the outcome.
 *
 * The worker touches ONLY its own sockets and the result fields guarded by the
 * volatile flags below; all logging happens on the game thread (MpUpnpPoll).
 */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <process.h>		/* _beginthreadex (the CRT-safe spawn) */
#  pragma comment(lib, "ws2_32.lib")
#  undef OpenEvent			/* same clash-defuse mp_net.c does */
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <pthread.h>
   typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define closesocket    close
#endif

#include "jericho.h"
#include "jer_console.h"
#include "mp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* How long the worker will wait on any one network read before giving up. The
 * thread owns these waits, so a timeout only abandons THIS attempt -- never a
 * frame. SSDP discovery is given the MX window plus slack; HTTP hops are shorter
 * because a router on the LAN answers in milliseconds. */
#define MP_UPNP_SSDP_MS		2500
#define MP_UPNP_HTTP_MS		2500
#define MP_UPNP_DELETE_MS	1500

/* The single worker attempt, on a shared result block guarded by volatile flags.
 * The worker writes the fields, sets `done`, then parks waiting for `stop`; the
 * game thread reads only after `done` is set and never while `running` is about
 * to be cleared (MpUpnpStart waits for the previous attempt to finish). */
static volatile int gUpnpRunning;	/* a worker is alive (mapping OR teardown) */
static volatile int gUpnpStop;		/* teardown requested: delete + exit */
static volatile int gUpnpDone;		/* the mapping attempt finished */
static volatile int gUpnpOk;		/* at least one protocol mapped */
static volatile int gUpnpTcp;		/* TCP AddPortMapping succeeded */
static volatile int gUpnpUdp;		/* UDP AddPortMapping succeeded */
static int  gUpnpReported;		/* the outcome has reached the console */
static int  gUpnpPortMap;		/* the session port to map */

/* Result + teardown parameters, written by the worker, read by MpUpnpPoll and
 * (during the teardown tail) by the worker itself. Stable for the lifetime of
 * one attempt because MpUpnpStart waits for `running` to clear first. */
static char gUpnpPublicIp[64];
static char gUpnpReason[128];
static char gUpnpInternalIp[64];
static char gUpnpHost[64];
static int  gUpnpPort = 80;
static char gUpnpService[96];
static char gUpnpCtlUrl[256];

static void MpUpnpSleep(int ms)
{
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	usleep((useconds_t)ms * 1000);
#endif
}

/* A socket that waits at most `ms` on a recv/send instead of blocking forever. */
static void MpUpnpSetTimeout(SOCKET s, int ms)
{
#ifdef _WIN32
	DWORD tv = (DWORD)ms;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
	struct timeval tv;

	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

/* The LAN address a router should forward to -- the one this host would use to
 * reach the internet, not the loopback. A UDP connect() to a public address is
 * the standard way to ask the OS which local address that is, without sending a
 * packet. */
static void MpUpnpLocalIp(char* out, size_t cap)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a, local;
#ifdef _WIN32
	int len = (int)sizeof(local);
#else
	socklen_t len = sizeof(local);
#endif

	out[0] = 0;
	if (s == INVALID_SOCKET)
		return;

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = inet_addr("8.8.8.8");
	a.sin_port = htons(9);

	if (connect(s, (struct sockaddr*)&a, sizeof(a)) == 0 &&
	    getsockname(s, (struct sockaddr*)&local, &len) == 0)
	{
		snprintf(out, cap, "%s", inet_ntoa(local.sin_addr));
	}

	closesocket(s);
}

static int MpUpnpLower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Extract the value of one HTTP/SSDP header, case-insensitively. */
static void MpUpnpHeaderValue(const char* msg, const char* name, char* out, int cap)
{
	const char* p = msg;
	size_t nameLen = strlen(name);

	out[0] = 0;

	for (p = msg; p != NULL && *p != '\0'; )
	{
		int match = 1;
		size_t i;

		for (i = 0; i < nameLen; i++)
		{
			if (MpUpnpLower(p[i]) != MpUpnpLower(name[i]))
			{
				match = 0;
				break;
			}
		}

		if (match && p[nameLen] == ':')
		{
			const char* v = p + nameLen + 1;
			const char* e;

			while (*v == ' ' || *v == '\t')
				v++;

			e = v;
			while (*e != '\0' && *e != '\r' && *e != '\n')
				e++;

			{
				size_t n = (size_t)(e - v);

				if (n >= (size_t)cap)
					n = (size_t)cap - 1;

				memcpy(out, v, n);
				out[n] = 0;
			}
			return;
		}

		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
}

/* Collect the LOCATION headers of every IGD that answers an M-SEARCH. */
static int MpUpnpSsdpSearch(char locs[][256], int max)
{
	static const char query[] =
		"M-SEARCH * HTTP/1.1\r\n"
		"HOST: 239.255.255.250:1900\r\n"
		"MAN: \"ssdp:discover\"\r\n"
		"MX: 2\r\n"
		"ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
		"\r\n";
	SOCKET s;
	struct sockaddr_in local, to;
	int one = 1, n = 0;

	if (max <= 0)
		return 0;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == INVALID_SOCKET)
		return 0;

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = 0;

	if (bind(s, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
	{
		closesocket(s);
		return 0;
	}

	{
		struct ip_mreq mreq;

		memset(&mreq, 0, sizeof(mreq));
		mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
		mreq.imr_interface.s_addr = htonl(INADDR_ANY);
		setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));
	}

	MpUpnpSetTimeout(s, MP_UPNP_SSDP_MS);

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = inet_addr("239.255.255.250");
	to.sin_port = htons(1900);

	sendto(s, query, (int)strlen(query), 0, (struct sockaddr*)&to, sizeof(to));

	for (;;)
	{
		char buf[2048];
		struct sockaddr_in from;
#ifdef _WIN32
		int fl = (int)sizeof(from);
#else
		socklen_t fl = sizeof(from);
#endif
		int r = recvfrom(s, buf, (int)sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fl);

		if (r <= 0)
			break;		/* the SSDP window closed */

		buf[r] = 0;

		{
			char loc[256];
			int dup = 0, k;

			MpUpnpHeaderValue(buf, "location", loc, (int)sizeof(loc));

			if (loc[0] == 0)
				continue;

			for (k = 0; k < n; k++)
			{
				if (strcmp(locs[k], loc) == 0)
					dup = 1;
			}

			if (!dup)
			{
				snprintf(locs[n], 256, "%s", loc);
				n++;
				if (n >= max)
					break;
			}
		}
	}

	closesocket(s);
	return n;
}

/* Split an http://host[:port]/path into its parts. The router's own LOCATION is
 * essentially always an IP literal, so a host name is not resolved here. */
static int MpUpnpParseLocation(const char* url, char* host, int hostCap,
	int* port, char* path, int pathCap)
{
	const char* p;
	const char* hostStart, *hostEnd;

	*port = 80;
	path[0] = '/';
	path[1] = 0;

	if (strncmp(url, "http://", 7) != 0)
		return 0;

	p = url + 7;
	hostStart = p;
	hostEnd = hostStart;

	while (*hostEnd != '\0' && *hostEnd != ':' && *hostEnd != '/')
		hostEnd++;

	if (hostEnd == hostStart)
		return 0;

	{
		size_t hn = (size_t)(hostEnd - hostStart);

		if (hn >= (size_t)hostCap)
			hn = (size_t)hostCap - 1;

		memcpy(host, hostStart, hn);
		host[hn] = 0;
	}

	if (*hostEnd == ':')
	{
		*port = atoi(hostEnd + 1);
		while (*hostEnd != '\0' && *hostEnd != '/')
			hostEnd++;
	}

	if (*hostEnd == '/')
		snprintf(path, (size_t)pathCap, "%s", hostEnd);

	return 1;
}

/* A small blocking HTTP GET (used only by the worker). */
static int MpUpnpHttpGet(const char* host, int port, const char* path,
	char* out, int cap)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	char req[512];
	int total = 0;

	if (s == INVALID_SOCKET)
		return 0;

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	a.sin_addr.s_addr = inet_addr(host);

	if (a.sin_addr.s_addr == INADDR_NONE)
	{
		closesocket(s);
		return 0;
	}

	MpUpnpSetTimeout(s, MP_UPNP_HTTP_MS);

	if (connect(s, (struct sockaddr*)&a, sizeof(a)) == SOCKET_ERROR)
	{
		closesocket(s);
		return 0;
	}

	snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Connection: close\r\n"
		"\r\n",
		path, host, port);

	if (send(s, req, (int)strlen(req), 0) < 0)
	{
		closesocket(s);
		return 0;
	}

	for (;;)
	{
		int r = recv(s, out + total, cap - total - 1, 0);

		if (r <= 0)
			break;

		total += r;
		if (total >= cap - 1)
			break;
	}

	out[total] = 0;
	closesocket(s);

	return total > 0;
}

/* Find a WANIPConnection (or WANPPPConnection) service and its controlURL in
 * the description document. Fixed-subset string scan, no XML parser. */
static int MpUpnpParseDesc(const char* xml, char* service, int svcCap,
	char* ctlUrl, int ctlCap)
{
	const char* p;
	const char* afterSvc = NULL;

	service[0] = 0;
	ctlUrl[0] = 0;

	for (p = xml; (p = strstr(p, "<serviceType>")) != NULL; p += strlen("<serviceType>"))
	{
		const char* v = p + strlen("<serviceType>");
		const char* e = strstr(v, "</serviceType>");
		char buf[96];
		size_t len;

		if (e == NULL)
			break;

		len = (size_t)(e - v);

		if (len == 0 || len >= sizeof(buf))
			continue;

		memcpy(buf, v, len);
		buf[len] = 0;

		if (strstr(buf, "WANIPConnection") != NULL || strstr(buf, "WANPPPConnection") != NULL)
		{
			snprintf(service, (size_t)svcCap, "%s", buf);
			afterSvc = e;
			break;
		}
	}

	if (afterSvc == NULL)
		return 0;

	{
		const char* c = strstr(afterSvc, "<controlURL>");

		if (c != NULL)
		{
			const char* v = c + strlen("<controlURL>");
			const char* e = strstr(v, "</controlURL>");

			if (e != NULL)
			{
				size_t len = (size_t)(e - v);

				if (len > 0 && len < (size_t)ctlCap)
				{
					memcpy(ctlUrl, v, len);
					ctlUrl[len] = 0;
					return 1;
				}
			}
		}
	}

	return 0;
}

/* One SOAP action over HTTP. Returns the HTTP status code, or 0 when no HTTP
 * reply could be read. */
static int MpUpnpSoap(const char* host, int port, const char* path,
	const char* serviceType, const char* action, const char* bodyArgs,
	char* resp, int cap)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	char body[1024];
	char req[1600];
	int total = 0, status = 0;

	if (s == INVALID_SOCKET)
		return 0;

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	a.sin_addr.s_addr = inet_addr(host);

	if (a.sin_addr.s_addr == INADDR_NONE)
	{
		closesocket(s);
		return 0;
	}

	MpUpnpSetTimeout(s, MP_UPNP_HTTP_MS);

	if (connect(s, (struct sockaddr*)&a, sizeof(a)) == SOCKET_ERROR)
	{
		closesocket(s);
		return 0;
	}

	snprintf(body, sizeof(body),
		"<?xml version=\"1.0\"?>"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		"<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
		action, serviceType, bodyArgs, action);

	snprintf(req, sizeof(req),
		"POST %s HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Content-Type: text/xml; charset=\"utf-8\"\r\n"
		"SOAPAction: \"%s#%s\"\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n"
		"%s",
		path, host, port, serviceType, action, (int)strlen(body), body);

	if (send(s, req, (int)strlen(req), 0) < 0)
	{
		closesocket(s);
		return 0;
	}

	for (;;)
	{
		int r = recv(s, resp + total, cap - total - 1, 0);

		if (r <= 0)
			break;

		total += r;
		if (total >= cap - 1)
			break;
	}

	resp[total] = 0;
	closesocket(s);

	if (strncmp(resp, "HTTP/1.", 7) == 0)
	{
		const char* sp = strchr(resp, ' ');

		if (sp != NULL)
			status = atoi(sp + 1);
	}

	return status;
}

static int MpUpnpAddMapping(const char* proto)
{
	char args[512];
	char resp[1024];
	int status;

	snprintf(args, sizeof(args),
		"<NewRemoteHost></NewRemoteHost>"
		"<NewExternalPort>%d</NewExternalPort>"
		"<NewProtocol>%s</NewProtocol>"
		"<NewInternalPort>%d</NewInternalPort>"
		"<NewInternalClient>%s</NewInternalClient>"
		"<NewEnabled>1</NewEnabled>"
		"<NewPortMappingDescription>JERICHO</NewPortMappingDescription>"
		"<NewLeaseDuration>0</NewLeaseDuration>",
		gUpnpPortMap, proto, gUpnpPortMap, gUpnpInternalIp);

	status = MpUpnpSoap(gUpnpHost, gUpnpPort, gUpnpCtlUrl, gUpnpService,
		"AddPortMapping", args, resp, (int)sizeof(resp));

	if (status == 200)
		return 1;

	/* A router that already has this exact mapping answers 500 with this fault.
	 * For our purposes the port IS mapped, so treat it as success rather than
	 * reporting a false failure. */
	if (status == 500 && strstr(resp, "ConflictInMappingEntry") != NULL)
		return 1;

	return 0;
}

static int MpUpnpGetExternalIp(void)
{
	char resp[1024];
	int status;

	gUpnpPublicIp[0] = 0;

	status = MpUpnpSoap(gUpnpHost, gUpnpPort, gUpnpCtlUrl, gUpnpService,
		"GetExternalIPAddress", "<NewExternalIPAddress></NewExternalIPAddress>",
		resp, (int)sizeof(resp));

	if (status == 200)
	{
		const char* p = strstr(resp, "<NewExternalIPAddress>");

		if (p != NULL)
		{
			const char* v = p + strlen("<NewExternalIPAddress>");
			const char* e = strstr(v, "</NewExternalIPAddress>");

			if (e != NULL && (size_t)(e - v) < sizeof(gUpnpPublicIp))
			{
				size_t n = (size_t)(e - v);

				memcpy(gUpnpPublicIp, v, n);
				gUpnpPublicIp[n] = 0;
				return 1;
			}
		}
	}

	return 0;
}

static void MpUpnpDeleteMapping(const char* proto)
{
	char args[256];
	char resp[512];

	snprintf(args, sizeof(args),
		"<NewRemoteHost></NewRemoteHost>"
		"<NewExternalPort>%d</NewExternalPort>"
		"<NewProtocol>%s</NewProtocol>",
		gUpnpPortMap, proto);

	MpUpnpSoap(gUpnpHost, gUpnpPort, gUpnpCtlUrl, gUpnpService,
		"DeletePortMapping", args, resp, (int)sizeof(resp));
}

#ifdef _WIN32
static DWORD WINAPI MpUpnpWorker(LPVOID unused)
#else
static void* MpUpnpWorker(void* unused)
#endif
{
	char locs[8][256];
	char descPath[256];
	char desc[8192];
	int nlocs, i;
	int tcp = 0, udp = 0;

	(void)unused;

	MpUpnpLocalIp(gUpnpInternalIp, sizeof(gUpnpInternalIp));

	if (gUpnpInternalIp[0] != 0)
	{
		nlocs = MpUpnpSsdpSearch(locs, 8);

		for (i = 0; i < nlocs && !tcp && !gUpnpStop; i++)
		{
			if (!MpUpnpParseLocation(locs[i], gUpnpHost, (int)sizeof(gUpnpHost),
					&gUpnpPort, descPath, (int)sizeof(descPath)))
				continue;

			if (!MpUpnpHttpGet(gUpnpHost, gUpnpPort, descPath, desc, (int)sizeof(desc)))
				continue;

			if (!MpUpnpParseDesc(desc, gUpnpService, (int)sizeof(gUpnpService),
					gUpnpCtlUrl, (int)sizeof(gUpnpCtlUrl)))
				continue;

			/* The public address is a nice-to-have, not a gate: the mapping can
			 * succeed even when a router declines to report its external IP. */
			MpUpnpGetExternalIp();

			tcp = MpUpnpAddMapping("TCP");
			udp = MpUpnpAddMapping("UDP");

			if (tcp)
				break;		/* found a gateway that accepted the mapping */
		}

		if (!tcp && !udp && nlocs <= 0)
			snprintf(gUpnpReason, sizeof(gUpnpReason), "no UPnP gateway answered");
		else if (!tcp && !udp)
			snprintf(gUpnpReason, sizeof(gUpnpReason), "no gateway accepted the mapping");
	}
	else
	{
		snprintf(gUpnpReason, sizeof(gUpnpReason), "no local address to map");
	}

	gUpnpTcp = tcp;
	gUpnpUdp = udp;
	gUpnpOk = (tcp || udp) ? 1 : 0;
	gUpnpDone = 1;

	/* Park until the session tears down, then remove what we added. */
	while (!gUpnpStop)
		MpUpnpSleep(100);

	if (tcp)
		MpUpnpDeleteMapping("TCP");
	if (udp)
		MpUpnpDeleteMapping("UDP");

	gUpnpRunning = 0;

#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif
}

static void MpUpnpThreadStart(void)
{
#ifdef _WIN32
	HANDLE h = CreateThread(NULL, 0, MpUpnpWorker, NULL, 0, NULL);

	if (h != NULL)
		CloseHandle(h);		/* detached: the worker outlives the handle */
#else
	pthread_t t;

	if (pthread_create(&t, NULL, MpUpnpWorker, NULL) == 0)
		pthread_detach(t);
	else
		gUpnpRunning = 0;
#endif
}

void MpUpnpStart(int port)
{
	int waited = 0;

	/* A previous session's teardown tail (the best-effort DeletePortMapping)
	 * runs on the same worker. Give it a moment to finish before starting a new
	 * attempt; if it is stuck on a dead router, skip mapping rather than race
	 * the shared result fields -- the session still works on the LAN. */
	while (gUpnpRunning && waited < 500)
	{
		MpUpnpSleep(25);
		waited += 25;
	}

	if (gUpnpRunning)
	{
		jer_console_log("UPnP: previous attempt still cleaning up - skipping");
		return;
	}

	gUpnpStop = 0;
	gUpnpDone = 0;
	gUpnpOk = 0;
	gUpnpTcp = 0;
	gUpnpUdp = 0;
	gUpnpReported = 0;
	gUpnpPortMap = port;
	gUpnpPublicIp[0] = 0;
	gUpnpReason[0] = 0;
	gUpnpInternalIp[0] = 0;
	gUpnpHost[0] = 0;
	gUpnpPort = 80;
	gUpnpService[0] = 0;
	gUpnpCtlUrl[0] = 0;

	gUpnpRunning = 1;
	MpUpnpThreadStart();
}

/* Called once a frame from the game thread: report the outcome exactly once. */
void MpUpnpPoll(void)
{
	if (!gUpnpDone || gUpnpReported)
		return;

	gUpnpReported = 1;

	if (gUpnpOk)
	{
		const char* pub = (gUpnpPublicIp[0] != 0) ? gUpnpPublicIp : "unknown";

		if (gUpnpTcp && gUpnpUdp)
			jer_console_log("UPnP: router port %d mapped (TCP+UDP) - public %s:%d",
				gUpnpPortMap, pub, gUpnpPortMap);
		else
			jer_console_log("UPnP: router port %d mapped (%s only) - public %s",
				gUpnpPortMap, gUpnpTcp ? "TCP" : "UDP", pub);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] upnp: port %d mapped (tcp=%d udp=%d) - public %s\n",
				gUpnpPortMap, gUpnpTcp, gUpnpUdp, pub);

		if (gUpnpPublicIp[0] != 0)
			jer_console_log("Direct Connect: %s:%d", gUpnpPublicIp, gUpnpPortMap);
	}
	else
	{
		jer_console_log("UPnP: couldn't open router port %d - LAN still works, or use Direct Connect",
			gUpnpPortMap);

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx, "[mp] upnp: no mapping for port %d (%s) - LAN/direct only\n",
				gUpnpPortMap, gUpnpReason[0] ? gUpnpReason : "failed");
	}
}

/* Ask the worker to remove its mappings and exit. Non-blocking: the worker
 * wakes within its poll interval, deletes what it added and returns. */
void MpUpnpStop(void)
{
	gUpnpStop = 1;
}
