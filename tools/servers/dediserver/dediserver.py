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
import signal
import socket
import struct
import sys
import threading
from urllib.parse import urlsplit
import time
import urllib.error
import urllib.request

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
    "master_interval_s": 20.0, # heartbeat cadence; keep well under the master's ttl
    "master_timeout_s": 5.0,
}

# How often the roster is re-broadcast. mp_session.c sends it every 120 frames,
# which is ~2s at 60 fps -- the same cadence a real host uses, so a client sees
# peers appear and leave on the timing it was written against.
ROSTER_REFRESH_S = 2.0

CONFIG_NAME = "dediserver.json"


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


def load_config(path):
    cfg = dict(DEFAULTS)
    if path and os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            cfg.update(json.load(f))
    return cfg


# --------------------------------------------------------------------------- log
LEVELS = {"debug": 10, "info": 20, "warning": 30, "error": 40}


class Log:
    """stdout, and optionally a file. Join/leave/error lines are the ones that
    matter when a client "never connected" vs "connected and was dropped"."""

    def __init__(self, level="info", path=""):
        self.min = LEVELS.get(level, 20)
        self.lock = threading.Lock()
        self.fh = None
        if path:
            self.fh = open(path, "a", encoding="utf-8", buffering=1)

    def _emit(self, lvl, msg):
        if LEVELS[lvl] < self.min:
            return
        line = "%s [%s] %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), lvl.upper(), msg)
        with self.lock:
            print(line, flush=True)
            if self.fh:
                self.fh.write(line + "\n")

    def debug(self, msg):
        self._emit("debug", msg)

    def info(self, msg):
        self._emit("info", msg)

    def warning(self, msg):
        self._emit("warning", msg)

    def error(self, msg):
        self._emit("error", msg)


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
            self.log.warning("%s: bad handshake (tag %r), dropping" % (who, tag))
            conn.close()
            return
        try:
            _proto, _sdk, build, _sp, name32, _mesh, modc, _a, _b = m.HELLO.unpack_from(payload, 0)
        except Exception:
            self.log.warning("%s: malformed HELLO, dropping" % who)
            conn.close()
            return

        name = name32.split(b"\0")[0].decode("latin1", "replace") or "Player"

        with self.lock:
            pid = self.free_id()
            if pid is None:
                try:
                    # The real host answers a full server with a REJECT alone --
                    # a WELCOME here would tell the client it got in.
                    m.send_frame(conn, m.TAG["reject"], m.REJECT.pack(1, 0, 0, 0, b"server is full"))
                except OSError:
                    pass
                conn.close()
                # len(self.clients), not self.player_count(): we are INSIDE the
                # lock, and player_count() takes it too -> a non-reentrant Lock
                # would deadlock this thread with the lock held forever.
                self.log.warning("refused %s: server is full (%d/%d)"
                                 % (who, len(self.clients), self.max_players))
                return
            self.clients[conn] = {"id": pid, "name": name, "addr": who}

        self.log.info("player %d '%s' joined from %s (mods=%d, build %#06x)"
                      % (pid, name, who, modc, build))

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
            m.send_frame(conn, m.TAG["welcome"], self._welcome(pid, running=0, seed=seed))
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
                    self.broadcast(payload, m.TAG["chat"], skip=conn)
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

    def _welcome(self, pid, running, seed=0):
        # 15 values: playerId, maxPlayers, modsEnforced, modsMatched, running,
        # subGame, mpLevel, arena, gamemode, city, timeOfDay, weather, seed,
        # hostCar, rules  (mp_proto.h MP_WELCOME, 18 bytes since v11).
        return m.WELCOME.pack(pid, self.max_players, 0, 1, running, 0, 0, 0,
                              int(self.cfg["gamemode"]), self.city, 1, 0, seed, 0xFF, 0)

    # -- UDP beacon (LAN discovery)
    def beacon_loop(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        host = self.cfg["name"].encode()[:31] + b"\0" * 32
        host = host[:32]
        while not self.stop.is_set():
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
                        "protoVersion": m.PROTO, "sdkVersion": m.SDK,
                        **adv,
                    })
                    if st not in (200, 201):
                        raise OSError("register returned %s" % st)
                    token = body["token"]
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
                self.log.warning("master %s unreachable (%s); retrying in %.0fs"
                                 % (base, e, backoff))
                self.stop.wait(backoff)
                backoff = min(backoff * 2, 60.0)
                token = None

        if token is not None:
            try:
                post("/v1/unregister", {"token": token})
                self.log.info("unregistered from master")
            except OSError:
                pass

    # -- lifecycle
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
    cfg = load_config(path)
    for flag, key in (
        ("port", "port"), ("name", "name"), ("city", "city"), ("gamemode", "gamemode"),
        ("max_players", "max_players"), ("beacon", "beacon"), ("beacon_ms", "beacon_ms"),
        ("idle_drop_s", "idle_drop_s"), ("rules", "rules"),
        ("log_level", "log_level"), ("log_file", "log_file"),
        ("master", "master"), ("master_name", "master_name"),
        ("advertise_host", "advertise_host"), ("advertise_port", "advertise_port"),
        ("master_interval_s", "master_interval_s"),
    ):
        v = getattr(args, flag)
        if v is not None:
            cfg[key] = v
    return cfg, path


def main(argv=None):
    cfg, path = build_config(argv)
    log = Log(cfg.get("log_level", "info"), cfg.get("log_file", ""))
    if path:
        log.debug("config: %s" % path)
    proto_check.assert_ok()

    server = Dedicated(cfg, log)

    def on_signal(signum, _frame):
        log.info("signal %d received" % signum)
        server.stop.set()

    for name in ("SIGINT", "SIGTERM", "SIGBREAK"):
        sig = getattr(signal, name, None)
        if sig is None:
            continue
        try:
            signal.signal(sig, on_signal)
        except (ValueError, OSError):
            pass

    return server.run()


if __name__ == "__main__":
    sys.exit(main())
