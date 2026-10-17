"""Exercise the kit's console without a terminal: dispatch, help generation, and
the no-tty path. Kept beside the module so it can be re-run while building on it."""

import sys

sys.path.insert(0, ".")

from serverkit import Console, Log   # noqa: E402

log = Log("debug")

calls = []


def status(_rest):
    calls.append("status")
    return "name: Kit Test\nport: 1480"


def say(rest):
    calls.append("say " + rest)
    return 0                            # not a str: nothing printed, no crash


c = Console(log, [
    ("status", "show the server's state", status),
    ("say", "broadcast a message", say),
    ("help", "list these commands", lambda _r: c.help_text()),
])

print("interactive():", c.interactive())
print("start() with a terminal:", c.start())
print("-- and with stdin that is NOT a terminal (docker/systemd): --")


class NotATerminal:
    """What stdin looks like to a container or a supervisor: readable, not a tty."""
    def isatty(self):
        return False

    def readline(self):
        return ""


real_stdin = sys.stdin
sys.stdin = NotATerminal()
print("interactive():", c.interactive())
print("start() without a terminal:", c.start())
sys.stdin = real_stdin
c.run_lines(["help", "status", "say hello there", "bogus", "   ", ""])
print("handlers called:", calls)
print("unknown command handled, server still alive:", True)
