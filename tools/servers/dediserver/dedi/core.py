"""dedi.core -- the dedicated server itself: config, session loop, beacon, master
registration and the operator console. dediserver.py is the entry point.
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

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # .../servers/dediserver
sys.path.insert(0, os.path.dirname(HERE))                            # .../servers (serverkit)

from serverkit import (LEVELS, Console, Log, apply_overrides,   # noqa: E402
                       duration, install_signals, load_config)

TOOLS = os.path.dirname(os.path.dirname(HERE))      # .../mp/tools
SERVERS = os.path.dirname(HERE)                      # .../mp/tools/servers
sys.path.insert(0, TOOLS)
sys.path.insert(0, SERVERS)
import mp_test as m            # noqa: E402  the single Python source of truth
from dedi import relay   # noqa: E402  hit / colour / channel routing
from dedi.plugins import CONSOLE, Player, PluginManager, tick_loop   # noqa: E402
import proto_check            # noqa: E402  the wire-mirror guard


# ------------------------------------------------------------------------ config
DEFAULTS = {
    "port": 1318,
    "name": "Dedicated",
    "city": 0,                 # GameLevel the clients should load (0..3)
    "gamemode": 0,
    "max_players": 16,         # a client takes a slot 1..max_players-1 (0 is the server);
                               # 16 is the protocol's cap (MP_MAX_PLAYERS)
    "motd": "",                # said to each joiner as a SERVER chat line ("" = nothing).
                               # One line: the wire field holds 95 characters.
    "motd_color": "#7FE8FF",   # that line's colour, "#RRGGBB". It rides in the chat
                               # frame's reserved bytes, so an older client just
                               # prints the default colour instead of failing.
    "beacon": True,            # advertise on UDP so the LAN browser lists us
    "beacon_ms": 1000,
    "idle_drop_s": 30.0,       # real silence before a client is dropped (0 = never)
    "live": True,              # advertise as a live match (the level is always up)
    # Session rules, readable (packed for the wire with MP_RULES_PACK's layout,
    # mp_proto.h). Defaults match an in-game host (mp_config.c): cops on, difficulty 1.
    "cops": True,              # police cars spawn at all
    "cop_difficulty": 1,       # 0..2 (MP_COP_DIFF_MAX)
    "cop_immunity": False,     # cops exist but never come after anyone
    "invulnerable": False,     # cars take no damage
    # Plugins (dedi/plugins.py; README "Plugins"). null = every plugin found, minus
    # plugins_disabled and any marked ENABLED_BY_DEFAULT = False; a list = exactly
    # those, in order. plugins_dir is relative to this server's folder.
    # The session's time of day and weather, by name (the engine's own lists:
    # FEmain.c TimeOfDayNames / WeatherNames). Read by a client as it loads.
    "time_of_day": "day",      # dawn | day | dusk | night
    "weather": "clear",        # clear | rain | wet
    "announce_joins": True,    # announce plugin: "X joined/left" as server chat
    "plugins": None,
    "plugins_disabled": [],
    "plugins_dir": "plugins",
    # admin plugin: player ids (from !whoami) that are admins on join, and an
    # optional shared password for !login ("" = no password logins).
    "admins": [],
    "admin_password": "",
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


def _chat_chunks(text, limit=95):
    """Split a server line into MP_CHAT-sized pieces (95 bytes + NUL), at spaces
    where possible and never inside a UTF-8 sequence. A line longer than one frame
    used to be cut at byte 95; now it goes out as several chat lines, which the
    client stacks and wraps -- same frame, same protocol."""
    data = text.encode()
    out = []
    while data:
        if len(data) <= limit:
            out.append(data)
            break
        cut = data.rfind(b" ", limit // 2, limit + 1)
        if cut <= 0:
            cut = limit
            while cut > 0 and (data[cut] & 0xC0) == 0x80:   # not mid-character
                cut -= 1
        out.append(data[:cut].rstrip())
        data = data[cut:].lstrip(b" ")
    return out


def _ink_bytes(value):
    """A config colour ("#RRGGBB", or "RRGGBB") as the three bytes the chat frame carries.

    Anything unparsable becomes the near-white a chat line uses by default: a typo in
    a config file should cost a colour, not the motd. Black is never a fallback --
    it would be invisible against the console.
    """
    text = (value or "").strip().lstrip("#")

    try:
        if len(text) >= 6:
            v = int(text[:6], 16)
            return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
    except ValueError:
        pass

    return (240, 240, 240)


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
def _ms32():
    """A millisecond clock in the PING tick's 32 bits (the host uses its own clock)."""
    return int(time.monotonic() * 1000) & 0xFFFFFFFF


class Dedicated:
    def __init__(self, cfg, log):
        self.cfg = cfg
        if "rules" not in cfg:
            resolve_rules(cfg, set(cfg))     # built from readable keys (tests, embedders)
        self.log = log
        self.lock = threading.Lock()
        self.clients = {}        # conn -> {"id", "name", "addr"}
        self.cars = {}           # playerId -> one MP_CARSTATE_ENTRY
        self.colors = {}         # playerId -> its MP_COLOR_ENTRY (8 bytes), the owner's word
        self.stop = threading.Event()
        self.started = time.time()

        # Reported by the console. The beacon can be turned off from there (a LAN
        # you do not want to appear on) without stopping anything else, so the
        # loop reads this rather than the config value directly.
        self.beacon_on = bool(self.cfg["beacon"])

        # What the master_loop last managed, for 'master' to print. Kept as its own
        # small dict so the loop's local `token` stays local.
        self.master_state = {"base": "", "registered": False, "last": ""}

        # Plugins. `home` is where plugins_dir is resolved from; readvertise_evt asks
        # the master loop to re-register now (rules changed) instead of next beat.
        self.home = HERE
        self.config_path = cfg.get("_config_path") or ""
        self.readvertise_evt = threading.Event()
        self.plugins = PluginManager(self)
        self.plugins.load()

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

    # -- the plugin-facing surface (dedi/plugins.py ServerAPI calls these)
    def player_views(self):
        with self.lock:
            rows = list(self.clients.items())
        return sorted((Player(c, i) for c, i in rows), key=lambda p: p.id)

    def send_to(self, conn, tag, payload):
        m.send_frame(conn, tag, payload)

    def send_server_chat(self, text, color=None, conn=None):
        """Chat line(s) in the server's own voice (player id 0xFF), to one
        connection or everyone. A colour rides in the frame's reserved bytes, as
        the motd's does."""
        ink = _ink_bytes(color) if color else (0, 0, 0)
        for line in str(text).split("\n"):
            for piece in _chat_chunks(line) or [b" "]:
                frame = m.CHAT.pack(0xFF, ink[0], ink[1], ink[2], piece)
                if conn is None:
                    self.broadcast(frame, m.TAG["chat"])
                else:
                    try:
                        self.send_to(conn, m.TAG["chat"], frame)
                    except OSError:
                        pass

    def set_rules(self, **rules):
        for k, v in rules.items():
            if k not in RULE_KEYS:
                raise KeyError("unknown rule %r" % k)
            self.cfg[k] = v
        resolve_rules(self.cfg, set(RULE_KEYS))
        self.log.info("rules now: %s (from the next level load)" % rules_text(int(self.cfg["rules"])))
        self.readvertise()
        return rules_text(int(self.cfg["rules"]))

    def readvertise(self):
        self.readvertise_evt.set()

    def set_session(self, time_of_day=None, weather=None):
        """Time of day / weather by name or number; from the next level load."""
        if time_of_day is not None:
            i = _named_index(time_of_day, TIMES_OF_DAY, -1)
            if i < 0:
                raise ValueError("time of day is one of %s" % "/".join(TIMES_OF_DAY))
            self.cfg["time_of_day"] = TIMES_OF_DAY[i]
        if weather is not None:
            i = _named_index(weather, WEATHERS, -1)
            if i < 0:
                raise ValueError("weather is one of %s" % "/".join(WEATHERS))
            self.cfg["weather"] = WEATHERS[i]
        self.log.info("session now: %s, %s (from the next level load)"
                      % (self.cfg["time_of_day"], self.cfg["weather"]))
        self.readvertise()
        return "%s, %s" % (self.cfg["time_of_day"], self.cfg["weather"])

    def save_config_keys(self, keys):
        """Write these cfg keys back into the config file, keeping every other key
        as the operator wrote it. Atomic (temp file + replace). Returns the path."""
        path = self.config_path or os.path.join(HERE, CONFIG_NAME)
        data = {}
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        for k in keys:
            data[k] = self.cfg.get(k)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
            f.write("\n")
        os.replace(tmp, path)
        self.log.info("saved %s to %s" % (", ".join(keys), path))
        return path

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
                rows.append((info["id"], info["name"], model, model_city, palette, ready,
                             info.get("ping", 0)))
        rows.sort(key=lambda r: r[0])

        out = bytearray(m.ROSTER.pack(len(rows), 0, 0, 0))
        for pid, name, model, model_city, palette, ready, ping in rows:
            out += m.ROSTER_ENTRY.pack(
                pid & 0xFF, 0, model & 0xFF, model_city & 0xFF, ready,
                min(int(ping), 0xFFFF),               # ping: our PING -> their PONG round trip
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
            # ...and the colour table on the same cadence, as the host's
            # MpSendColors(1) every 120 frames: how a late joiner learns everyone
            table = relay.color_table(self)
            if table:
                m.send_frame(conn, m.TAG["color"], table)
            # ...and a PING, as the host's keepalive does: the client echoes the tick
            # in a PONG, and that round trip is the roster's `ping` (MpPingForPlayer)
            m.send_frame(conn, m.TAG["ping"], struct.pack("<I", _ms32()))
        except OSError:
            return False, now
        except Exception as e:
            # a roster we cannot build must not cost the player their connection
            self.log.debug("roster refresh failed for player %d: %s" % (pid, e))
        return True, now

    def note_pong(self, conn, payload):
        """A client's PONG to our PING: the round trip in ms, kept for the roster.
        Returns it, or None for a frame that is not an answer to us."""
        if len(payload) < 4:
            return None
        rtt = (_ms32() - struct.unpack_from("<I", payload, 0)[0]) & 0xFFFFFFFF
        if rtt > 60000:
            return None
        with self.lock:
            info = self.clients.get(conn)
            if info is not None:
                info["ping"] = rtt
        return rtt

    def broadcast(self, payload, tag, skip=None):
        with self.lock:
            conns = [c for c in self.clients if c is not skip]
        for c in conns:
            try:
                m.send_frame(c, tag, payload)
            except OSError:
                pass

    def relay_peer_state(self, conn, tag, payload, pid):
        """Forward an owner -> peers state frame, verbatim, to everyone else.

        On-foot poses (JPPD) and replicated traffic/police (JPTF) are sent by the
        machine that owns them to the HOST, and the in-game host relays them as-is
        to every other connection (mp_session.c MpHandlePedState / MpHandleTraffic
        -> MpHostRelay). This server dropped both, so on a dedicated server nobody
        saw a player on foot or another machine's traffic. The server keeps no copy:
        neither has a late-join snapshot in the game either.

        A pose may only describe its sender: a JPPD row naming another player id
        is a spoof (or a confused client) and the frame is dropped. Returns True
        when relayed."""
        if tag == m.TAG["ped"]:
            try:
                count = m.PEDSTATE.unpack_from(payload, 0)[0]
                rows = [payload[m.PEDSTATE.size + i * m.PEDSTATE_ENTRY.size]
                        for i in range(count)]
                if count < 1 or len(payload) < m.PEDSTATE.size + count * m.PEDSTATE_ENTRY.size \
                        or any(r != pid for r in rows):
                    raise ValueError
            except (ValueError, IndexError, struct.error):
                self.log.debug("player %d: dropping a malformed or foreign on-foot frame" % pid)
                return False
        self.broadcast(payload, tag, skip=conn)
        return True

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

        # Plugins first: a command ("!help") is consumed and never relayed.
        with self.lock:
            info = self.clients.get(conn)
        if info is not None and self.plugins.chat(Player(conn, info), line):
            self.log.info("[cmd] %s: %s" % (name, line.split(None, 1)[0]))
            return

        self.log.info("[chat] %s: %s" % (name, line))

        out = m.CHAT.pack(pid, 0, 0, 0, line.encode("latin1", "replace")[:95])
        # To EVERYONE, the sender included. The client never prints its own line:
        # MpSendChat (mp_session.c) sends it to the host and waits for the host's
        # echo, exactly as a game host republishes "to everyone (including the
        # sender)". Skipping the sender here meant a player on a dedicated server
        # never saw anything they typed -- alone on the server, chat looked dead.
        self.broadcast(out, m.TAG["chat"])

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
                self.clients[conn] = {"id": pid, "name": name, "addr": who,
                                      "player_id": m.parse_player_id(payload)}

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
        # spare carries the session rules (bits 1..: mp_proto.h, MpHandleStart reads
        # (spare >> 1) & 0xff) -- a 0 here, and a 0 in the welcome's rules byte,
        # meant every client adopted "no rules": cops OFF, whatever this server's
        # config said.
        sess = m.SESSION.pack(0, self.city, time_of_day_index(self.cfg), weather_index(self.cfg),
                              seed, max(2, self.player_count() + 1), 1,
                              (int(self.cfg["rules"]) & 0x7F) << 1)
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

        # THE MOTD, and only to THIS connection: it is a greeting, not an
        # announcement. It goes down the chat channel because that is already the
        # client's own "SERVER:" voice (see cmd_say): player id 0xFF matches no
        # player, so the client prints the text with no name attached to it.
        # The text is the MESSAGE ONLY: the client labels a speaker with no player row
        # as "SERVER" itself, so a prefix here would read "SERVER: SERVER: ...".
        motd = (self.cfg.get("motd") or "").strip()

        if motd:
            ink = _ink_bytes(self.cfg.get("motd_color"))
            try:
                for piece in _chat_chunks(motd):
                    m.send_frame(conn, m.TAG["chat"],
                                 m.CHAT.pack(0xFF, ink[0], ink[1], ink[2], piece))
            except OSError:
                pass
            self.log.debug("sent the motd to %s" % who)

        with self.lock:
            joined = self.clients.get(conn)
        if joined is not None:
            self.plugins.call("on_player_join", Player(conn, joined))

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
                elif tag in (m.TAG["ped"], m.TAG["traffic"]):
                    self.relay_peer_state(conn, tag, payload, pid)
                elif tag == m.TAG["hit"]:
                    relay.hit(self, conn, pid, payload)
                elif tag == m.TAG["color"]:
                    relay.color(self, conn, pid, payload)
                elif tag == m.TAG["channel"]:
                    relay.channel(self, conn, pid, payload)
                elif tag == m.TAG["chat"]:
                    self._chat(conn, payload, pid, name)
                elif tag == m.TAG["ping"]:
                    # Liveness. Echo the tick; with no PONG the client's last-receive
                    # clock stops advancing and it drops itself at its idle timeout.
                    try:
                        m.send_frame(conn, m.TAG["pong"], payload or b"\x00\x00\x00\x00")
                    except OSError:
                        break
                elif tag == m.TAG["pong"]:
                    self.note_pong(conn, payload)
                elif tag == m.TAG["leave"]:
                    break
        finally:
            with self.lock:
                info = self.clients.pop(conn, None)
                if info:
                    self.cars.pop(info["id"], None)
                    self.colors.pop(info["id"], None)
            if info:
                self.plugins.call("on_player_leave", Player(conn, info))
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
                              int(self.cfg["gamemode"]), self.city,
                              time_of_day_index(self.cfg), weather_index(self.cfg),
                              seed, 0xFF, int(self.cfg["rules"]) & 0xFF)

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
                # sleep a beat -- or less, when the rules changed and the listing
                # must say so now (readvertise): re-registering replaces our entry
                if self.readvertise_evt.wait(interval) and not self.stop.is_set():
                    self.readvertise_evt.clear()
                    token = None
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
            ("plugins", "loaded and disabled plugins", self.cmd_plugins),
        ] + self.plugins.console_entries({"status", "list", "say", "kick", "beacon",
                                          "master", "stop", "help", "plugins"})

    def cmd_plugins(self, _rest):
        rows = ["loaded:   %s" % (", ".join(n for n, _ in self.plugins.plugins) or "none")]
        for n, why in sorted(self.plugins.disabled.items()):
            rows.append("disabled: %s (%s)" % (n, why))
        return "\n".join(rows)

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
            "gamemode:  %d" % int(self.cfg["gamemode"]),
            "rules:     %s" % rules_text(int(self.cfg["rules"])),
            "time:      %s, %s" % (TIMES_OF_DAY[time_of_day_index(self.cfg)],
                                   WEATHERS[weather_index(self.cfg)]),
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
        # No "SERVER:" prefix: the client labels a speaker with no player row as
        # SERVER itself (0xFF matches no player), so a prefix would double it.
        for piece in _chat_chunks(rest):
            self.broadcast(m.CHAT.pack(0xFF, 0, 0, 0, piece), m.TAG["chat"])
        self.log.info("console: said %r" % rest)
        return "said to %d player(s)" % self.player_count()

    def cmd_kick(self, rest):
        rest = (rest or "").strip()

        if not rest.isdigit():
            return "kick who?  (kick <id>; 'list' shows the ids)"

        return self.kick_player(int(rest))

    def kick_player(self, pid, reason=""):
        with self.lock:
            target = [(conn, c) for conn, c in self.clients.items() if c["id"] == pid]

        if not target:
            return "no player %d connected  ('list')" % pid

        conn, c = target[0]

        # The protocol already has this message, and the client handles it properly
        # ("You were kicked by the host" then out to the frontend) -- so send that
        # rather than just dropping the socket and leaving them guessing.
        try:
            self.send_to(conn, m.TAG["kick"], m.KICK.pack(pid, 0, 0, 0))
        except OSError:
            pass

        self.log.info("kicked player %d '%s' (%s)%s" % (pid, c["name"], c["addr"],
                                                        (": " + reason) if reason else ""))

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
        self.readvertise_evt.set()       # wake the master loop so it unregisters now
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
        threads.append(threading.Thread(target=tick_loop, args=(self.plugins, self.stop),
                                        daemon=True))
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
            self.readvertise_evt.set()
            srv.close()
            self.plugins.unload()
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
    ap.add_argument("--cops", dest="cops", action="store_true", default=None,
                    help="police cars spawn (default)")
    ap.add_argument("--no-cops", dest="cops", action="store_false")
    ap.add_argument("--cop-difficulty", type=int, choices=(0, 1, 2), metavar="N",
                    help="cop difficulty 0..2 (default 1)")
    ap.add_argument("--cop-immunity", action="store_true", default=None,
                    help="cops exist but never come after anyone")
    ap.add_argument("--invulnerable", action="store_true", default=None,
                    help="cars take no damage")
    ap.add_argument("--time-of-day", choices=TIMES_OF_DAY, help="session time of day (default day)")
    ap.add_argument("--weather", choices=WEATHERS, help="session weather (default clear)")
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


RULE_KEYS = ("cops", "cop_difficulty", "cop_immunity", "invulnerable")


TIMES_OF_DAY = ("dawn", "day", "dusk", "night")      # engine order (0..3)
WEATHERS = ("clear", "rain", "wet")                   # engine order (0..2)


def _named_index(value, names, default):
    v = str(value).strip().lower()
    if v.isdigit() and int(v) < len(names):
        return int(v)
    return names.index(v) if v in names else default


def time_of_day_index(cfg):
    return _named_index(cfg.get("time_of_day", "day"), TIMES_OF_DAY, 1)


def weather_index(cfg):
    return _named_index(cfg.get("weather", "clear"), WEATHERS, 0)


def pack_rules(cops, invuln, diff, immune):
    """mp_proto.h MP_RULES_PACK: cops 0x1, invuln 0x2, difficulty << 2 (mask 0x3), immune 0x10."""
    return ((0x1 if cops else 0) | (0x2 if invuln else 0)
            | ((int(diff) & 0x3) << 2) | (0x10 if immune else 0))


def rules_text(packed):
    return "cops %s, difficulty %d, immunity %s, invulnerable %s" % (
        "on" if packed & 0x1 else "off", (packed >> 2) & 0x3,
        "on" if packed & 0x10 else "off", "on" if packed & 0x2 else "off")


def resolve_rules(cfg, file_keys):
    """Set cfg["rules"] (the packed int the wire uses) from the readable keys.

    An old config that carries "rules" and none of the readable keys is honoured
    as-is; returns True then, so the caller can log a deprecation line."""
    legacy = ("rules" in file_keys and not any(k in file_keys for k in RULE_KEYS)
              and True)
    if legacy:
        cfg["rules"] = int(cfg["rules"]) & 0x1F
        return True
    diff = int(cfg.get("cop_difficulty", 1))
    if diff < 0 or diff > 2:
        diff = 1
    cfg["cop_difficulty"] = diff
    cfg["rules"] = pack_rules(cfg.get("cops", True), cfg.get("invulnerable", False),
                              diff, cfg.get("cop_immunity", False))
    return False


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
        ("idle_drop_s", "idle_drop_s"), ("cops", "cops"),
        ("cop_difficulty", "cop_difficulty"), ("cop_immunity", "cop_immunity"),
        ("invulnerable", "invulnerable"),
        ("time_of_day", "time_of_day"), ("weather", "weather"),
        ("log_level", "log_level"), ("log_file", "log_file"),
        ("master", "master"), ("master_name", "master_name"),
        ("advertise_host", "advertise_host"), ("advertise_port", "advertise_port"),
        ("master_interval_s", "master_interval_s"),
    ))
    file_keys = set()
    if path:
        with open(path, "r", encoding="utf-8") as f:
            file_keys = set(json.load(f))
    if any(getattr(args, k, None) is not None for k in RULE_KEYS):
        file_keys |= {"cops"}      # a rules flag on the command line wins over a legacy int
    cfg["_rules_legacy"] = resolve_rules(cfg, file_keys)
    return cfg, path


def main(argv=None):
    cfg, path = build_config(argv)
    log = Log(cfg.get("log_level", "info"), cfg.get("log_file", ""))
    if path:
        log.debug("config: %s" % path)
    proto_check.assert_ok()
    if cfg.pop("_rules_legacy", False):
        log.warning("config: \"rules\": %d is deprecated -- use \"cops\", \"cop_difficulty\", "
                    "\"cop_immunity\", \"invulnerable\" (%s)" % (cfg["rules"], rules_text(cfg["rules"])))
    log.info("rules: %s" % rules_text(int(cfg["rules"])))

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

    for key, names, idx in (("time_of_day", TIMES_OF_DAY, time_of_day_index),
                            ("weather", WEATHERS, weather_index)):
        if _named_index(cfg[key], names, -1) < 0:
            log.warning("%s %r is not one of %s; using %s"
                        % (key, cfg[key], "/".join(names), names[idx(cfg)]))
        cfg[key] = names[idx(cfg)]

    cfg["_config_path"] = path
    server = Dedicated(cfg, log)

    log.debug("signals: %s" % ", ".join(install_signals(server.stop, log)))

    console = Console(log, server.console_commands())
    server.console = console
    console.start()

    return server.run()

