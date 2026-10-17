"""Drive both LIVE servers through stdin, the way a supervisor would.

serverkit's console only reads stdin when it believes someone is listening. The
JER_CONSOLE=on override says so when there is no terminal -- a systemd unit with a
pipe, a docker exec -i, or this test -- which is the only way to prove the live
read loop end to end rather than the dispatch function behind it.

Each server is started for real, typed at, and expected to exit by itself after
`stop`. Nothing here needs a game, a network, or a terminal.
"""

import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))          # .../servers


def drive(title, argv, lines, expect, wait=20.0):
    """Start argv, type `lines`, and require every string in `expect` in the output.

    Readiness comes from the child, not from a sleep: a fixed pause made this flaky
    (the first line typed before the console thread was up simply vanished). A reader
    thread collects output and tells us when the console announces itself.
    """
    env = dict(os.environ)
    env["JER_CONSOLE"] = "on"                              # no terminal, but listen

    print("== %s" % title)
    p = subprocess.Popen([sys.executable] + argv, cwd=HERE, env=env,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)

    out = []
    ready = threading.Event()

    def reader():
        for line in p.stdout:
            out.append(line)
            if "console: on" in line:
                ready.set()
        ready.set()                                        # EOF: never became ready

    threading.Thread(target=reader, daemon=True).start()

    if not ready.wait(15.0):
        print("   [FAIL] the console never announced itself")

    for line in lines:
        time.sleep(0.5)                                    # let the previous reply land
        try:
            p.stdin.write(line + "\n")
            p.stdin.flush()
        except OSError:                                    # it already died; the
            break                                          # output will say why

    try:
        p.wait(timeout=wait)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()

    text = "".join(out)
    fails = 0

    for want in expect:
        ok = want in text
        print("   [%s] %r" % ("PASS" if ok else "FAIL", want))
        fails += 0 if ok else 1

    # ...and `stop` should have ended the process by itself
    ended = p.returncode == 0
    print("   [%s] exited on 'stop' (returncode %s)" % ("PASS" if ended else "FAIL", p.returncode))
    fails += 0 if ended else 1

    if fails:
        print("   ---- output ----")
        for line in text.splitlines():
            print("   " + line)

    return fails


def main():
    fails = 0

    fails += drive(
        "dedicated server",
        ["dediserver/dediserver.py", "--port", "1490", "--name", "Stdin Test",
         "--no-beacon", "--master", "", "--log-level", "info"],
        ["help", "status", "list", "say hello", "kick 9", "beacon off", "master", "stop"],
        ["console: on",
         "name, port, city, mode, players, uptime, beacon, master",
         "Stdin Test",
         "no players connected",
         "said to 0 player(s)",
         "no player 9 connected",
         "beacon off",
         "no discovery server configured",
         "stopping (unregisters from the master, closes players)"])

    fails += drive(
        "master",
        ["masterserver/masterserver.py", "--bind", "127.0.0.1", "--port", "8190",
         "--log-level", "info"],
        ["help", "status", "list", "drop nobody", "stop"],
        ["console: on",
         "remove a listing by token or name",
         "listening:  127.0.0.1:8190",
         "no servers listed",
         "nothing matched 'nobody'",
         "stopping..."])

    print("\nscripted stdin: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
