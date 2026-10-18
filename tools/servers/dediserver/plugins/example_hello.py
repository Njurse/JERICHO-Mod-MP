"""example_hello -- a minimal custom plugin. Disabled by default.

Enable it by listing it in dediserver.json, e.g.
    "plugins": ["commands", "admin", "example_hello"]
Copy this file as a starting point: every hook is optional (see dedi/plugins.py).
"""
ENABLED_BY_DEFAULT = False
DESCRIPTION = "greets joiners and adds !hello"

_server = None


def on_load(server):
    global _server
    _server = server
    server.register_command("hello", cmd_hello, help="say hello back")
    server.log.info("example_hello loaded")


def on_player_join(player):
    _server.send_private(player, "Hello %s, welcome!" % player.name)


def cmd_hello(caller, args):
    return "hello, %s!" % caller.name
