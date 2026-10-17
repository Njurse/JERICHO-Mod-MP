"""The master's reachability prober, tested against real sockets.

An entry that heartbeats while nothing listens on its session port is the failure
this exists for, so the test uses real sockets: one port it listens on, one it does
not. It also checks that the prober says so ONCE when the answer changes, rather
than every sweep -- a warning per sweep is a warning nobody reads.

Run from the servers directory or from here; it fixes up its own path.
"""

import os
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                                   # masterserver + serverkit
sys.path.insert(0, os.path.dirname(HERE))                  # .../tools/servers

import masterserver as ms      # noqa: E402


def main():
    fails = 0

    def check(name, ok, detail=""):
        nonlocal fails
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))
        fails += 0 if ok else 1

    # a port that really answers
    live = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    live.bind(("127.0.0.1", 0))
    live.listen(4)
    live_port = live.getsockname()[1]

    # ...and one that does not: bind then close, so the port is free and closed
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    dead_port = probe.getsockname()[1]
    probe.close()

    cfg = dict(ms.DEFAULTS)
    cfg.update({"probe_s": 1, "ttl_s": 60})
    log = ms.Log("info")
    registry = ms.Registry(cfg, log)
    stop = threading.Event()

    for name, port in (("Live Row", live_port), ("Shut Row", dead_port)):
        registry.register({"name": name, "port": port, "city": 0, "protoVersion": 11},
                          "127.0.0.1")

    check("nothing has been probed yet",
          all(e.reachable is None for e in registry.by_key.values()))
    check("both entries want a probe", len(registry.needs_probe(5)) == 2)

    t = threading.Thread(target=ms.prober, args=(registry, stop, log), daemon=True)
    t.start()

    deadline = time.time() + 12.0
    while time.time() < deadline:
        rows = {e.name: e for e in registry.by_key.values()}
        if all(e.reachable is not None for e in rows.values()):
            break
        time.sleep(0.2)

    stop.set()
    t.join(timeout=3.0)
    live.close()

    rows = {e.name: e for e in registry.by_key.values()}

    check("the live row is reachable", rows.get("Live Row") is not None
          and rows["Live Row"].reachable is True)
    check("the live row reports a round trip", rows.get("Live Row") is not None
          and rows["Live Row"].probeMs >= 1, "probeMs=%s" % (rows.get("Live Row").probeMs
                                                            if rows.get("Live Row") else None))
    check("the shut row is NOT reachable", rows.get("Shut Row") is not None
          and rows["Shut Row"].reachable is False)

    listed = {s["name"]: s for s in [e.public() for e in registry.by_key.values()]}
    check("the listing carries reachable", listed["Live Row"]["reachable"] is True
          and listed["Shut Row"]["reachable"] is False)
    check("the listing carries probeMs", listed["Live Row"]["probeMs"] >= 1)

    # a fresh probe of an unchanged answer is not a transition, so nothing is logged
    check("no transition for an unchanged answer",
          registry.set_probe(next(k for k, e in registry.by_key.items()
                                  if e.name == "Shut Row"), False, 0) is None)
    check("a change of answer IS a transition",
          registry.set_probe(next(k for k, e in registry.by_key.items()
                                  if e.name == "Shut Row"), True, 3) is not None)

    # reachability must not be persisted: a stale "down" would greet a restart as a lie
    check("reachability is not persisted",
          "reachable" not in rows["Live Row"].persist()
          and "probeAt" not in rows["Live Row"].persist())

    print("\nmaster probe: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
