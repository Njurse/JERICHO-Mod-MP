"""admin -- who is an admin, and the commands admins (and players) get.

An admin is recognised by PLAYER ID: the 32 hex digits a JERICHO client sends in
its HELLO (generated once per install, [mp] player_id). List ids under "admins" in
dediserver.json; a player finds their own with !whoami. Optionally an
"admin_password" lets a player !login for the rest of their session.

Honest limits: the id is a bearer secret (whoever copies a player's config is
that player to this server) and the link is unencrypted, so the id and a !login
password can be read off the network path. Good enough for a hobby server; do not
reuse the admin password anywhere else.
"""
import hmac
import time

import mp_test as m          # normalize_player_id: dashed or plain hex, any case

DESCRIPTION = ("admins by player id / !login; !whoami !who !admin !kick !cops !difficulty "
               "!time !weather !motd !say")

LOGIN_MAX_FAILS = 3        # wrong passwords allowed per window ...
LOGIN_WINDOW_S = 60.0      # ... within this many seconds ...
LOGIN_LOCKOUT_S = 300.0    # ... before !login is refused for this long
now = time.monotonic       # tests replace this

_server = None
_fails = {}                # key (player id or ip) -> [timestamps]
_locked = {}               # key -> unlock time


def on_load(server):
    global _server
    _server = server
    _fails.clear()
    _locked.clear()
    reg = server.register_command
    reg("whoami", cmd_whoami, help="your slot, name and player id")
    reg("who", cmd_who, help="the players connected")
    reg("login", cmd_login, help="!login <password> - become an admin for this session")
    reg("logout", cmd_logout, help="drop your admin rights for this session")
    reg("kick", cmd_kick, admin=True, help="!kick <slot|name> [reason] - remove a player")
    reg("cops", cmd_cops, admin=True, console=True, help="!cops on|off - police (next level load)")
    reg("difficulty", cmd_difficulty, admin=True, console=True,
        help="!difficulty 0-2 - cop difficulty (next level load)")
    reg("motd", cmd_motd, admin=True, console=True,
        help="!motd [text|save] - show, set, or save the greeting to dediserver.json")
    reg("time", cmd_time, admin=True, console=True,
        help="!time dawn|day|dusk|night - time of day (next level load)")
    reg("weather", cmd_weather, admin=True, console=True,
        help="!weather clear|rain|wet - weather (next level load)")
    reg("admin", cmd_admin, admin=True, console=True,
        help="!admin add|remove <slot|name|uuid>, !admin list - saved to dediserver.json")
    reg("say", cmd_say, admin=True, help="!say <text> - a line from the server to everyone")


def _admin_ids():
    out = set()
    for x in (_server.config_get("admins") or []):
        n = m.normalize_player_id(x)
        if n:
            out.add(n)
        else:
            _server.log.warning("admins: %r is not a player id (a UUID); ignored" % (x,))
    return out


def _is_listed(player):
    return bool(player.player_id) and m.normalize_player_id(player.player_id) in _admin_ids()


def on_player_join(player):
    if _is_listed(player):
        player.admin = True
        _server.log.info("player %d '%s' is an admin (by player id)" % (player.id, player.name))
        _server.send_private(player, "You are an admin on this server (!help).", "#9CFF9C")


def cmd_whoami(caller, _args):
    pid = caller.player_id or "none (your game is too old to send one)"
    return "slot %d, '%s', player id %s%s" % (caller.id, caller.name, pid,
                                             ", admin" if caller.admin else "")


def cmd_who(caller, _args):
    rows = []
    for p in _server.players():
        row = "%2d  %s%s" % (p.id, p.name, " (admin)" if p.admin else "")
        if caller.admin:          # ids and addresses are for admins only
            row += "  %s  %s" % (p.player_id or "no id", p.addr)
        rows.append(row)
    return "\n".join(rows) or "nobody is connected"


def _login_key(caller):
    return caller.player_id or caller.addr.rsplit(":", 1)[0]


def cmd_login(caller, args):
    if caller.admin:
        return "you are already an admin"
    password = _server.config_get("admin_password") or ""
    if not password:
        return "this server has no admin password; ask for your player id to be listed"
    key, t = _login_key(caller), now()
    if _locked.get(key, 0) > t:
        return "too many wrong passwords; try again in %ds" % int(_locked[key] - t)
    if hmac.compare_digest(args.strip().encode(), password.encode()):
        caller.admin = True
        _fails.pop(key, None)
        _server.log.info("player %d '%s' logged in as admin" % (caller.id, caller.name))
        return "you are now an admin for this session"
    recent = [x for x in _fails.get(key, []) if t - x < LOGIN_WINDOW_S] + [t]
    _fails[key] = recent
    _server.log.warning("failed !login from player %d '%s' (%s)" % (caller.id, caller.name, caller.addr))
    if len(recent) >= LOGIN_MAX_FAILS:
        _locked[key] = t + LOGIN_LOCKOUT_S
        _fails.pop(key, None)
        return "wrong password; !login is locked for %ds" % int(LOGIN_LOCKOUT_S)
    return "wrong password"


def cmd_logout(caller, _args):
    if _is_listed(caller):
        return "you are listed as an admin by player id; that cannot be dropped here"
    caller.admin = False
    return "admin rights dropped"


def cmd_kick(caller, args):
    words = args.split()
    target, reason = None, None
    # A name may contain spaces, so try the longest leading run of words first.
    for n in range(len(words), 0, -1):
        target = _server.find_player(" ".join(words[:n]))
        if target is not None:
            reason = " ".join(words[n:]) or None
            break
    if target is None:
        return "kick who? (!kick <slot|name> [reason]; !who lists them)"
    return _server.kick(target, reason, by="admin %s" % caller.name)


def cmd_cops(caller, args):
    a = args.strip().lower()
    if a not in ("on", "off"):
        return "usage: !cops on|off"
    return "rules: %s -- applies from the next level load" % _server.set_cops(a == "on")


def cmd_difficulty(caller, args):
    a = args.strip()
    if a not in ("0", "1", "2"):
        return "usage: !difficulty 0|1|2"
    return "rules: %s -- applies from the next level load" % _server.set_difficulty(int(a))


def cmd_motd(caller, args):
    text = args.strip()
    if not text:
        return "motd: %s" % (_server.config_get("motd") or "(none)")
    if text.lower() == "save":
        _server.save_config("motd")
        return "motd saved to dediserver.json"
    _server.config_set("motd", text)
    _server.log.info("motd set by %s" % _server.describe(caller))
    return "motd set for this run (!motd save keeps it)"


def cmd_time(caller, args):
    try:
        return "next level load: %s" % _server.set_session(time_of_day=args.strip() or None)
    except ValueError as e:
        return "usage: !time dawn|day|dusk|night (%s)" % e


def cmd_weather(caller, args):
    try:
        return "next level load: %s" % _server.set_session(weather=args.strip() or None)
    except ValueError as e:
        return "usage: !weather clear|rain|wet (%s)" % e


def cmd_admin(caller, args):
    parts = args.split(None, 1)
    verb = parts[0].lower() if parts else "list"
    rest = parts[1].strip() if len(parts) > 1 else ""
    ids = [m.normalize_player_id(x) for x in (_server.config_get("admins") or [])]
    ids = [x for x in ids if x]
    if verb == "list":
        if not ids:
            return "no admins listed (by player id)"
        names = {p.player_id: p.name for p in _server.players() if p.player_id}
        return "\n".join("%s%s" % (i, ("  (%s, online)" % names[i]) if i in names else "") for i in ids)
    if verb not in ("add", "remove") or not rest:
        return "usage: !admin add|remove <slot|name|uuid>, !admin list"
    target = _server.find_player(rest)
    pid = (target.player_id if target is not None else None) or m.normalize_player_id(rest)
    if not pid:
        return "%s: no such player online, and not a player id" % rest
    pid = m.normalize_player_id(pid)
    if verb == "add":
        if pid in ids:
            return "%s is already an admin" % pid
        ids.append(pid)
    else:
        if pid not in ids:
            return "%s is not listed" % pid
        ids.remove(pid)
    _server.config_set("admins", ids)
    _server.save_config("admins")
    for p in _server.players():
        if p.player_id and m.normalize_player_id(p.player_id) == pid:
            p.admin = (verb == "add")
            _server.send_private(p, "You are %s an admin on this server."
                                 % ("now" if verb == "add" else "no longer"), "#9CFF9C")
    _server.log.info("admin %s %s by %s" % (verb, pid, _server.describe(caller)))
    return "%s %s (saved to dediserver.json)" % ("added" if verb == "add" else "removed", pid)


def cmd_say(caller, args):
    if not args.strip():
        return "say what? (!say <text>)"
    _server.broadcast(args.strip())
    return None
