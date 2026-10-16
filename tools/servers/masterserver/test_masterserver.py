#!/usr/bin/env python3
"""test_masterserver.py -- a self-contained test for masterserver.py.

Loopback only: no game, no assets, no third-party packages. It starts a real
master process on an ephemeral port with a short TTL and drives the real HTTP
API -- register, list, heartbeat, TTL expiry, unregister, the error shapes, and
the fact that the public address is derived from the connection (not the body).

    python test_masterserver.py

Exit 0 = every check passed, 1 = a failure (each one is printed).
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "masterserver.py")
TTL = 3                      # short, so expiry is cheap to test
SLEEP = TTL + 1.5

failures = []


def check(name, cond, detail=""):
    mark = "PASS" if cond else "FAIL"
    print("[%s] %s%s" % (mark, name, (" -- " + detail) if detail and not cond else ""))
    if not cond:
        failures.append(name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def call(base, path, body=None, raw=None):
    """Return (status, decoded-json-or-None). raw= overrides the encoded body."""
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(
        base + path, data=data, headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            payload = r.read().decode("utf-8")
            try:
                return r.status, json.loads(payload)
            except ValueError:
                return r.status, None
    except urllib.error.HTTPError as e:
        payload = e.read().decode("utf-8")
        try:
            return e.code, json.loads(payload)
        except ValueError:
            return e.code, None


def wait_up(base, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        try:
            st, _ = call(base, "/v1/health")
            if st == 200:
                return True
        except OSError:
            pass
        time.sleep(0.2)
    return False


def main():
    port = free_port()
    base = "http://127.0.0.1:%d" % port
    proc = subprocess.Popen(
        [sys.executable, SERVER, "--bind", "127.0.0.1", "--port", str(port),
         "--ttl", str(TTL), "--sweep", "1", "--log-level", "warning"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        check("server answers /v1/health", wait_up(base), "no health response")
        if failures:
            return 1

        st, body = call(base, "/v1/health")
        check("health shape", st == 200 and body.get("version") == 1 and "uptimeSeconds" in body)

        st, body = call(base, "/v1/servers")
        check("empty list is count 0", st == 200 and body["count"] == 0 and body["servers"] == [])

        # -- register
        st, reg = call(base, "/v1/register",
                       {"name": "Bob's Game", "port": 1318, "city": 1,
                        "maxPlayers": 8, "players": 0, "protoVersion": 11})
        check("register -> 201", st == 201, str(st))
        token = reg.get("token")
        check("register returns a token", bool(token))
        check("register derives the public host from the peer", reg.get("host") == "127.0.0.1",
              str(reg.get("host")))
        check("register echoes the session port", reg.get("port") == 1318)
        check("register returns the ttl", reg.get("ttl") == TTL)

        # -- list shows it
        st, body = call(base, "/v1/servers")
        check("list shows the host", st == 200 and body["count"] == 1, json.dumps(body))
        row = body["servers"][0]
        check("listed row matches", row["name"] == "Bob's Game" and row["host"] == "127.0.0.1"
              and row["port"] == 1318 and row["city"] == 1)
        check("row has ageSeconds", "ageSeconds" in row)

        # -- heartbeat refreshes
        st, hb = call(base, "/v1/heartbeat", {"token": token, "players": 3, "inProgress": 1})
        check("heartbeat -> 200", st == 200, str(st))
        check("heartbeat reports expiresIn", hb.get("expiresIn", 0) > 0)
        st, body = call(base, "/v1/servers")
        check("heartbeat updated the counts", body["servers"][0]["players"] == 3
              and body["servers"][0]["inProgress"] == 1)

        # -- error shapes
        st, body = call(base, "/v1/heartbeat", {"token": "nope"})
        check("unknown token -> 404", st == 404 and body.get("error") == "unknown_token", str(st))
        st, body = call(base, "/v1/register", {"name": "x" * 40, "port": 1318})
        check("over-long name -> 400", st == 400, str(st))
        st, body = call(base, "/v1/register", {"name": "ok"})
        check("missing port -> 400", st == 400, str(st))
        st, body = call(base, "/v1/register", raw=b"{not json")
        check("bad json -> 400", st == 400, str(st))
        st, body = call(base, "/v1/servers", raw=b"x" * 8192)
        check("over-long body -> 413", st == 413, str(st))
        st, body = call(base, "/v1/nope")
        check("unknown path -> 404", st == 404, str(st))

        # -- TTL expiry (the host stops heartbeating)
        print("       waiting %.1fs for the %ss TTL to lapse..." % (SLEEP, TTL))
        time.sleep(SLEEP)
        st, body = call(base, "/v1/servers")
        check("an un-heartbeated host expires", body["count"] == 0, json.dumps(body))
        st, body = call(base, "/v1/heartbeat", {"token": token})
        check("an expired token -> 404", st == 404, str(st))

        # -- unregister is immediate and idempotent
        st, reg = call(base, "/v1/register", {"name": "Temp", "port": 2000})
        tok2 = reg["token"]
        st, body = call(base, "/v1/unregister", {"token": tok2})
        check("unregister -> 200", st == 200 and body.get("ok") is True)
        st, body = call(base, "/v1/servers")
        check("unregister removes the host at once", body["count"] == 0, json.dumps(body))
        st, body = call(base, "/v1/unregister", {"token": tok2})
        check("unregister is idempotent", st == 200, str(st))

        # -- re-register replaces (a new token is issued)
        st, r1 = call(base, "/v1/register", {"name": "Same", "port": 3000})
        st, r2 = call(base, "/v1/register", {"name": "Same", "port": 3000})
        check("re-register issues a fresh token", r1["token"] != r2["token"])
        st, old = call(base, "/v1/heartbeat", {"token": r1["token"]})
        check("the replaced token is dead", st == 404, str(st))
        st, body = call(base, "/v1/servers")
        check("re-register does not duplicate the row", body["count"] == 1, json.dumps(body))

        # -- the public address is derived from the connection, but an explicit
        #    advertiseHost overrides it (a host behind a proxy / with public DNS)
        st, dns = call(base, "/v1/register",
                       {"name": "DNS", "port": 4000, "advertiseHost": "game.example.com"})
        check("advertiseHost overrides the derived address",
              dns.get("host") == "game.example.com", str(dns.get("host")))
        st, body = call(base, "/v1/servers")
        check("both hosts are listed", body["count"] == 2, json.dumps(body))
        call(base, "/v1/unregister", {"token": dns["token"]})

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print()
    if failures:
        print("test_masterserver: FAIL (%d) -- %s" % (len(failures), ", ".join(failures)))
        return 1
    print("test_masterserver: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
