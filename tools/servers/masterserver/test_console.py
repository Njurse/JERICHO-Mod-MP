"""The master's console, driven without a terminal.

The table is built by console_commands(registry, stop, log, shutdown), which takes
its dependencies rather than reaching for globals -- so this exercises the real
handlers against a real Registry with no socket bound and no server running.

Run it from the servers directory or from here; it fixes up its own path.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                                   # masterserver + serverkit
sys.path.insert(0, os.path.dirname(HERE))                  # .../tools/servers
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))  # .../tools

import masterserver as ms      # noqa: E402
from serverkit import Console  # noqa: E402


def build(persist_path=""):
    cfg = dict(ms.DEFAULTS)
    cfg.update({"bind": "127.0.0.1", "port": 8100, "persist_path": persist_path})
    log = ms.Log("warning")
    registry = ms.Registry(cfg, log)
    stop = ms.threading.Event()
    holder, commands = ms.console_commands(registry, stop, log)
    console = Console(log, commands)
    holder[0] = console
    return registry, console, stop


def main():
    fails = 0

    def check(name, ok, detail=""):
        nonlocal fails
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))
        fails += 0 if ok else 1

    registry, console, stop = build()

    text = console.help_text()
    check("help lists every command",
          all(n in text for n in ("status", "list", "drop", "persist", "stop", "help")),
          "%d commands" % len(console.order))

    st = console.commands["status"][2]("")
    check("status shows the listen address", "127.0.0.1:8100" in st, st.splitlines()[0])
    check("status says persistence is off", "off (in memory only)" in st)
    check("status counts the entries", "entries:    0/512" in st, st.splitlines()[1])

    # -- two servers register, as the dedi's POST would
    for name, port in (("Alpha", 1400), ("Beta", 1401)):
        registry.register({"name": name, "port": port, "city": 0, "protoVersion": 11,
                           "maxPlayers": 8, "players": 0}, "127.0.0.1")

    check("status counts the entries", "entries:    2/512" in console.commands["status"][2](""))

    listing = console.commands["list"][2]("")
    check("list has a header", "NAME" in listing and "TOKEN" in listing, listing.splitlines()[0])
    check("list shows both servers", "Alpha" in listing and "Beta" in listing)
    check("list shows the address", "127.0.0.1:1400" in listing)

    # -- drop by name, then by token prefix, then a miss
    reply = console.commands["drop"][2]("Alpha")
    check("drop by name works", "dropped 'Alpha'" in reply, reply)
    check("the registry is down to one", registry.count() == 1)

    token = next(iter(registry.by_token))
    reply = console.commands["drop"][2](token[:8])
    check("drop by token prefix works", "dropped 'Beta'" in reply, reply)
    check("the registry is empty", registry.count() == 0)

    reply = console.commands["drop"][2]("nobody")
    check("drop of nothing says so", "nothing matched" in reply, reply)
    check("drop with no argument is refused", "nothing matched" in console.commands["drop"][2](""))

    # -- persist: off, then on with a real file
    check("persist says it has nowhere to write",
          "no persist path configured" in console.commands["persist"][2](""))

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".test_console_registry.json")
    registry2, console2, _stop2 = build(persist_path=path)
    registry2.register({"name": "Gamma", "port": 1402, "city": 0, "protoVersion": 11},
                       "127.0.0.1")
    reply = console2.commands["persist"][2]("")
    check("persist writes the file", "saved 1 entr(ies)" in reply and os.path.exists(path), reply)

    if os.path.exists(path):
        os.unlink(path)

    # -- stop sets the event and asks the server to stop
    console.commands["stop"][2]("")
    # The flag is the whole job: serve()'s watcher thread turns it into
    # httpd.shutdown() (and blocking here instead raced the process exit).
    check("stop sets the stop event", stop.is_set())

    print("\nmaster console: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
