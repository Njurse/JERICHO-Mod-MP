"""announce -- "<name> joined" / "<name> left" as a server chat line to everyone.

Off with "announce_joins": false in dediserver.json.
"""
DESCRIPTION = "join/leave announcements"
COLOR = "#B0B0B0"

_server = None


def on_load(server):
    global _server
    _server = server


def on_player_join(player):
    # back after a city change (kicked to rejoin): not a join (the game's own
    # MpRejoinQuietTake keeps its join sound quiet the same way)
    if _server.is_rejoin(player, take=True):
        return
    if _server.config_get("announce_joins", True):
        # The same text the game's own host toasts (mp_session.c MpHandleHello).
        ver = player.version
        label = ("v" + ver if ver[:1].isdigit() else ver) if ver else "?"
        _server.broadcast("%s joined (%s, mods %06x)" % (player.name, label, player.mods_digest),
                          COLOR)


def on_player_leave(player):
    if _server.is_rejoin(player):
        return      # the city change kicked them to rejoin: not a leave
    if _server.config_get("announce_joins", True):
        _server.broadcast("%s left" % player.name, COLOR)
