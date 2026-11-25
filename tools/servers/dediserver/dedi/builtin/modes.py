"""modes -- the session's game mode: show it, and switch it like a listen host.

The mode is the "gamemode" key (0 TAKE A RIDE, 1 FOX N HOUNDS, 2 CAR BOMB TAG,
3 COPS N ROBBERS). The server referees every one of them itself (dedi/markref.py,
kept in lockstep with mp_mark.c by tools/mode_check/run_mode_check.py); this
plugin only adds the commands.

  !mode                       the mode and its round time (anyone)
  !mode <id|name> [minutes]   switch (admin / console). Round state and referee
                              restart at round 0 of the new mode and every player
                              rejoins into it, as a map rotation does. Names:
                              takearide, foxnhounds, carbombtag, copsnrobbers
                              (or ride / fox / bomb / cnr).
  !modetime <1..5>            the round time for modes 1-3 (admin / console).
                              Like a mode switch it restarts at round 0 and has
                              everyone rejoin: the in-game host, too, only changes
                              it in the lobby, and a running clock never jumps.
  !mode ... save              also write the change into dediserver.json

A listen host can only change mode in its lobby (MpSetSessionGamemode); a
dedicated server is always in a match, hence the rejoin.
"""
ENABLED_BY_DEFAULT = True
DESCRIPTION = "game mode: !mode (show / switch), !modetime"
COLOR = "#FFD24A"

_server = None


def _describe():
    import mp_test as m
    gid = int(_server.config_get("gamemode", 0) or 0)
    t = ""
    if m.mode_timer(gid) != m.MODE_TIMER_NONE:
        t = ", %d min rounds" % int(_server.config_get("mode_time", m.MARK_MINUTES_DEFAULT) or 3)
    return "%d (%s)%s" % (gid, m.mode_name(gid), t)


def on_load(server):
    global _server
    _server = server
    server.register_command("mode", cmd_mode, console=True,
                            help="!mode [id|name] [minutes] [save] - show / switch the game mode")
    server.register_command("modetime", cmd_modetime, admin=True, console=True,
                            help="!modetime <1..5> - round time for modes 1-3")
    server.register_command("endround", cmd_endround, admin=True, console=True,
                            help="!endround - end the active timed round now")
    server.register_command("restart", cmd_restart, admin=True, console=True,
                            help="!restart - restart the current round now")
    server.log.info("game mode: %s" % _describe())


def _is_admin(caller):
    return bool(getattr(caller, "admin", True))     # the console has no Player: admin


def cmd_mode(caller, args):
    words = [w for w in (args or "").split() if w]
    if not words:
        return "game mode: %s" % _describe()
    if not _is_admin(caller):
        return "only an admin can change the game mode"
    save = words[-1].lower() == "save"
    if save:
        words = words[:-1]
    minutes = None
    if len(words) >= 2 and words[-1].isdigit():
        minutes = int(words.pop())
    try:
        _server.set_gamemode(" ".join(words), minutes)
    except ValueError as e:
        return str(e)
    if save:
        _server.save_config_keys(["gamemode", "mode_time"])
    msg = "game mode now %s%s" % (_describe(), " (saved)" if save else "")
    _server.broadcast(msg, COLOR)
    return msg


def cmd_modetime(caller, args):
    import mp_test as m
    try:
        n = int((args or "").split()[0])
    except (IndexError, ValueError):
        return "usage: !modetime <%d..%d>" % (m.MARK_MINUTES_MIN, m.MARK_MINUTES_MAX)
    _server.set_gamemode(int(_server.config_get("gamemode", 0) or 0), n)
    msg = "round time now %d min - restarting the match" % int(_server.config_get("mode_time"))
    _server.broadcast(msg, COLOR)
    return msg


def _actor(caller):
    return "the console" if getattr(caller, "conn", None) is None else ("admin %s" % caller.name)


def cmd_endround(caller, _args):
    import mp_test as m
    gid = int(_server.config_get("gamemode", 0) or 0)
    if m.mode_timer(gid) == m.MODE_TIMER_NONE:
        return "current mode has no round timer"
    if not _server.end_round():
        return "no active round to end"
    who = _actor(caller)
    _server.log.info("round ended by %s" % who)
    msg = "Round ended by %s" % who
    _server.broadcast(msg, COLOR)
    return msg


def cmd_restart(caller, _args):
    if not _server.restart_round():
        return "cannot restart round right now"
    who = _actor(caller)
    _server.log.info("round restarted by %s" % who)
    msg = "Round restarted by %s" % who
    _server.broadcast(msg, COLOR)
    return msg
