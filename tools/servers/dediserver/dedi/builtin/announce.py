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
    if _server.config_get("announce_joins", True):
        _server.broadcast("%s joined" % player.name, COLOR)


def on_player_leave(player):
    if _server.config_get("announce_joins", True):
        _server.broadcast("%s left" % player.name, COLOR)
