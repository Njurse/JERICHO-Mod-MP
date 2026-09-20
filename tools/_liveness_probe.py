#!/usr/bin/env python3
"""Temporary diagnostic: measure whether a server answers a join liveness-wise.

Usage: _liveness_probe.py <port> [seconds]
  - connects, sends HELLO, waits for WELCOME (+ START if it comes)
  - sends a PING, then waits `seconds` for PONG / any traffic
  - watches for the socket closing (EOF) at any point
Deletes nothing; run against mp_dediserver.py or mp_test.py host --start.
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_test as m

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 1600
WATCH = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(6.0)
    s.connect(("127.0.0.1", PORT))
    print(f"[probe] connected to 127.0.0.1:{PORT}", flush=True)
    m.send_frame(s, m.TAG["hello"], m.build_hello("Probe", [("mp", m.MP_VER)], 0))
    print("[probe] HELLO sent", flush=True)

    saw_welcome = saw_start = False
    deadline = time.time() + 6.0
    while time.time() < deadline and not (saw_welcome and saw_start):
        try:
            tag, payload = m.recv_frame(s)
        except socket.timeout:
            print("[probe] timed out waiting for WELCOME/START", flush=True)
            break
        if tag is None:
            print("[probe] socket closed before WELCOME/START", flush=True)
            return 1
        print(f"[probe] <- {tag!r} ({len(payload)}B)", flush=True)
        if tag == m.TAG["welcome"]:
            saw_welcome = True
        if tag == m.TAG["start"]:
            saw_start = True
        if tag in (m.TAG["ping"], m.TAG["pong"]):
            print(f"[probe]   (keepalive {tag!r})", flush=True)

    if not saw_welcome:
        print("[probe] FAIL: no WELCOME", flush=True)
        return 1

    # Liveness: send a PING and see if the peer ever answers with PONG.
    print(f"[probe] sending PING; watching {WATCH}s for PONG/any traffic", flush=True)
    tick = int(time.time() * 1000) & 0xFFFFFFFF
    m.send_frame(s, m.TAG["ping"], struct.pack("<I", tick))

    got_pong = False
    got_any = False
    eof = False
    end = time.time() + WATCH
    while time.time() < end:
        s.settimeout(max(0.1, end - time.time()))
        try:
            tag, payload = m.recv_frame(s)
        except socket.timeout:
            break
        if tag is None:
            eof = True
            break
        got_any = True
        print(f"[probe] <- {tag!r} ({len(payload)}B)", flush=True)
        if tag == m.TAG["pong"]:
            got_pong = True

    print(f"[probe] RESULT welcome={saw_welcome} start={saw_start} "
          f"pong={got_pong} any_traffic={got_any} eof={eof}", flush=True)
    if eof:
        print("[probe] the server CLOSED the connection during the watch", flush=True)
    elif not got_pong:
        print("[probe] the server never answered PING with PONG (no liveness)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
