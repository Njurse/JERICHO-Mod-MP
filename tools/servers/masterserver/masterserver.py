#!/usr/bin/env python3
"""masterserver.py -- the mp master discovery service (prototype, HTTP + JSON).

A SEPARATE PROJECT: a network service, not game content. See README.md. It has
no dependency on the game or on any other tool beyond an optional sanity check
against the local wire mirror.

It is a registry with a time-to-live:

  * a host POSTs /v1/register and then heartbeats; the master remembers the
    host's PUBLIC address (the TCP peer IP -- so a NAT'd host needs no
    configuration, the same reason the game's direct connect works);
  * a client GETs /v1/servers and gets the live list;
  * a host that stops heartbeating is dropped after its TTL, so a crash
    disappears on its own.

The full API contract lives in README.md; this file is written against it. The
mp binary wire protocol is untouched -- this is purely additive to LAN discovery.

    python masterserver.py
    python masterserver.py --port 8080 --ttl 90 --config masterserver.json
"""
from __future__ import annotations

import argparse
import html
import json
import os
import secrets
import select
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

API_VERSION = 1
DEFAULT_PORT = 8100

# The optional wire-mirror check. If the mp tools are not beside us (a standalone
# copy of this project), the master still runs -- it just cannot warn about a
# host whose protoVersion differs from this checkout's mirror.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The plumbing both servers share: Log, the config merge, signals, and (later) the
# console. It sits one directory up, beside this file's parent -- the same place
# proto_check lives, so one path insert serves both.
from serverkit import (Console, LEVELS, Log, apply_overrides,   # noqa: E402
                       duration, install_signals, load_config)

try:
    import proto_check  # type: ignore
    import mp_test as mp_proto  # type: ignore

    EXPECTED_PROTO = mp_proto.PROTO
except Exception:  # pragma: no cover - standalone copy
    proto_check = None
    EXPECTED_PROTO = None


# ----------------------------------------------------------------------- registry
class Entry:
    __slots__ = (
        "name", "host", "port", "city", "gamemode", "rules", "players", "maxPlayers",
        "inProgress", "modsEnforced", "modHash", "protoVersion", "sdkVersion",
        "gameBuild", "token", "last_seen",
        # reachability: the master can see what a client cannot without trying --
        # an entry that heartbeats while nothing listens on its session port
        "reachable", "probeMs", "probeAt",
    )

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))
        # Keep a persisted last_seen (reload) instead of stamping "now", or every
        # entry reloaded from disk would look freshly heartbeated and never expire.
        saved = kw.get("last_seen")
        self.last_seen = float(saved) if saved else time.time()

        # None = not probed yet, which is not the same as "down"
        if self.probeAt is None:
            self.probeAt = 0.0
        if self.probeMs is None:
            self.probeMs = 0

    def age(self, now=None):
        return (time.time() if now is None else now) - self.last_seen

    def public(self, now=None):
        """The JSON shape a client sees (never the private address)."""
        return {
            "name": self.name,
            "host": self.host,
            "port": self.port,
            "players": self.players,
            "maxPlayers": self.maxPlayers,
            "city": self.city,
            "gamemode": self.gamemode,
            "rules": self.rules,
            "inProgress": self.inProgress,
            "modsEnforced": self.modsEnforced,
            "modHash": self.modHash,
            "protoVersion": self.protoVersion,
            "sdkVersion": self.sdkVersion,
            "gameBuild": self.gameBuild,
            "ageSeconds": round(self.age(now), 1),
            # Whether anything answers on the session port. None until the first
            # probe; a client can use it to avoid offering a dead row.
            "reachable": self.reachable,
            "probeMs": self.probeMs or 0,
        }

    def persist(self):
        # reachability is a LIVE measurement: persisting "down" would greet a
        # restart with a lie until the prober caught up
        return {k: getattr(self, k) for k in self.__slots__
                if k not in ("reachable", "probeMs", "probeAt")}


class Registry:
    def __init__(self, cfg, log):
        self.cfg = cfg
        self.log = log
        self.lock = threading.Lock()
        self.save_lock = threading.Lock()
        self.by_key = {}      # (host, port) -> Entry
        self.by_token = {}    # token -> (host, port)
        self.started = time.time()
        self.max_entries = int(cfg.get("max_entries", 512))
        self.ttl = float(cfg.get("ttl_s", 60))
        self.persist_path = cfg.get("persist_path") or ""
        if self.persist_path:
            self._load()

    # -- persistence (optional)
    def _save(self):
        if not self.persist_path:
            return
        # Snapshot under the registry lock (the sweeper may be mutating by_key),
        # then serialise the write so two savers cannot collide on the temp file.
        with self.lock:
            data = [e.persist() for e in self.by_key.values()]
        blob = json.dumps({"version": API_VERSION, "entries": data})
        with self.save_lock:
            try:
                tmp = "%s.%d.tmp" % (self.persist_path, threading.get_ident())
                with open(tmp, "w", encoding="utf-8") as f:
                    f.write(blob)
                os.replace(tmp, self.persist_path)
            except OSError as e:
                self.log.warning("could not persist registry: %s" % e)

    def _load(self):
        if not os.path.exists(self.persist_path):
            return
        try:
            with open(self.persist_path, "r", encoding="utf-8") as f:
                blob = json.load(f)
            for d in blob.get("entries", []):
                e = Entry(**{k: d.get(k) for k in Entry.__slots__})
                if e.age() <= self.ttl:
                    self.by_key[(e.host, e.port)] = e
                    self.by_token[e.token] = (e.host, e.port)
            self.log.info("reloaded %d persisted entr(ies)" % len(self.by_key))
        except (OSError, ValueError, TypeError) as e:
            self.log.warning("could not reload persisted registry: %s" % e)

    # -- mutations
    def register(self, fields, peer_ip):
        host = (fields.get("advertiseHost") or peer_ip or "").strip()
        port = int(fields["port"])
        key = (host, port)
        with self.lock:
            existing = self.by_key.get(key)
            if existing is None and len(self.by_key) >= self.max_entries:
                return None, "registry_full"
            token = secrets.token_hex(16)
            e = Entry(
                name=fields["name"], host=host, port=port,
                city=fields.get("city", 0), gamemode=fields.get("gamemode", 0),
                rules=fields.get("rules", 0),
                players=fields.get("players", 0),
                maxPlayers=fields.get("maxPlayers", 8),
                inProgress=fields.get("inProgress", 0),
                modsEnforced=fields.get("modsEnforced", 0),
                modHash=fields.get("modHash", 0),
                protoVersion=fields.get("protoVersion", 0),
                sdkVersion=fields.get("sdkVersion", 0),
                gameBuild=fields.get("gameBuild", 0),
                token=token,
            )
            if existing is not None:
                self.by_token.pop(existing.token, None)
            self.by_key[key] = e
            self.by_token[token] = key
        self._save()
        return e, None

    def heartbeat(self, token, fields):
        with self.lock:
            key = self.by_token.get(token)
            if key is None:
                return None
            e = self.by_key.get(key)
            if e is None:
                return None
            for k in ("players", "inProgress", "rules"):
                if k in fields:
                    e.__setattr__(k, int(fields[k]))
            e.last_seen = time.time()
        self._save()
        return e

    def unregister(self, token):
        with self.lock:
            key = self.by_token.pop(token, None)
            if key is not None:
                self.by_key.pop(key, None)
        self._save()
        return key is not None

    def drop(self, what):
        """Remove an entry by token, token prefix, or name. None if nothing matched.

        The console needs this because an entry otherwise lives until its TTL: a
        server that went away without unregistering sits in the listing until it
        expires, and an operator who knows it is gone should be able to say so.
        """
        what = (what or "").strip()

        if not what:
            return None

        low = what.lower()
        hit = None

        with self.lock:
            for token, key in list(self.by_token.items()):
                e = self.by_key.get(key)

                if e is not None and (token == what or token.startswith(what)
                                      or e.name.lower() == low):
                    hit = (token, key, e)
                    break

            if hit is not None:
                token, key, e = hit
                self.by_key.pop(key, None)
                self.by_token.pop(token, None)

        if hit is None:
            return None

        self._save()
        return hit[2]

    def sweep(self):
        now = time.time()
        with self.lock:
            dead = [k for k, e in self.by_key.items() if e.age(now) > self.ttl]
            for k in dead:
                e = self.by_key.pop(k)
                self.by_token.pop(e.token, None)
        if dead:
            self._save()
        return dead

    def listing(self, now):
        with self.lock:
            return [e.public(now) for e in self.by_key.values()]

    def needs_probe(self, stale_s):
        """[(key, host, port)] for entries whose reachability is old or unknown."""
        now = time.time()
        out = []

        with self.lock:
            for key, e in self.by_key.items():
                if not e.probeAt or now - e.probeAt >= stale_s:
                    out.append((key, e.host, e.port))

        return out

    def set_probe(self, key, ok, ms):
        """Record a probe. Returns a transition worth logging, or None."""
        now = time.time()

        with self.lock:
            e = self.by_key.get(key)

            if e is None:                 # it expired or was dropped mid-probe
                return None

            was = e.reachable
            e.reachable = bool(ok)
            # at least 1 when it answered: a loopback connect is well under a
            # millisecond, and 0 here already means "not measured"
            e.probeMs = max(1, int(ms)) if ok else 0
            e.probeAt = now

            if was == e.reachable:
                return None

            return (e.name, e.host, e.port, e.reachable)

    def count(self):
        with self.lock:
            return len(self.by_key)


# ------------------------------------------------------------------ http handler
class Handler(BaseHTTPRequestHandler):
    server_version = "mp-masterserver/%d" % API_VERSION
    registry = None          # set by serve()
    log = None
    cfg = None

    # quieter, structured-ish default logging
    def log_message(self, fmt, *args):
        if self.log:
            self.log.debug("%s %s" % (self.address_string(), fmt % args))

    # -- helpers
    def _json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _error(self, code, err, msg):
        self._json(code, {"error": err, "message": msg, "version": API_VERSION})

    def _read_body(self):
        cap = int(self.cfg.get("max_body_bytes", 4096))
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except (ValueError, TypeError):
            return None, "bad_length"
        # A negative length is not a small body: read(-1) would block until EOF and
        # buffer without bound, so refuse it here.
        if n < 0:
            return None, "bad_length"
        if n > cap:
            return None, "too_large"
        raw = self.rfile.read(n) if n else b""
        if not raw:
            return {}, None
        try:
            obj = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None, "bad_json"
        if not isinstance(obj, dict):
            return None, "bad_json"
        return obj, None

    def _peer_ip(self):
        return self.client_address[0]

    # -- GET
    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/v1/health":
            return self._json(200, {
                "ok": True, "version": API_VERSION,
                "uptimeSeconds": round(time.time() - self.registry.started, 1),
                "servers": self.registry.count(),
            })
        if u.path == "/v1/servers":
            # The one log line that answers "is anything discovering me?" -- a
            # registration missing from the log is a server problem, but a client
            # that never appears HERE is a network one, and the two look identical
            # from the game.
            self.log.info("listing -> %d entr(ies) to %s"
                          % (self.registry.count(), self.client_address[0]))
            return self._servers(q)
        if u.path in ("/", "/index.html"):
            return self._index()
        return self._error(404, "not_found", "no such path")

    def _servers(self, q):
        now = time.time()
        rows = self.registry.listing(now)
        # Everything is listed by default -- a live match must not be hidden from a
        # client that did not ask for anything special. The filters are opt-in
        # EXCLUSIONS: ?exclude_full=1 / ?exclude_inprogress=1.
        if _flag(q, "exclude_full"):
            rows = [r for r in rows if r["players"] < r["maxPlayers"]]
        if _flag(q, "exclude_inprogress"):
            rows = [r for r in rows if not r["inProgress"]]
        rows.sort(key=lambda r: r["ageSeconds"])
        if "limit" in q:
            try:
                rows = rows[: max(0, int(q["limit"][0]))]
            except ValueError:
                pass
        self._json(200, {
            "version": API_VERSION,
            "count": len(rows),
            "generatedAt": round(now, 3),
            "servers": rows,
        })

    def _index(self):
        now = time.time()
        rows = self.registry.listing(now)
        parts = [
            "<!doctype html><meta charset=utf-8>",
            "<title>JERICHO mp servers</title>",
            "<h1>JERICHO mp servers</h1>",
            "<p>%d live. This page is a convenience; the API is "
            "<a href=\"/v1/servers\">/v1/servers</a>.</p>" % len(rows),
            "<table border=1 cellpadding=4><tr><th>name<th>address<th>players"
            "<th>city<th>mode<th>rules<th>live<th>last seen</tr>",
        ]
        for r in rows:
            parts.append(
                "<tr><td>%s<td>%s:%d<td>%d/%d<td>%d<td>%d<td>%d<td>%s<td>%.0fs ago</tr>"
                % (html.escape(str(r["name"])), html.escape(str(r["host"])), r["port"],
                   r["players"], r["maxPlayers"], r["city"], r["gamemode"],
                   r.get("rules") or 0,
                   "yes" if r["inProgress"] else "no", r["ageSeconds"])
            )
        parts.append("</table>")
        if self.cfg.get("public_url"):
            parts.append("<p>Public URL: %s</p>" % html.escape(self.cfg["public_url"]))
        body = "".join(parts).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # -- POST
    def do_POST(self):
        u = urlparse(self.path)
        body, err = self._read_body()
        if err == "too_large":
            return self._error(413, err, "request body too large")
        if err is not None:
            return self._error(400, err, "could not parse the JSON body")
        if u.path == "/v1/register":
            return self._register(body)
        if u.path == "/v1/heartbeat":
            return self._heartbeat(body)
        if u.path == "/v1/unregister":
            return self._unregister(body)
        return self._error(404, "not_found", "no such path")

    def _register(self, body):
        name = str(body.get("name", "")).strip()
        try:
            port = int(body["port"])
        except (KeyError, TypeError, ValueError):
            return self._error(400, "bad_port", "port is required and must be an integer")
        if not (1 <= len(name) <= 31):
            return self._error(400, "bad_name", "name must be 1..31 characters")
        if not (1 <= port <= 65535):
            return self._error(400, "bad_port", "port must be 1..65535")

        # The port PLAYERS should dial, when NAT rewrites it. A host whose router maps
        # public 30000 -> local 1318 registers port=1318 and advertisePort=30000, and
        # the listing has to name the one that works. Everything downstream (the key,
        # the listing, the log) then treats the advertised port as the entry's port.
        try:
            adv = int(body.get("advertisePort") or 0)
        except (TypeError, ValueError):
            return self._error(400, "bad_advertise_port", "advertisePort must be an integer")
        if adv:
            if not (1 <= adv <= 65535):
                return self._error(400, "bad_advertise_port", "advertisePort must be 1..65535")
            port = adv

        fields = {
            "name": name, "port": port,
            "city": _clamp(body.get("city", 0), 0, 3),
            "gamemode": _clamp(body.get("gamemode", 0), 0, 255),
            "rules": _clamp(body.get("rules", 0), 0, 65535),
            "players": _clamp(body.get("players", 0), 0, 255),
            "maxPlayers": _clamp(body.get("maxPlayers", 8), 1, 255),
            "inProgress": 1 if body.get("inProgress") else 0,
            "modsEnforced": _clamp(body.get("modsEnforced", 0), 0, 255),
            "modHash": _clamp(body.get("modHash", 0), 0, 65535),
            "protoVersion": _clamp(body.get("protoVersion", 0), 0, 65535),
            "sdkVersion": _clamp(body.get("sdkVersion", 0), 0, 65535),
            "gameBuild": _clamp(body.get("gameBuild", 0), 0, 65535),
        }
        if body.get("advertiseHost"):
            fields["advertiseHost"] = str(body["advertiseHost"])[:255]

        if EXPECTED_PROTO is not None and fields["protoVersion"] not in (0, EXPECTED_PROTO):
            self.log.warning("register from %s advertises protoVersion %s (this mirror is %s)"
                             % (self._peer_ip(), fields["protoVersion"], EXPECTED_PROTO))

        entry, why = self.registry.register(fields, self._peer_ip())
        if entry is None:
            return self._error(503, why, "the registry is full; try again shortly")
        self.log.info("register %s:%d '%s' from %s (%d/%d, city %d)"
                      % (entry.host, entry.port, entry.name, self._peer_ip(),
                         entry.players, entry.maxPlayers, entry.city))
        return self._json(201, {
            "ok": True, "token": entry.token, "host": entry.host, "port": entry.port,
            "ttl": self.registry.ttl, "version": API_VERSION,
        })

    def _heartbeat(self, body):
        token = str(body.get("token", ""))
        if not token:
            return self._error(400, "bad_token", "token is required")
        e = self.registry.heartbeat(token, body)
        if e is None:
            return self._error(404, "unknown_token",
                               "unknown or expired token -- register again")
        return self._json(200, {
            "ok": True, "ttl": self.registry.ttl,
            "expiresIn": round(self.registry.ttl - e.age(), 1),
            "version": API_VERSION,
        })

    def _unregister(self, body):
        token = str(body.get("token", ""))
        self.registry.unregister(token)
        return self._json(200, {"ok": True})


def _clamp(v, lo, hi):
    try:
        return max(lo, min(hi, int(v)))
    except (TypeError, ValueError):
        return lo


def _flag(query, name):
    """A boolean query flag: present and not an explicit false-ish value.

    `?exclude_full=0` must NOT exclude -- a bare truthiness test on the parsed
    value would treat the non-empty list ['0'] as true.
    """
    v = query.get(name)
    return bool(v) and v[0].strip().lower() not in ("", "0", "false", "no", "off")


# ---------------------------------------------------------------------- config
DEFAULTS = {
    "bind": "0.0.0.0",
    "port": DEFAULT_PORT,
    "ttl_s": 60,
    "sweep_s": 5,
    "max_entries": 512,
    "max_body_bytes": 4096,
    "persist_path": "",
    "log_level": "info",
    "log_file": "",
    "public_url": "",
    "probe_s": 5,           # how often to check that a listed server ANSWERS
}


def console_commands(registry, stop, log):
    """The master's command table.

    Plain closures rather than methods: the master is a set of functions, and
    taking its dependencies as arguments keeps the table buildable in a test with
    no bound socket and no server running.
    """
    def cmd_help(_rest):
        return holder[0].help_text() if holder[0] is not None else ""

    def cmd_status(_rest):
        return "\n".join([
            "listening:  %s:%s" % (registry.cfg["bind"], registry.cfg["port"]),
            "entries:    %d/%d" % (registry.count(), registry.max_entries),
            "ttl:        %ss, swept every %ss" % (registry.ttl, registry.cfg["sweep_s"]),
            "uptime:     %s" % duration(time.time() - registry.started),
            "persist:    %s" % (registry.persist_path or "off (in memory only)"),
            "public url: %s" % (registry.cfg.get("public_url") or "-"),
        ])

    def cmd_list(_rest):
        with registry.lock:
            rows = []
            for token, key in registry.by_token.items():
                e = registry.by_key.get(key)

                if e is not None:
                    rows.append((e.name, e.host, e.port, e.players, e.maxPlayers,
                                 e.age(), e.reachable, token[:8]))

        if not rows:
            return "no servers listed"

        rows.sort()
        out = ["%-24s %-21s %5s  %8s  %-4s  %s"
               % ("NAME", "ADDRESS", "PLRS", "AGE", "REACH", "TOKEN")]
        for name, host, port, players, max_players, age, reachable, token in rows:
            # None = not probed yet, which is not "down"
            reach = "?" if reachable is None else ("up" if reachable else "DOWN")
            out.append("%-24s %-21s %2d/%-2d  %8s  %-4s  %s"
                       % (name[:24], "%s:%d" % (host, port), players, max_players,
                          duration(age), reach, token))
        return "\n".join(out)

    def cmd_drop(rest):
        e = registry.drop(rest)

        if e is None:
            return "nothing matched %r  ('list' shows names and tokens)" % (rest or "")

        log.info("console: dropped %r at %s:%d" % (e.name, e.host, e.port))
        return "dropped %r at %s:%d" % (e.name, e.host, e.port)

    def cmd_persist(_rest):
        if not registry.persist_path:
            return "no persist path configured (nothing to write)"
        registry._save()
        return "saved %d entr(ies) to %s" % (registry.count(), registry.persist_path)

    def cmd_stop(_rest):
        log.info("console: stop requested")
        # Only the flag: the watcher thread in serve() calls httpd.shutdown(), which
        # BLOCKS until serve_forever returns -- doing it here would race the process
        # exiting before this reply could be printed (it did, once).
        stop.set()
        return "stopping..."

    holder = [None]

    return holder, [
        ("status", "listen address, entries, ttl, uptime, persistence", cmd_status),
        ("list", "the listed servers: name, address, players, age, token", cmd_list),
        ("drop", "remove a listing by token or name: drop <token|name>", cmd_drop),
        ("persist", "write the registry to its file now", cmd_persist),
        ("stop", "close the master", cmd_stop),
        ("help", "list these commands", cmd_help),
    ]


def sweeper(registry, stop, log):
    interval = max(1.0, float(registry.cfg.get("sweep_s", 5)))
    while not stop.wait(interval):
        dead = registry.sweep()
        for (host, port) in dead:
            log.info("expired %s:%d (no heartbeat within %ss)"
                     % (host, port, registry.ttl))


def prober(registry, stop, log):
    """Mark each entry reachable or not, one non-blocking connect at a time.

    This is the one thing only the master can see. An entry heartbeats happily
    while nothing listens on its session port -- a firewall, a container whose port
    is not published, a server that moved -- and from every other vantage point the
    listing looks fine. Saying so in the listing, and once in the log when it
    changes, turns "my server is listed but nobody can join it" into one line on
    the VPS instead of a session of guessing at someone else's client.

    Non-blocking, one connect at a time, so a dead address costs a two-second
    timeout in this thread and nothing anywhere else.
    """
    stale_s = max(2.0, float(registry.cfg.get("probe_s", 5)))
    timeout_s = 2.0
    pending = []
    sock = None
    key = None
    started = 0.0

    while not stop.is_set():
        if sock is None:
            if not pending:
                pending = registry.needs_probe(stale_s)

                if not pending:
                    stop.wait(1.0)
                    continue

            key, host, port = pending.pop(0)

            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.setblocking(False)
                sock.connect_ex((host, int(port)))
                started = time.time()
            except OSError as e:
                if sock is not None:
                    sock.close()
                sock = None
                _note(registry, log, key, False, 0, "cannot even try: %s" % e)

            continue

        # one connect in flight: wait for it, briefly
        try:
            _r, w, _x = select.select([], [sock], [], 0.05)
        except OSError:
            w = []

        if w:
            try:
                err = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            except OSError:
                err = 1

            sock.close()
            sock = None

            if err == 0:
                _note(registry, log, key, True, (time.time() - started) * 1000.0, None)
            else:
                _note(registry, log, key, False, 0, "connect failed (errno %d)" % err)

            continue

        if time.time() - started > timeout_s:
            sock.close()
            sock = None
            _note(registry, log, key, False, 0, "no answer in %.0fs" % timeout_s)


def _note(registry, log, key, ok, ms, why):
    """Record a probe and say so when the answer CHANGED (never every sweep)."""
    change = registry.set_probe(key, ok, ms)

    if change is None:
        return

    name, host, port, reachable = change

    if reachable:
        log.info("reachable again: %r at %s:%d (%d ms)"
                 % (name, host, port, max(1, int(ms))))
    else:
        log.warning("UNREACHABLE: %r at %s:%d -- listed, but nothing answers%s"
                    % (name, host, port, (": " + why) if why else ""))


def serve(cfg, log):
    registry = Registry(cfg, log)
    Handler.registry = registry
    Handler.log = log
    Handler.cfg = cfg

    httpd = ThreadingHTTPServer((cfg["bind"], int(cfg["port"])), Handler)
    httpd.daemon_threads = True
    log.info("master listening on http://%s:%d (api v%d, ttl %ss, %d entr(ies) reloaded)"
             % (cfg["bind"], int(cfg["port"]), API_VERSION, cfg["ttl_s"], registry.count()))

    stop = threading.Event()
    install_signals(stop, log)

    # httpd.shutdown() blocks until serve_forever returns, so it cannot be called
    # from this thread: a watcher waits for the stop event and asks the server to
    # stop from outside. Without it a signal set the flag and nothing else.
    threading.Thread(target=lambda: (stop.wait(), httpd.shutdown()),
                     daemon=True).start()

    threading.Thread(target=sweeper, args=(registry, stop, log), daemon=True).start()
    threading.Thread(target=prober, args=(registry, stop, log), daemon=True).start()

    holder, commands = console_commands(registry, stop, log)
    console = Console(log, commands)
    holder[0] = console
    console.start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log.info("shutting down")
    finally:
        stop.set()
        registry._save()
        httpd.server_close()
    return registry


def main(argv=None):
    ap = argparse.ArgumentParser(description="the JERICHO mp master discovery service")
    ap.add_argument("--config", default="", help="JSON config file (CLI flags override it)")
    ap.add_argument("--bind", default=None)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--ttl", type=float, default=None, help="entry lifetime in seconds")
    ap.add_argument("--sweep", type=float, default=None, help="expiry sweep interval in seconds")
    ap.add_argument("--max-entries", type=int, default=None)
    ap.add_argument("--persist", default=None, help="JSON file to persist the registry")
    ap.add_argument("--log-level", default=None, choices=sorted(LEVELS))
    ap.add_argument("--log-file", default=None)
    ap.add_argument("--public-url", default=None)
    ap.add_argument("--probe", type=float, default=None,
                    help="seconds between reachability probes of a listing (0 disables)")
    args = ap.parse_args(argv)

    try:
        cfg = load_config(args.config, DEFAULTS)
    except (OSError, ValueError) as e:
        sys.stderr.write("masterserver: could not read config %r: %s\n" % (args.config, e))
        return 1

    apply_overrides(cfg, args, (
        ("bind", "bind"), ("port", "port"), ("ttl", "ttl_s"), ("sweep", "sweep_s"),
        ("max_entries", "max_entries"), ("persist", "persist_path"),
        ("log_level", "log_level"), ("log_file", "log_file"), ("public_url", "public_url"),
        ("probe", "probe_s"),
    ))

    log = Log(cfg.get("log_level", "info"), cfg.get("log_file", ""))
    if proto_check is not None:
        proto_check.assert_ok()   # a drifting local wire mirror fails now, not later

    try:
        serve(cfg, log)
    except OSError as e:
        log.error("could not bind %s:%s: %s" % (cfg["bind"], cfg["port"], e))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
