"""modes -- the game-mode plugin skeleton. DISABLED by default.

The session's game mode is the existing "gamemode" key (default 0 = TAKE A RIDE,
already in DEFAULTS; a config without it keeps working). The server sends that id
in the welcome, the start, the LAN beacon and the master advert, exactly as before;
this plugin changes none of that.

What it is for: the place a mode's server-side rules will live once the round state
exists (docs/GAMEMODES.md, "Dedicated server"). Today it only says which mode the
session runs and offers a read-only `!mode`. Enable it by naming it in "plugins",
e.g. "plugins": ["commands", "admin", "announce", "vote", "modes"].
"""
ENABLED_BY_DEFAULT = False
DESCRIPTION = "game-mode skeleton: names the session's mode, !mode (read-only)"

_server = None


def _mode_id():
    try:
        return int(_server.config_get("gamemode", 0) or 0)
    except (TypeError, ValueError):
        return 0


def _describe():
    import mp_test as m      # the single Python mirror of the mode ids (GAMEMODE_NAMES)
    gid = _mode_id()
    name = m.mode_name(gid)
    if gid not in m.GAMEMODE_NAMES:
        return "%d (%s: not a mode this server knows; clients play it as %s)" % (
            gid, name, m.mode_name(0))
    return "%d (%s)" % (gid, name)


def on_load(server):
    global _server
    _server = server
    server.register_command("mode", cmd_mode, help="the session's game mode", console=True)
    server.log.info("game mode: %s" % _describe())


def cmd_mode(caller, args):
    return "game mode: %s" % _describe()
