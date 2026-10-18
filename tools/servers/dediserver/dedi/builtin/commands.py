"""commands -- chat lines starting with '!' or '/' are commands, not chat.

The line is consumed (never relayed to the other players), run through the
shared command registry, and the reply goes privately to the sender. !help lists
only what the caller is allowed to run.
"""
DESCRIPTION = "'!'/'/' chat commands with private replies; !help"

REPLY_COLOR = "#FFE08A"
PREFIXES = ("!", "/")

_server = None


def on_load(server):
    global _server
    _server = server
    server.register_command("help", cmd_help, help="list the commands you can use")


def on_chat(player, text):
    if not text.startswith(PREFIXES) or len(text) < 2 or text[1] in " !/":
        return False
    reply = _server.run_command(player, text)
    if reply:
        _server.send_private(player, reply, REPLY_COLOR)
    return True


def cmd_help(caller, _args):
    rows = []
    for c in _server.commands_for(caller):
        rows.append("!%s%s - %s" % (c.name, " (admin)" if c.admin else "", c.help or ""))
    return "\n".join(rows) or "no commands"
