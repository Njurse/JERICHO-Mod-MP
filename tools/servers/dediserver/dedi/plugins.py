"""dedi.plugins -- the dedicated server's plugin system.

A plugin is a Python module. Built-ins live in dedi/builtin/; your own go in the
directory named by "plugins_dir" (default: plugins/ beside dediserver.py). Every
hook is optional, a plain module-level function:

    on_load(server)              the plugin was enabled; `server` is a ServerAPI
    on_unload()                  the server is stopping (or the plugin was disabled)
    on_player_join(player)       a player was accepted (after the welcome)
    on_player_leave(player)      a player left / was dropped
    on_chat(player, text)        a chat line; return True to CONSUME it (not relayed)
    on_tick()                    about once a second

Optional module attributes:

    ENABLED_BY_DEFAULT = False   only load when named in "plugins"
    DESCRIPTION = "..."          shown by the console's `plugins` command

Commands are registered from on_load with server.register_command(...). A plugin
that raises is logged and DISABLED; the server keeps running.

Which plugins load: "plugins": null (the default) loads every plugin found --
built-ins first, then plugins_dir, each alphabetically -- except those with
ENABLED_BY_DEFAULT = False and those named in "plugins_disabled". "plugins": [names]
loads exactly those, in that order.
"""
from __future__ import annotations

import importlib.util
import os
import threading
import time
import traceback

BUILTIN_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "builtin")
HOOKS = ("on_load", "on_unload", "on_player_join", "on_player_leave", "on_chat", "on_tick")


class Player:
    """One connected player, as plugins see it.

    id         the session slot (1..max_players-1)
    name       the name they joined with (free text: never trust it for identity)
    addr       "ip:port"
    player_id  32 hex digits from the client's JPID block, or None (older client)
    admin      True once a plugin (admin) has granted it
    state      a dict plugins may keep per-player data in
    """

    def __init__(self, conn, info):
        self.conn = conn
        self._info = info

    id = property(lambda self: self._info["id"])
    name = property(lambda self: self._info["name"])
    addr = property(lambda self: self._info["addr"])
    player_id = property(lambda self: self._info.get("player_id"))
    state = property(lambda self: self._info.setdefault("state", {}))

    @property
    def admin(self):
        return bool(self._info.get("admin"))

    @admin.setter
    def admin(self, value):
        self._info["admin"] = bool(value)

    def __repr__(self):
        return "Player(%d, %r)" % (self.id, self.name)


class Console:
    """The caller for a command typed at the server console: always an admin."""
    id = 0
    name = "console"
    addr = "console"
    player_id = None
    admin = True
    conn = None
    state = {}


CONSOLE = Console()


class Command:
    def __init__(self, name, handler, admin, help, plugin, console):
        self.name, self.handler, self.admin = name, handler, admin
        self.help, self.plugin, self.console = help, plugin, console


class ServerAPI:
    """What a plugin gets in on_load. A thin, stable face over the server."""

    def __init__(self, server, manager, plugin_name):
        self._s = server
        self._m = manager
        self.plugin = plugin_name
        self.log = _PrefixLog(server.log, plugin_name)

    # -- players
    def players(self):
        return self._s.player_views()

    def find_player(self, key):
        """A player by slot number or (case-insensitive) exact name, else None."""
        key = str(key).strip()
        for p in self.players():
            if (key.isdigit() and p.id == int(key)) or p.name.lower() == key.lower():
                return p
        return None

    # -- talking
    def send_private(self, player, text, color=None):
        """Chat line(s) to ONE player, in the server's voice. Console callers get
        nothing here: their reply is the command's return value."""
        if getattr(player, "conn", None) is not None:
            self._s.send_server_chat(text, color, conn=player.conn)

    def broadcast(self, text, color=None):
        self._s.send_server_chat(text, color)

    def kick(self, player, reason=None, by="admin", announce=True, rejoin=False):
        """Drop a player. With a reason they see "Kicked by <by>: <reason>" (cut to
        the protocol's 63 bytes) and chat is told; without one, the stock kick.
        rejoin=True tells current clients to reconnect by themselves (a city change)."""
        return self._s.kick_player(player.id, reason, by=by, announce=announce, rejoin=rejoin)

    # -- config / rules
    def config_get(self, key, default=None):
        return self._s.cfg.get(key, default)

    def config_set(self, key, value):
        self._s.cfg[key] = value

    def set_rules(self, **rules):
        """cops=, cop_difficulty=, cop_immunity=, invulnerable= -- applied from the
        next level load (a client reads the rules as it joins/loads); the discovery
        listing is refreshed at once."""
        return self._s.set_rules(**rules)

    def set_cops(self, on):
        return self.set_rules(cops=bool(on))

    def set_difficulty(self, level):
        return self.set_rules(cop_difficulty=int(level))

    def readvertise(self):
        self._s.readvertise()

    # -- commands
    def register_command(self, name, handler, admin=False, help="", console=False):
        """handler(caller, args) -> str reply (or None). `caller` is a Player, or
        the console (admin, no conn). console=True also offers it at the server
        console, unless a console command of that name already exists."""
        return self._m.register_command(name, handler, admin, help, self.plugin, console)

    def set_session(self, time_of_day=None, weather=None):
        """time_of_day (dawn/day/dusk/night) / weather (clear/rain/wet), from the
        next level load. Raises ValueError for an unknown name."""
        return self._s.set_session(time_of_day=time_of_day, weather=weather)

    def set_city(self, city):
        """Make `city` (0..3) the session city: new joiners load it (the welcome and
        start carry it) and the discovery listing is refreshed. Players already in
        a level stay in the old one until they rejoin. Returns the new city."""
        return self._s.set_city(city)

    def save_config(self, *keys):
        """Write these config keys back to dediserver.json (atomic, other keys kept)."""
        return self._s.save_config_keys(list(keys))

    def describe(self, player):
        """'name (slot N, uuid)' -- for logging who did something."""
        return "%s (slot %s, %s)" % (player.name, player.id, player.player_id or "no id")

    def commands(self):
        return self._m.commands_for(None)

    def commands_for(self, caller):
        return self._m.commands_for(caller)

    def run_command(self, caller, line):
        return self._m.run_command(caller, line)


class _PrefixLog:
    def __init__(self, log, name):
        self._log, self._p = log, "[%s] " % name

    def __getattr__(self, level):
        fn = getattr(self._log, level)
        return lambda msg: fn(self._p + str(msg))


class PluginManager:
    def __init__(self, server):
        self.server = server
        self.log = server.log
        self.plugins = []          # [(name, module)] in load order
        self.disabled = {}         # name -> reason
        self.commands = {}         # name -> Command
        self._lock = threading.RLock()

    # -- discovery
    def _user_dir(self):
        d = self.server.cfg.get("plugins_dir") or "plugins"
        if not os.path.isabs(d):
            d = os.path.join(self.server.home, d)
        return d

    @staticmethod
    def _scan(d):
        if not os.path.isdir(d):
            return []
        return sorted(f[:-3] for f in os.listdir(d)
                      if f.endswith(".py") and not f.startswith("_"))

    def _import(self, name, path):
        spec = importlib.util.spec_from_file_location("dedi_plugin_" + name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod

    def load(self):
        found = {}                                     # name -> path, built-ins win
        order = []
        for d in (BUILTIN_DIR, self._user_dir()):
            for n in self._scan(d):
                if n not in found:
                    found[n] = os.path.join(d, n + ".py")
                    order.append(n)
                else:
                    self.log.warning("plugin %s in %s ignored: a plugin of that name is "
                                     "already loaded" % (n, d))

        wanted = self.server.cfg.get("plugins")
        disabled = set(self.server.cfg.get("plugins_disabled") or [])
        explicit = wanted is not None
        names = list(wanted) if explicit else order

        for n in names:
            if n in disabled:
                continue
            if n not in found:
                self.log.warning("plugin %r is configured but was not found" % n)
                continue
            try:
                mod = self._import(n, found[n])
            except (Exception, SystemExit) as e:
                self._disable(n, "failed to import: %s" % e, tb=True)
                continue
            if not explicit and getattr(mod, "ENABLED_BY_DEFAULT", True) is False:
                continue
            self.plugins.append((n, mod))
            if hasattr(mod, "on_load"):
                if not self._call_one(n, mod, "on_load", ServerAPI(self.server, self, n)):
                    continue
            self.log.info("plugin loaded: %s" % n)

    def unload(self):
        for n, mod in reversed(list(self.plugins)):
            self._call_one(n, mod, "on_unload")
        self.plugins = []

    # -- hooks
    def _disable(self, name, reason, tb=False):
        with self._lock:
            self.disabled[name] = reason
            self.plugins = [(n, m) for n, m in self.plugins if n != name]
            for c in [c for c in self.commands.values() if c.plugin == name]:
                del self.commands[c.name]
        self.log.error("plugin %s disabled: %s" % (name, reason))
        if tb:
            self.log.debug(traceback.format_exc())

    def _call_one(self, name, mod, hook, *args):
        fn = getattr(mod, hook, None)
        if fn is None:
            return None
        try:
            return fn(*args)
        except (Exception, SystemExit) as e:   # a plugin must never stop the server
            self._disable(name, "%s raised %s: %s" % (hook, type(e).__name__, e), tb=True)
            return False

    def call(self, hook, *args):
        for n, mod in list(self.plugins):
            self._call_one(n, mod, hook, *args)

    def chat(self, player, text):
        """True when a plugin consumed the line (so it is not relayed)."""
        for n, mod in list(self.plugins):
            if self._call_one(n, mod, "on_chat", player, text) is True:
                return True
        return False

    # -- commands
    def register_command(self, name, handler, admin, help, plugin, console):
        name = name.lower().lstrip("!/")
        with self._lock:
            if name in self.commands and self.commands[name].plugin != plugin:
                # first come, first served: a later plugin cannot hijack a command
                self.log.error("plugin %s: command !%s is already registered by %s; "
                               "ignored" % (plugin, name, self.commands[name].plugin))
                return False
            self.commands[name] = Command(name, handler, bool(admin), help, plugin, console)
            return True

    def commands_for(self, caller):
        """The commands `caller` may run (None = all of them), sorted."""
        with self._lock:
            cmds = sorted(self.commands.values(), key=lambda c: c.name)
        return [c for c in cmds if caller is None or not c.admin or caller.admin]

    def run_command(self, caller, line):
        """Run '<name> <args>' for `caller`; the reply text (never raises)."""
        parts = line.strip().lstrip("!/").split(None, 1)
        if not parts:
            return None
        name, args = parts[0].lower(), (parts[1] if len(parts) > 1 else "")
        cmd = self.commands.get(name)
        if cmd is None:
            return "unknown command '%s' -- !help lists what you can use" % name
        if cmd.admin and not caller.admin:
            self.log.info("denied !%s to %s (not an admin)" % (name, getattr(caller, "name", "?")))
            return "!%s is for admins" % name
        if cmd.admin:
            self.log.info("admin: !%s by %s (slot %s, %s)" % (
                name, getattr(caller, "name", "?"), getattr(caller, "id", "?"),
                getattr(caller, "player_id", None) or "no id"))
        try:
            return cmd.handler(caller, args)
        except (Exception, SystemExit) as e:
            self.log.error("command %s (%s) raised %s: %s" % (name, cmd.plugin, type(e).__name__, e))
            self.log.debug(traceback.format_exc())
            return "!%s failed (see the server log)" % name

    def console_entries(self, taken):
        """(name, help, fn(rest)) rows for the operator console: console=True
        commands whose names the core does not already use."""
        out = []
        for c in self.commands_for(None):
            if c.console and c.name not in taken:
                out.append((c.name, c.help, (lambda rest, n=c.name: self.run_command(CONSOLE, n + " " + (rest or "")))))
        return out


def tick_loop(manager, stop, every=1.0):
    while not stop.wait(every):
        manager.call("on_tick")
