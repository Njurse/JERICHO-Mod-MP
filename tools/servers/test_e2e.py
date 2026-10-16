#!/usr/bin/env python3
"""test_e2e.py -- end-to-end test: master discovery server + dedicated server + a client.

Loopback only: no game, no assets, no third-party packages. It starts the real
master (../masterserver/masterserver.py) and a real dediserver registered to it,
then checks the whole path the internet-play plan describes:

  * the dedi appears in the master's listing -- it registered itself;
  * its heartbeats keep it alive past the master's TTL (a silent host would lapse);
  * a client completes the handshake with the dedi (mp_test.py's mock client);
  * if the master restarts and forgets it, the dedi notices and re-registers;
  * a clean shutdown unregisters it at once, instead of waiting for the TTL.

    python test_e2e.py

Exit 0 = every check passed, 1 = a failure (each is printed).
"""
from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEDI = os.path.join(HERE, "dediserver", "dediserver.py")
MASTER = os.path.join(HERE, "masterserver", "masterserver.py")
MOCK = os.path.join(os.path.dirname(HERE), "mp_test.py")   # tools/mp_test.py

MASTER_TTL = 6
HEARTBEAT = 2

failures = []


def check(name, cond, detail=""):
    print("[%s] %s%s" % ("PASS" if cond else "FAIL", name, (" -- " + detail) if detail and not cond else ""))
    if not cond:
        failures.append(name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def listing(base):
    try:
        with urllib.request.urlopen(base + "/v1/servers", timeout=3) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError):
        return None


def wait_count(base, want, timeout=15):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        d = listing(base)
        last = None if d is None else d["count"]
        if last == want:
            return True
        time.sleep(0.3)
    return False


def start_master(port):
    return subprocess.Popen(
        [sys.executable, MASTER, "--bind", "127.0.0.1", "--port", str(port),
         "--ttl", str(MASTER_TTL), "--sweep", "1", "--log-level", "warning"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def stop(proc, sig=None):
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.send_signal(sig if sig is not None else signal.SIGTERM)
    except (OSError, ValueError):
        proc.terminate()
    try:
        proc.wait(timeout=6)
    except subprocess.TimeoutExpired:
        proc.kill()


def main():
    mp = free_port()
    dp = free_port()
    base = "http://127.0.0.1:%d" % mp
    master = None
    dedi = None
    tail = ""
    try:
        master = start_master(mp)
        check("master is up", wait_count(base, 0) is True)

        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        dedi = subprocess.Popen(
            [sys.executable, DEDI, "--port", str(dp), "--no-beacon", "--name", "E2E Host",
             "--master", base, "--master-interval", str(HEARTBEAT),
             "--idle-drop", "30", "--log-level", "info"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=flags,
        )

        check("the dedi registers (appears in the master listing)", wait_count(base, 1),
              "no registration within 15s")
        if failures:
            return 1
        row = listing(base)["servers"][0]
        check("the registered row is right", row["name"] == "E2E Host" and row["port"] == dp
              and row["host"] == "127.0.0.1", json.dumps(row))

        # -- heartbeats keep it alive across the TTL
        print("       waiting %ds (longer than the %ds TTL) to prove heartbeats..." % (MASTER_TTL + 3, MASTER_TTL))
        time.sleep(MASTER_TTL + 3)
        check("heartbeats keep the dedi listed past the TTL", listing(base)["count"] == 1)

        # -- a client can join the dedi
        r = subprocess.run(
            [sys.executable, MOCK, "client", "127.0.0.1", "--port", str(dp),
             "--name", "E2E Tester", "--timeout", "8"],
            capture_output=True, text=True, timeout=30,
        )
        check("a client completes the handshake with the dedi", r.returncode == 0,
              (r.stdout + r.stderr).strip()[-300:])
        check("the client got playerId 1", "playerId=1" in r.stdout, r.stdout.strip()[-200:])

        # -- a restarted master forgets the dedi; it must re-register
        stop(master)
        master = start_master(mp)
        check("the dedi re-registers with the new master", wait_count(base, 1, timeout=20),
              "no re-registration after the master restart")

        # -- a clean shutdown unregisters at once
        sig = signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM
        try:
            dedi.send_signal(sig)
        except (OSError, ValueError):
            stop(dedi)
        try:
            dedi.wait(timeout=8)
            check("the dedi exits cleanly on the shutdown signal", dedi.returncode == 0,
                  "returncode %s" % dedi.returncode)
        except subprocess.TimeoutExpired:
            check("the dedi exits cleanly on the shutdown signal", False, "did not exit")

        check("a clean shutdown unregisters at once (well inside the TTL)",
              wait_count(base, 0, timeout=5), "still listed after a clean stop")
    finally:
        stop(dedi)
        if dedi is not None and dedi.stdout is not None:
            tail = dedi.stdout.read() or ""
        stop(master)
        if tail:
            print("--- dedi log tail ---\n" + tail.strip()[-800:])

    print()
    if failures:
        print("test_e2e: FAIL (%d) -- %s" % (len(failures), ", ".join(failures)))
        return 1
    print("test_e2e: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
