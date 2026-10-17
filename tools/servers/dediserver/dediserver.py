#!/usr/bin/env python3
"""dediserver.py -- a headless dedicated server for the JERICHO mp mod.

It hosts a real session with no game window: it accepts players, hands out ids,
relays their car-state and chat, answers PING with PONG, and optionally beacons
on UDP so the in-game LAN browser lists it. A client has something to join and
the server can be left up while the client side restarts.

    python dediserver.py                       # TCP + UDP on 1318, beaconing
    python dediserver.py --config dediserver.json
    python dediserver.py --port 1400 --name "Bob's 24/7" --no-beacon
    python dediserver.py --master http://master.example.com:8100

Then point a client at it:

    JERICHO_dev.exe -nointro -nofmv -join 127.0.0.1
    JERICHO_dev.exe -nointro -nofmv -join 127.0.0.1:1400

The wire format lives in mp_test.py (imported so there is only one copy), and
servers/proto_check.py fails at startup if that mirror has drifted from
mp_proto.h. With --master the server registers with a master discovery service
(servers/masterserver/) and heartbeats, so internet clients can find it.
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import threading
from urllib.parse import urlsplit
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from serverkit import (LEVELS, Console, Log, apply_overrides,   # noqa: E402
                       duration, install_signals, load_config)

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(os.path.dirname(HERE))      # .../mp/tools
SERVERS = os.path.dirname(HERE)                      # .../mp/tools/servers
sys.path.insert(0, TOOLS)
sys.path.insert(0, SERVERS)
import mp_test as m            # noqa: E402  the single Python source of truth
import proto_check            # noqa: E402  the wire-mirror guard


# ------------------------------------------------------------------------ config
DEFAULTS = {
    "port": 1318,
    "name": "Dedicated",
    "city": 0,                 # GameLevel the clients should load (0..3)
    "gamemode": 0,
    "max_players": 8,          # a client takes a slot 1..max_players-1 (0 is the server)
    "beacon": True,            # advertise on UDP so the LAN browser lists us
    "beacon_ms": 1000,
    "idle_drop_s": 30.0,       # real silence before a client is dropped (0 = never)
    "live": True,              # advertise as a live match (the level is always up)
    "rules": 0,                # MP_RULES_PACK'd session rules (1 = cops); names the mode
    "log_level": "info",
    "log_file": "",
    # master discovery (optional). master = "" disables registration.
    "master": "http://jerichomp.animetiddi.es:8100",   # "" disables registration
    "master_name": "",
    "advertise_host": "",     # public address players reach us on ("" = the address we register from); set this behind NAT
    "advertise_port": 0,      # public port players dial (0 = our session port); set this when the router REMAPS it
    "mod_check": 0,            # 0 = admit anyone, 1 = ids must match, 2 = ids AND versions
    "mods": [],                # the ids a client must have enabled when mod_check is on
    "mod_versions": {},        # id -> version, consulted only by mod_check 2
    "master_interval_s": 20.0, # heartbeat cadence; keep well under the master's ttl
    "master_timeout_s": 5.0,
}

# How often the roster is re-broadcast. mp_session.c sends it every 120 frames,
# which is ~2s at 60 fps -- the same cadence a real host uses, so a client sees
# peers appear and leave on the timing it was written against.
ROSTER_REFRESH_S = 2.0

CONFIG_NAME = "dediserver.json"


def parse_manifest(payload):
    """The MP_MOD_INFO rows that ride after an MP_HELLO, as [(id, version, enabled)].

    The manifest travels INSIDE the hello frame rather than as its own message, so a
    short payload is not an error -- it is a client with no mods to declare.
    """
    body = m.HELLO.size
    rows = []

    for i in range(0, (len(payload) - body) // m.MODI.size):
        chunk = payload[body + i * m.MODI.size:body + (i + 1) * m.MODI.size]

        try:
            mod_id, version, enabled, _res = m.MODI.unpack(chunk)
        except Exception:
            break

        rows.append((mod_id.split(b"\0")[0].decode("latin1", "replace"),
                     version.split(b"\0")[0].decode("latin1", "replace"),
                     enabled))

    return rows


def mods_match(declared, required, versions, level):
    """The mod-check policy, exactly as the real host applies it.

    level 0 admits anyone. level 1 compares the SET OF ENABLED IDS (the host ignores
    versions there, because the same mod's version differs between a dev and a
    release build). level 2 compares ids AND versions. A server that names no
    required mods matches everyone, which is what makes leaving the check on
    harmless.
    """
    if int(level) <= 0 or not required:
        return True

    have = set(mod_id for mod_id, _ver, enabled in declared if enabled)

    if have != set(required):
        return False

    if int(level) >= 2:
        for mod_id, version, enabled in declared:
            if enabled and mod_id in versions and versions[mod_id] != version:
                return False

    return True


def normalise_host(value):
    """A registration host, cleaned: no scheme, no path, no port, no trailing dot.

    `advertise_host` is what players DIAL, and it is easy to fill in with the URL you
    have in your head -- "https://play.example.net/" -- which reads correctly and
    lists wrongly: the master stores what it is given, so the address in every
    client's browser becomes the scheme plus the name, and nothing can resolve it.
    Only what cannot be part of a host is removed; nothing is guessed.
    """
    host = (value or "").strip()

    if "://" in host:
        host = host.split("://", 1)[1]

    host = host.split("/", 1)[0].split("?", 1)[0].strip().rstrip(".")

    # a port here is a mistake the operator means well by -- the port has its own key
    if host.count(":") == 1:
        name, _, tail = host.partition(":")

        if tail.isdigit():
            host = name

    return host


def is_private_host(host):
    """Is this an address that only works on the machine that owns it?

    A dedicated server and a master on the SAME box is a good arrangement, and it
    has one trap: point `master` at 127.0.0.1 (the obvious choice, since it is right
    there) and the master honestly records the address it saw -- 127.0.0.1 -- and
    lists THAT to every internet client, none of whom can dial it.
    """
    host = (host or "").strip().lower()

    if host in ("localhost", "::1"):
        return True

    try:
        ip = socket.inet_aton(host)          # IPv4 only
    except OSError:
        return False                          # a name: the operator's business

    a, b = ip[0], ip[1]

    return (a == 0 or a == 127 or a == 10
            or (a == 172 and 16 <= b <= 31)
            or (a == 192 and b == 168))


def master_base(url):
    """The master's base URL, with the defaults filled in.

    A bare host used to become `http://host` and then connect to port 80, where
    nothing is listening -- so a config that looked right registered nowhere, and
    the failure was a connect error in a log nobody was reading. The scheme and the
    port are the master's defaults now; a URL that names either keeps it.
    """
    url = (url or "").strip().rstrip("/")

    if not url:
        return ""

    if "://" not in url:
        url = "http://" + url

    parts = urlsplit(url)
    host = parts.hostname or ""

    if not host:
        return ""

    return "%s://%s:%d" % (parts.scheme or "http", host, parts.port or 8100)


# ---------------------------------------------------------------------- the server
class Dedicated:
    def __init__(self, cfg, log):
        self.cfg = cfg
        self.log = log
        self.lock = threading.Lock()
        self.clients = {}        # conn -> {"id", "name", "addr"}
        self.cars = {}           # playerId -> one MP_CARSTATE_ENTRY
        self.stop = threading.Event()
        self.started = time.time()

        # Reported by the console. The beacon can be turned off from there (a LAN
        # you do not want to appear on) without stopping anything else, so the
        # loop reads this rather than the config value directly.
        self.beacon_on = bool(self.cfg["beacon"])

        # What the master_loop last managed, for 'master' to print. Kept as its own
        # small dict so the loop's local `token` stays local.
        self.master_state = {"base": "", "registered": False, "last": ""}

        self.console = None
        self.started = time.time()

    # -- helpers
    @property
    def max_players(self):
        return int(self.cfg["max_players"])

    @property
    def port(self):
        return int(self.cfg["port"])

    @property
    def city(self):
        return int(self.cfg["city"])

    def player_count(self):
        with self.lock:
            return len(self.clients)

    def free_id(self):
        for i in range(1, self.max_players):
            if all(c["id"] != i for c in self.clients.values()):
                return i
        return None

    def carstate_union(self, frame):
        with self.lock:
            rows = list(self.cars.items())
        hdr = m.CARSTATE.pack(frame & 0xFFFFFFFF, len(rows), 0, 0, 0)
        return hdr + b"".join(e for _, e in rows)

    def roster_bytes(self):
        """MP_ROSTER for the clients connected right now -- who is in the match.

        A client learns its PEERS ONLY from this. mp_session.c's MpHandleRoster
        adopts any player it does not already know and, when
        MP_ROSTER_FLAG_CAR_READY is set, builds that player's car; without a roster
        every client knows just the server's phantom player 0 and itself, which is
        the symptom this server had: "you can see yourself and the host, but not the
        other players".

        The server gets no row of its own. The clients invent their player 0 without
        being told, and naming it here would start changing what that phantom is.

        A player's car comes from the carstate rows the server already keeps (the
        same model/modelCity the peers are driving); 0xFF while we have not seen one
        yet, which is "on foot" -- so a client still loading is listed, named, and
        simply given no car until its first carstate lands.
        """
        with self.lock:
            rows = []
            for info in self.clients.values():
                car = self.cars.get(info["id"])
                model, model_city, palette, ready = 0xFF, 0xFF, 0, 0
                if car is not None and len(car) == m.CARSTATE_ENTRY.size:
                    # the first five fields of the row: pid, flags, palette, model, modelCity
                    _pid, _fl, palette, model, model_city = m.CARSTATE_ENTRY.unpack_from(car, 0)[:5]
                    ready = m.ROSTER_FLAG_CAR_READY
                rows.append((info["id"], info["name"], model, model_city, palette, ready))
        rows.sort(key=lambda r: r[0])

        out = bytearray(m.ROSTER.pack(len(rows), 0, 0, 0))
        for pid, name, model, model_city, palette, ready in rows:
            out += m.ROSTER_ENTRY.pack(
                pid & 0xFF, 0, model & 0xFF, model_city & 0xFF, ready,
                0,                                    # ping: no per-client RTT is measured here yet
                palette & 0xFFFF,
                name.encode("latin1", "replace")[: m.ROSTER_NAME_MAX - 1])
        return bytes(out)

    def _send_roster_if_due(self, conn, last, pid):
        """(socket still alive, new timestamp) -- the roster when its cadence elapses.

        Called from BOTH ways round the receive loop on purpose. Putting it only in
        the timeout branch looked right and was wrong: a client that is DRIVING sends
        a carstate every frame, so its socket never times out and it would never be
        told a peer had joined. Only the joiner saw anyone, which is exactly the
        asymmetry that showed up here -- "added 2 remote player car(s)" on the second
        client and 1 on the first.
        """
        now = time.time()
        if now - last < ROSTER_REFRESH_S:
            return True, last
        try:
            m.send_frame(conn, m.TAG["roster"], self.roster_bytes())
        except OSError:
            return False, now
        except Exception as e:
            # a roster we cannot build must not cost the player their connection
            self.log.debug("roster refresh failed for player %d: %s" % (pid, e))
        return True, now

    def broadcast(self, payload, tag, skip=None):
        with self.lock:
            conns = [c for c in self.clients if c is not skip]
        for c in conns:
            try:
                m.send_frame(c, tag, payload)
            except OSError:
                pass

    def _chat(self, conn, payload, pid, name):
        """Print one chat line to the operator console, then relay it.

        The id inside the frame is the SENDER'S CLAIM, and a claim is all it is:
        the connection is who is talking. It is stamped here BEFORE the relay, so
        the other seats see the real speaker too -- the same rule the C host
        applies on its own chat path.

        Control characters are stripped, because this line goes to a terminal: a
        peer that puts an escape sequence or a newline in a message could
        otherwise forge console rows (or paint the operator's terminal).
        """
        try:
            _claimed, _r0, _r1, _r2, text = m.CHAT.unpack_from(payload, 0)
        except struct.error:
            self.log.debug("player %d '%s': ignoring a malformed chat frame" % (pid, name))
            return

        line = text.split(b"\0")[0].decode("latin1", "replace")
        line = "".join(ch if ch.isprintable() else " " for ch in line).strip()

        if not line:
            return

        self.log.info("[chat] %s: %s" % (name, line))

        out = m.CHAT.pack(pid, 0, 0, 0, line.encode("latin1", "replace")[:95])
        self.broadcast(out, m.TAG["chat"], skip=conn)

    # -- one client
    def serve(self, conn, addr):
        who = "%s:%d" % (addr[0], addr[1])
        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        conn.settimeout(self.cfg["idle_drop_s"] or 1.0)

        try:
            tag, payload = m.recv_frame(conn)
        except (OSError, socket.timeout):
            conn.close()
            return
        if tag != m.TAG["hello"]:
            # DEBUG, not a warning: a bare TCP connect is a health probe, a
            # reachability check (the master runs one every few seconds), or a port
            # scan -- none of which is an error, and warning about them buries the
            # lines that matter.
            self.log.debug("%s: connected without a HELLO (tag %r), dropping" % (who, tag))
            conn.close()
            return
        try:
            # the four tail bytes are (modCount, reserved[3]) -- reading them
            # shifted by one is why every join logged "mods=0"
            _proto, _sdk, build, _sp, name32, modc, _a, _b, _c = m.HELLO.unpack_from(payload, 0)
        except Exception:
            self.log.warning("%s: malformed HELLO, dropping" % who)
            conn.close()
            return

        name = name32.split(b"\0")[0].decode("latin1", "replace") or "Player"

        # The manifest rides after the hello header, modc rows of it.
        mods = parse_manifest(payload)

        # -- the wire has to agree before anything else means anything: a client on
        # a different protocol would misread every frame this server sends it, so it
        # is refused the way the real host refuses it, with the reason readable.
        if _proto != m.PROTO or _sdk != m.SDK:
            self.log.warning("%s: refused, version mismatch (proto %d/%d, sdk %d/%d)"
                             % (who, _proto, m.PROTO, _sdk, m.SDK))
            self._reject(conn, 2, "game/protocol version mismatch")
            return

        # -- and the mod list, if this server enforces one
        matched = mods_match(mods, self.cfg["mods"], self.cfg["mod_versions"],
                             self.cfg["mod_check"])

        if not matched:
            self.log.warning("%s: refused, mod list does not match (declared: %s; "
                             "required: %s)"
                             % (who, ", ".join(sorted(x[0] for x in mods if x[2])) or "none",
                                ", ".join(sorted(self.cfg["mods"]))))
            self._reject(conn, 3, "mod list does not match the server")
            return

        if mods:
            self.log.info("%s declares mods: %s"
                          % (who, ", ".join("%s %s" % (x[0], x[1]) for x in mods if x[2])))

        refused_full = False

        with self.lock:
            pid = self.free_id()
            if pid is None:
                # len(self.clients), not self.player_count(): we are INSIDE the
                # lock, and player_count() takes it too -> a non-reentrant Lock
                # would deadlock this thread with the lock held forever.
                refused_full = True
            else:
                self.clients[conn] = {"id": pid, "name": name, "addr": who}

        if refused_full:
            # The real host answers a full server with a REJECT alone -- a WELCOME
            # here would tell the client it got in. _reject half-closes, which is
            # what lets the reason reach the client at all (TESTING.md, F3).
            self.log.warning("refused %s: server is full (%d/%d)"
                             % (who, self.player_count(), self.max_players))
            self._reject(conn, 1, "server is full")
            return

        self.log.info("player %d '%s' joined from %s (mods=%d, build %#06x, "
                      "modsMatched=%d)"
                      % (pid, name, who, modc, build, matched))

        seed = int(time.time()) & 0xFFFFFFFF

        # The roster goes BEFORE the welcome. mp_test.py records the order as
        # load-bearing: a live joiner has to know who else is in the match before it
        # spawns cars, because the spawn walks the player list.
        try:
            m.send_frame(conn, m.TAG["roster"], self.roster_bytes())
        except Exception as e:
            # A roster we cannot build must NOT cost the player their connection --
            # they simply arrive without a peer list, which is what this server did
            # before it had one at all.
            self.log.warning("could not send the roster to %s: %s" % (who, e))
        sess = m.SESSION.pack(0, self.city, 1, 0, seed,
                              max(2, self.player_count() + 1), 1, 0)
        try:
            # running=1: a dedicated server IS a match in progress. It said 0, and
            # the client takes its "the host has STARTed" branch only when this is
            # set -- so a joiner was never offered the CAR SELECT and went straight
            # into the session in whatever vehicle the module defaulted to. 0 is
            # right for a host still sitting in its lobby; it is never right here.
            m.send_frame(conn, m.TAG["welcome"],
                         self._welcome(pid, running=1 if self.cfg["live"] else 0,
                                       matched=matched, seed=seed))
            m.send_frame(conn, m.TAG["start"], sess)
        except OSError:
            pass

        # Poll fast so we can keep the link fresh from OUR side too: a dedicated
        # server should not depend on the client to prove the link is alive.
        conn.settimeout(1.0)
        silent = 0
        idle_drop = float(self.cfg["idle_drop_s"])
        info = None
        last_roster = 0.0

        # The whole loop is wrapped so the cleanup below ALWAYS runs: a malformed
        # frame (a bad CARSTATE header) must not unwind the thread and strand the
        # socket open with its slot still taken.
        try:
            while not self.stop.is_set():
                try:
                    tag, payload = m.recv_frame(conn)
                except socket.timeout:
                    silent += 1
                    # Send our own keepalive PING and only give the client up after
                    # `idle_drop_s` of real silence. Without this a quiet client (a
                    # lobby, a load) would be dropped by our own timeout.
                    if idle_drop and silent >= idle_drop:
                        self.log.warning("player %d '%s' timed out (%.0fs silent)"
                                         % (pid, name, idle_drop))
                        break
                    try:
                        m.send_frame(conn, m.TAG["ping"],
                                     struct.pack("<I", int(time.time() * 1000) & 0xFFFFFFFF))
                    except OSError:
                        break
                    # The roster, on the host's own cadence (mp_session.c refreshes
                    # it every 120 frames = ~2s). This is how a client learns a peer
                    # joined or left, and how a peer's car appears once its first
                    # carstate has landed.
                    alive, last_roster = self._send_roster_if_due(conn, last_roster, pid)
                    if not alive:
                        break
                    continue
                except Exception:
                    break
                if tag is None:
                    break
                silent = 0

                # the running client's half of the cadence (see the helper): it is
                # never idle, so the timeout branch alone would never reach it
                alive, last_roster = self._send_roster_if_due(conn, last_roster, pid)
                if not alive:
                    break

                if tag == m.TAG["carstate"]:
                    try:
                        with self.lock:
                            _f, count, _a, _b, _c = m.CARSTATE.unpack_from(payload, 0)
                            # bound the loop by what actually arrived: a bogus count
                            # must not spin, and a short frame must not read past it
                            avail = (len(payload) - m.CARSTATE.size) // m.CARSTATE_ENTRY.size
                            for i in range(min(count, max(0, avail))):
                                off = m.CARSTATE.size + i * m.CARSTATE_ENTRY.size
                                e = payload[off:off + m.CARSTATE_ENTRY.size]
                                if len(e) == m.CARSTATE_ENTRY.size:
                                    self.cars[e[0]] = e
                    except Exception:
                        # a malformed frame is this connection's problem, not the
                        # server's: skip it and keep serving
                        self.log.debug("player %d '%s': ignoring a malformed carstate frame"
                                       % (pid, name))
                        continue
                    self.broadcast(self.carstate_union(int(time.time() * 30)),
                                   m.TAG["carstate"], skip=conn)
                elif tag == m.TAG["chat"]:
                    self._chat(conn, payload, pid, name)
                elif tag == m.TAG["ping"]:
                    # Liveness. Echo the tick; with no PONG the client's last-receive
                    # clock stops advancing and it drops itself at its idle timeout.
                    try:
                        m.send_frame(conn, m.TAG["pong"], payload or b"\x00\x00\x00\x00")
                    except OSError:
                        break
                elif tag == m.TAG["leave"]:
                    break
        finally:
            with self.lock:
                info = self.clients.pop(conn, None)
                if info:
                    self.cars.pop(info["id"], None)
            if info:
                self.log.info("player %d '%s' left (%d/%d)"
                              % (info["id"], info["name"], self.player_count(), self.max_players))
            try:
                conn.close()
            except OSError:
                pass

    def _reject(self, conn, reason, text):
        """Refuse, and let the client READ the reason.

        The old path sent the frame and closed the socket at once, so the peer often
        got 0 bytes back -- "a refused client is told nothing" (TESTING.md, F3). The
        real host half-closes, lets the peer drain, then closes; this does the same,
        and every refusal in this file goes through it.
        """
        try:
            m.send_frame(conn, m.TAG["reject"], m.REJECT.pack(reason, 0, 0, 0, text.encode()))
        except OSError:
            pass

        try:
            conn.shutdown(socket.SHUT_WR)     # half-close: our bytes still arrive
        except OSError:
            pass

        try:
            conn.settimeout(1.0)
            while conn.recv(4096):            # drain what the peer is still sending
                pass
        except OSError:
            pass

        try:
            conn.close()
        except OSError:
            pass

    def _welcome(self, pid, running, matched=1, seed=0):
        # 15 values: playerId, maxPlayers, modsEnforced, modsMatched, running,
        # subGame, mpLevel, arena, gamemode, city, timeOfDay, weather, seed,
        # hostCar, rules  (mp_proto.h MP_WELCOME, 18 bytes since v11).
        return m.WELCOME.pack(pid, self.max_players, int(self.cfg["mod_check"]),
                              1 if matched else 0, running, 0, 0, 0,
                              int(self.cfg["gamemode"]), self.city, 1, 0, seed, 0xFF, 0)

    # -- UDP beacon (LAN discovery)
    def beacon_loop(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        host = self.cfg["name"].encode()[:31] + b"\0" * 32
        host = host[:32]
        while not self.stop.is_set():
            if not self.beacon_on:
                self.stop.wait(self.cfg["beacon_ms"] / 1000.0)
                continue

            pkt = m.BEACON.pack(m.UDP_MAGIC, m.PROTO, self.port, host,
                                self.player_count(), self.max_players,
                                int(self.cfg["gamemode"]), 0, self.city,
                                1 if self.cfg["live"] else 0, 0,
                                int(self.cfg["rules"]))
            try:
                # to the DISCOVERY port, never to our own session port: the session port
                # travels INSIDE the beacon, and a browser listens on the fixed one
                s.sendto(pkt, ("255.255.255.255", m.DISCOVERY_PORT))
            except OSError:
                pass
            self.stop.wait(self.cfg["beacon_ms"] / 1000.0)
        s.close()

    # -- master discovery registration
    def master_loop(self):
        base = master_base(self.cfg["master"])
        token = None
        interval = float(self.cfg["master_interval_s"])
        timeout = float(self.cfg["master_timeout_s"])
        backoff = 2.0

        def post(path, body):
            req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, json.loads(r.read().decode())

        while not self.stop.is_set():
            try:
                if token is None:
                    # Where players actually reach us, when that is not where the master
                    # sees us. Both halves matter behind NAT: the address it would record
                    # is private, and a remapping router can hand out a public port that
                    # differs from our session port.
                    adv = {}
                    if self.cfg["advertise_host"]:
                        adv["advertiseHost"] = self.cfg["advertise_host"]
                    if int(self.cfg["advertise_port"]):
                        adv["advertisePort"] = int(self.cfg["advertise_port"])

                    st, body = post("/v1/register", {
                        "name": self.cfg["master_name"] or self.cfg["name"],
                        "port": self.port, "city": self.city,
                        "gamemode": int(self.cfg["gamemode"]),
                        # the same packed byte the beacon carries, so a browser
                        # can name the mode from either source
                        "rules": int(self.cfg["rules"]),
                        "players": self.player_count(), "maxPlayers": self.max_players,
                        "inProgress": 1 if self.cfg["live"] else 0,
                        # whether this server checks a client's mod manifest, so a
                        # browser can warn before anyone tries to join
                        "modsEnforced": int(self.cfg["mod_check"]),
                        "protoVersion": m.PROTO, "sdkVersion": m.SDK,
                        **adv,
                    })
                    if st not in (200, 201):
                        raise OSError("register returned %s" % st)
                    token = body["token"]
                    self.master_state.update(base=base, registered=True,
                                             last="registered at %s"
                                             % time.strftime("%H:%M:%S"))
                    listed = str(body.get("host") or "")

                    # The master lists whatever address it saw us from. Behind a
                    # co-hosted master that is loopback or a private address unless
                    # `master` names something public -- and then the listing is
                    # useless to the internet while looking perfectly healthy here.
                    if listed and not self.cfg["advertise_host"] and is_private_host(listed):
                        why = ("loopback" if listed.startswith("127.") or listed == "localhost"
                               else "a private address")
                        msg = ("listed as %s:%d, which is %s: internet clients cannot "
                               "reach that. Point master at a PUBLIC address (so the "
                               "peer address it records is the right one), or set "
                               "advertise_host to the public name"
                               % (listed, self.port, why))
                        self.log.warning(msg)

                        if self.console is not None:
                            self.console.say("WARNING: " + msg)

                    self.log.info("registered with master %s as %s:%d"
                                  % (base, body.get("host"), body.get("port")))
                    backoff = 2.0
                else:
                    try:
                        post("/v1/heartbeat", {"token": token, "players": self.player_count()})
                    except urllib.error.HTTPError as e:
                        if e.code == 404:      # expired / master restarted -> re-register
                            self.log.warning("master forgot us; re-registering")
                            token = None
                            continue
                        raise
                self.stop.wait(interval)
            except (OSError, ValueError, KeyError) as e:
                self.master_state.update(base=base, registered=False,
                                         last="unreachable: %s" % e)
                self.log.warning("master %s unreachable (%s); retrying in %.0fs"
                                 % (base, e, backoff))
                self.stop.wait(backoff)
                backoff = min(backoff * 2, 60.0)
                token = None

        if token is not None:
            try:
                post("/v1/unregister", {"token": token})
                self.master_state.update(registered=False, last="unregistered")
                self.log.info("unregistered from master")
            except OSError:
                pass

    # -- lifecycle
    # -- console ----------------------------------------------------------
    def console_commands(self):
        """The operator's command table.

        `help` is generated from this by the Console, so a command cannot exist
        without being listed -- and every handler takes the rest of the typed line
        as one string, so quoting is the operator's problem, not a parser's.
        """
        return [
            ("status", "name, port, city, mode, players, uptime, beacon, master", self.cmd_status),
            ("list", "connected players: id, name, address, how long", self.cmd_list),
            ("say", "broadcast a line into every player's console: say <text>", self.cmd_say),
            ("kick", "drop a player, telling them: kick <id>  ('list' shows ids)", self.cmd_kick),
            ("beacon", "LAN beacon: beacon [on|off], or its state", self.cmd_beacon),
            ("master", "discovery-server registration state", self.cmd_master),
            ("stop", "close the server (unregisters from the master)", self.cmd_stop),
            ("help", "list these commands", self.cmd_help),
        ]

    def cmd_help(self, _rest):
        return self.console.help_text() if self.console is not None else ""

    def cmd_status(self, _rest):
        adv = ""
        if self.cfg["advertise_host"] or int(self.cfg["advertise_port"]):
            adv = " (advertised as %s:%d)" % (self.cfg["advertise_host"],
                                              int(self.cfg["advertise_port"]) or self.port)
        return "\n".join([
            "name:      %s" % self.cfg["name"],
            "session:   TCP/%d%s" % (self.port, adv),
            "city:      %d" % self.city,
            "gamemode:  %d (rules 0x%02x)" % (int(self.cfg["gamemode"]), int(self.cfg["rules"])),
            "players:   %d/%d" % (self.player_count(), self.max_players),
            "uptime:    %s" % duration(time.time() - self.started),
            "beacon:    %s" % (("every %dms to UDP/%d"
                                % (int(self.cfg["beacon_ms"]), m.DISCOVERY_PORT))
                               if self.beacon_on else "off"),
            "master:    %s" % (self.master_state["last"] or "not configured"),
        ])

    def cmd_list(self, _rest):
        with self.lock:
            rows = sorted((c["id"], c["name"], c["addr"],
                           duration(time.time() - c.get("since", time.time())))
                          for c in self.clients.values())

        if not rows:
            return "no players connected"

        return "\n".join("%3d  %-20s %-21s %s" % r for r in rows)

    def cmd_say(self, rest):
        if not rest:
            return "say what?  (say <text>)"

        # 0xFF is not a player id, so the client's chat handler finds no name for it
        # and prints the text on its own -- which is what a server line wants. The
        # prefix is there because the text would otherwise read as a player's.
        chat = m.CHAT.pack(0xFF, 0, 0, 0, ("SERVER: " + rest).encode()[:95])
        self.broadcast(chat, m.TAG["chat"])
        self.log.info("console: said %r" % rest)
        return "said to %d player(s)" % self.player_count()

    def cmd_kick(self, rest):
        rest = (rest or "").strip()

        if not rest.isdigit():
            return "kick who?  (kick <id>; 'list' shows the ids)"

        pid = int(rest)

        with self.lock:
            target = [(conn, c) for conn, c in self.clients.items() if c["id"] == pid]

        if not target:
            return "no player %d connected  ('list')" % pid

        conn, c = target[0]

        # The protocol already has this message, and the client handles it properly
        # ("You were kicked by the host" then out to the frontend) -- so send that
        # rather than just dropping the socket and leaving them guessing.
        try:
            m.send_frame(conn, m.TAG["kick"], m.KICK.pack(pid, 0, 0, 0))
        except OSError:
            pass

        self.log.info("console: kicked player %d '%s' (%s)" % (pid, c["name"], c["addr"]))

        try:
            conn.shutdown(socket.SHUT_RDWR)      # wake their blocked recv; cleanup runs
        except OSError:
            pass

        return "kicked player %d '%s'" % (pid, c["name"])

    def cmd_beacon(self, rest):
        arg = (rest or "").strip().lower()

        if arg in ("on", "off"):
            self.beacon_on = (arg == "on")
            self.log.info("console: beacon %s" % arg)
            return "beacon %s" % arg

        if arg:
            return "usage: beacon [on|off]"

        return "beacon is %s (every %dms to UDP/%d)" % (
            "on" if self.beacon_on else "off", int(self.cfg["beacon_ms"]), m.DISCOVERY_PORT)

    def cmd_master(self, rest):
        if (rest or "").strip():
            return "usage: master"

        st = self.master_state

        if not st["base"]:
            return "no discovery server configured  (config 'master' is empty)"

        return "\n".join([
            "discovery:  %s" % st["base"],
            "registered: %s" % ("yes" if st["registered"] else "no"),
            "last:       %s" % (st["last"] or "-"),
            "heartbeat:  every %ss" % self.cfg["master_interval_s"],
        ])

    def cmd_stop(self, _rest):
        self.log.info("console: stop requested")
        self.stop.set()
        return "stopping (unregisters from the master, closes players)..."

    def run(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            srv.bind(("0.0.0.0", self.port))
        except OSError as e:
            self.log.error("could not bind TCP/%d: %s" % (self.port, e))
            return 1
        srv.listen(self.max_players)
        srv.settimeout(0.5)
        self.log.info("listening on TCP/%d (city %d, up to %d players); Ctrl-C to stop"
                      % (self.port, self.city, self.max_players))

        threads = []
        if self.cfg["beacon"]:
            threads.append(threading.Thread(target=self.beacon_loop, daemon=True))
        if self.cfg["master"]:
            threads.append(threading.Thread(target=self.master_loop, daemon=True))
        for t in threads:
            t.start()

        try:
            while not self.stop.is_set():
                try:
                    conn, addr = srv.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                threading.Thread(target=self.serve, args=(conn, addr), daemon=True).start()
        finally:
            self.stop.set()
            srv.close()
            self.log.info("shutting down (uptime %.0fs, %d player(s))"
                          % (time.time() - self.started, self.player_count()))
        for t in threads:
            t.join(timeout=3)
        return 0


# ------------------------------------------------------------------------- main
def parse_args(argv):
    ap = argparse.ArgumentParser(description="headless dedicated server for the JERICHO mp mod")
    ap.add_argument("--config", default=None,
                    help="JSON config file (default: dediserver.json beside this script)")
    ap.add_argument("--port", type=int)
    ap.add_argument("--name")
    ap.add_argument("--city", type=int, help="GameLevel the clients load (0..3)")
    ap.add_argument("--gamemode", type=int)
    ap.add_argument("--max-players", type=int)
    ap.add_argument("--beacon", dest="beacon", action="store_true", default=None)
    ap.add_argument("--no-beacon", dest="beacon", action="store_false")
    ap.add_argument("--beacon-ms", type=int)
    ap.add_argument("--idle-drop", type=float, dest="idle_drop_s", help="seconds of silence that drop a client (0 = never)")
    ap.add_argument("--rules", type=int, help="MP_RULES_PACK'd session rules (1 = cops); names the mode")
    ap.add_argument("--log-level", choices=sorted(LEVELS))
    ap.add_argument("--log-file")
    ap.add_argument("--master", help="master discovery URL, e.g. http://host:8100 ('' disables)")
    ap.add_argument("--master-name", help="name to register with the master (default: --name)")
    ap.add_argument("--advertise-port", metavar="N", type=int,
                    help="the public port players dial, if the router remaps it (0 = this "
                         "server's own --port). Only needed when the forward is not 1:1")
    ap.add_argument("--advertise-host", metavar="HOST",
                    help="the address players reach this server on, if that is not the "
                         "address the master sees (a NAT'd server: your public IP or hostname). "
                         "The listing is unusable without it behind NAT")
    ap.add_argument("--master-interval", type=float, dest="master_interval_s")
    return ap.parse_args(argv)


def build_config(argv):
    args = parse_args(argv)
    path = args.config
    if path is None:
        default_path = os.path.join(HERE, CONFIG_NAME)
        path = default_path if os.path.exists(default_path) else ""
    cfg = load_config(path, DEFAULTS)
    apply_overrides(cfg, args, (
        ("port", "port"), ("name", "name"), ("city", "city"), ("gamemode", "gamemode"),
        ("max_players", "max_players"), ("beacon", "beacon"), ("beacon_ms", "beacon_ms"),
        ("idle_drop_s", "idle_drop_s"), ("rules", "rules"),
        ("log_level", "log_level"), ("log_file", "log_file"),
        ("master", "master"), ("master_name", "master_name"),
        ("advertise_host", "advertise_host"), ("advertise_port", "advertise_port"),
        ("master_interval_s", "master_interval_s"),
    ))
    return cfg, path


def main(argv=None):
    cfg, path = build_config(argv)
    log = Log(cfg.get("log_level", "info"), cfg.get("log_file", ""))
    if path:
        log.debug("config: %s" % path)
    proto_check.assert_ok()

    # -- the values that would otherwise fail quietly
    cleaned = normalise_host(cfg["advertise_host"])

    if cleaned != (cfg["advertise_host"] or ""):
        log.warning("advertise_host %r -> %r (a scheme, path or port cannot be part "
                    "of a host; the port has its own key)"
                    % (cfg["advertise_host"], cleaned))
        cfg["advertise_host"] = cleaned

    # The listing's name comes from the wire's 32-byte field, and the master
    # refuses anything outside 1..31 rather than truncating it itself -- so a long
    # name costs the WHOLE registration (a 400 the log calls "master unreachable"),
    # which reads like a network fault and is not one.
    name = str(cfg["name"] or "")

    if not (1 <= len(name) <= 31):
        trimmed = (name.strip() or "JERICHO Server")[:31]
        log.warning("name %r is %d characters; a listing's name is 1..31 (the wire "
                    "field is 32 bytes with its terminator) and the master rejects the "
                    "whole registration outside that, so it will be listed as %r"
                    % (name, len(name), trimmed))
        cfg["name"] = trimmed

    if int(cfg["max_players"]) > m.MAX_PLAYERS:
        log.warning("max_players %d is above the protocol's %d -- a player's slot is a "
                    "number in MP_MAX_PLAYERS' range, and the client's roster holds %d, "
                    "so the extra seats could not be used; using %d"
                    % (int(cfg["max_players"]), m.MAX_PLAYERS, m.MAX_PLAYERS, m.MAX_PLAYERS))
        cfg["max_players"] = m.MAX_PLAYERS

    if int(cfg["city"]) < 0 or int(cfg["city"]) > 3:
        log.warning("city %d is outside 0..3 (CHICAGO, HAVANA, LAS VEGAS, RIO); the "
                    "clients will load whatever it names, if the level registry has it"
                    % int(cfg["city"]))

    server = Dedicated(cfg, log)

    log.debug("signals: %s" % ", ".join(install_signals(server.stop, log)))

    console = Console(log, server.console_commands())
    server.console = console
    console.start()

    return server.run()


if __name__ == "__main__":
    sys.exit(main())
