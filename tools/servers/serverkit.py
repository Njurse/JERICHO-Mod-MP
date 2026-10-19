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
import os
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


def _add_missing(have, defaults, prefix=""):
    """Add (in place) every key of `defaults` that `have` lacks; recurse into dicts
    both sides hold. Existing keys and values are never touched. Returns the
    dotted names added."""
    added = []
    for k, v in defaults.items():
        if k.startswith("_"):
            continue                       # internal, never a file key
        if k not in have:
            have[k] = json.loads(json.dumps(v))   # a copy: never share a default list
            added.append(prefix + k)
        elif isinstance(have[k], dict) and isinstance(v, dict):
            added += _add_missing(have[k], v, prefix + k + ".")
    return added


def upgrade_config(path, defaults, skip=(), say=None):
    """Add the keys a newer version introduced to an existing JSON config file.

    Only MISSING keys are added (appended, in `defaults` order; nested dicts merged
    only to add); every existing key and value is kept verbatim. `skip` names keys
    never to add (a legacy file they would reinterpret). Nothing missing means
    nothing is written. Before a write the old file is copied to <path>.bak; the
    new one is written atomically. A file that cannot be read or written (absent,
    not a JSON object, a read-only mount) is left alone. Returns the added names.
    """
    say = say or (lambda msg: sys.stderr.write(msg + "\n"))
    if not path or not os.path.isfile(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = f.read()
        have = json.loads(raw)
    except (OSError, ValueError):
        return []
    if not isinstance(have, dict):
        return []
    added = _add_missing(have, {k: v for k, v in defaults.items() if k not in skip})
    if not added:
        return []
    try:
        with open(path + ".bak", "w", encoding="utf-8") as f:
            f.write(raw)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(have, f, indent=2, ensure_ascii=False)
            f.write("\n")
        os.replace(tmp, path)
    except OSError as e:
        say("config %s: could not add new keys (%s): %s" % (path, e, ", ".join(added)))
        return []
    say("config %s: added new keys with their defaults: %s (old file kept as %s.bak)"
        % (path, ", ".join(added), os.path.basename(path)))
    return added


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


def duration(seconds):
    """Seconds as '12s' / '4m 07s' / '1h 02m'.

    Uptime and connection age are the two things an operator reads most, and a raw
    float is useless for both. Shared because both servers' consoles print them.
    """
    seconds = int(max(0, seconds))

    if seconds < 60:
        return "%ds" % seconds

    if seconds < 3600:
        return "%dm %02ds" % (seconds // 60, seconds % 60)

    return "%dh %02dm" % (seconds // 3600, (seconds % 3600) // 60)


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
        """Is there a person on the other end?

        JER_CONSOLE=on/off overrides the guess. Without it a supervisor piping
        commands in (or a test) could never reach the console, and with it the
        guard stays the default: no terminal, no console, one line to say so.
        """
        forced = os.environ.get("JER_CONSOLE", "").strip().lower()

        if forced in ("1", "on", "true", "force"):
            return True

        if forced in ("0", "off", "false", "none"):
            return False

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

