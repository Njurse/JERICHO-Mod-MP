"""The pieces both JERICHO mp servers need, in one place.

The master and the dedicated server grew up separately and ended up carrying the
same three things twice: a Log (two near-identical copies), a DEFAULTS -> JSON ->
CLI config load, and a startup/stop path (and only one of the two had signals at
all). Adding an interactive console to both would have made it three copies, so
the seams moved here first -- and the console lives here too, which is why this
module exists at all rather than a bit more copy-paste per server.

Nothing in here knows what a registry or a session is. It is the plumbing only:
logging, config merging, signal handling, and the console's input loop.
"""

import json
import signal
import sys
import threading
import time

# --------------------------------------------------------------------------- log
LEVELS = {"debug": 10, "info": 20, "warning": 30, "error": 40}


class Log:
    """stdout, and optionally a file.

    One format for both servers on purpose: the lines that matter when a client
    "never connected" vs "connected and was dropped" are grepped out of captured
    output, and two formats would mean two greps.
    """

    def __init__(self, level="info", path=""):
        self.min = LEVELS.get(level, 20)
        self.lock = threading.Lock()
        self.fh = None
        if path:
            self.fh = open(path, "a", encoding="utf-8", buffering=1)

    def _emit(self, lvl, msg):
        if LEVELS[lvl] < self.min:
            return
        line = "%s [%s] %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), lvl.upper(), msg)
        with self.lock:
            print(line, flush=True)
            if self.fh:
                self.fh.write(line + "\n")

    def debug(self, msg):
        self._emit("debug", msg)

    def info(self, msg):
        self._emit("info", msg)

    def warning(self, msg):
        self._emit("warning", msg)

    def error(self, msg):
        self._emit("error", msg)


# ------------------------------------------------------------------------ config
def load_config(path, defaults):
    """DEFAULTS, then the JSON file over them. CLI flags are applied on top by
    apply_overrides.

    A path that was ASKED FOR and cannot be read raises: a typo'd --config used to
    be either fatal or silently ignored depending on which server you started, and
    "the server came up with settings nobody asked for" is the worse of the two.
    """
    cfg = dict(defaults)

    if path:
        with open(path, "r", encoding="utf-8") as f:
            cfg.update(json.load(f))

    return cfg


def apply_overrides(cfg, args, pairs):
    """Copy the CLI flags that were actually given over the config.

    `pairs` is (argparse dest, config key) -- the two differ often enough
    (--ttl -> ttl_s, --persist -> persist_path) that spelling it out beats a
    guess. A flag left as None means "not given", so it cannot clobber the file.
    """
    for flag, key in pairs:
        v = getattr(args, flag, None)

        if v is not None:
            cfg[key] = v

    return cfg


# ------------------------------------------------------------------------ signals
def install_signals(stop, log, names=("SIGINT", "SIGTERM", "SIGBREAK")):
    """Turn the usual signals into "set stop", and say which ones took.

    SIGBREAK matters on Windows: terminating a console app from a task manager or
    a script sends it, and without a handler the graceful path (unregistering from
    the master, closing clients) never runs. Returns the names that installed, so
    a caller can log them rather than guess.
    """
    def on_signal(signum, _frame):
        log.info("signal %d received" % signum)
        stop.set()

    installed = []

    for name in names:
        sig = getattr(signal, name, None)

        if sig is None:
            continue

        try:
            signal.signal(sig, on_signal)
        except (ValueError, OSError):     # not the main thread, or unsupported
            continue

        installed.append(name)

    return installed


# ------------------------------------------------------------------------ console
class Console:
    """An interactive command line on stdin, for a server run in a terminal.

    Deliberately NOT a network admin port: nothing listens, nothing authenticates,
    and the only people who can use it already have a shell on the machine. That
    is the right trade for two servers that mostly run under a supervisor or in a
    container -- an unauthenticated remote control socket would be a liability.

    When stdin is not a terminal (docker without -it, a systemd unit, a captured
    run) there is nobody to type, so it says one line and stays out of the way
    rather than dying on a read that never returns.

    `help` is GENERATED from the command table, so a new command cannot exist
    without being documented in it.
    """

    def __init__(self, log, commands, prompt=""):
        """`commands` is a list of (name, help, handler).

        A handler takes the rest of the line as one string and may return a string
        to be printed, or None. Raising is allowed: it is caught and reported as a
        failed command rather than taking the server down.
        """
        self.log = log
        self.commands = {}
        self.order = []
        self.prompt = prompt

        for name, text, fn in commands:
            self.commands[name] = (name, text, fn)
            self.order.append(name)

    # -- input ------------------------------------------------------------
    @staticmethod
    def interactive():
        """Is there a person on the other end?"""
        try:
            return sys.stdin is not None and sys.stdin.isatty()
        except (ValueError, OSError):        # closed, or not a real stream
            return False

    def start(self):
        """Run the console on a daemon thread. False when there is no terminal."""
        if not self.interactive():
            self.log.info("console: off (stdin is not a terminal; run it in a shell "
                          "or with docker run -it to get it)")
            return False

        self.log.info("console: on -- type 'help' for commands")

        t = threading.Thread(target=self.loop, name="console", daemon=True)
        t.start()
        return True

    def loop(self):
        while True:
            try:
                line = sys.stdin.readline()
            except (ValueError, OSError):
                return

            if line == "":                   # end of input, not an error
                self.log.info("console: end of input")
                return

            self.dispatch(line)

    def run_lines(self, lines):
        """Drive the console from a list of lines instead of a terminal.

        This is how the console is checked without a person and without a pty: the
        same dispatch path a typed line takes, so a test exercises the real thing.
        """
        for line in lines:
            self.dispatch(line)

    # -- dispatch ---------------------------------------------------------
    def dispatch(self, line):
        line = (line or "").strip()

        if not line:
            return

        name, _, rest = line.partition(" ")
        cmd = self.commands.get(name)

        if cmd is None:
            self.say("unknown command '%s' -- 'help' lists them" % name)
            return

        try:
            out = cmd[2](rest.strip())
        except Exception as e:               # a bad command must not kill the server
            self.say("%s: %s" % (name, e))
            return

        if isinstance(out, str) and out:
            self.say(out)

    def say(self, text):
        """Console output is the operator's own conversation with the server, so it
        goes to stdout plain -- unterminated by log prefixes on every line. Commands
        that CHANGE something log that change themselves."""
        for line in str(text).splitlines():
            print(line, flush=True)

    def help_text(self):
        width = max([len(n) for n in self.order] + [0])
        lines = ["commands:"]

        for name in self.order:
            lines.append("  %-*s  %s" % (width, name, self.commands[name][1]))

        return "\n".join(lines)

