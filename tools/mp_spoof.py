#!/usr/bin/env python3
"""mp_spoof.py -- the peer-trust regression probe.

A session's message handlers name players by id, and an id on the wire is just a
number. Three of them used to believe the number:

  MP_INPUT   a row naming another player was stored and republished as that
             player's own input (the fallback pad their car coasts on),
  MP_COLOR   a peer could repaint somebody else on every screen,
  MP_CHANNEL a peer chose the channel name and the length outright.

Each is now bound to the CONNECTION the bytes arrived on, so this probe plays a
well-behaved client and then sends exactly what a hostile one would, and reads
the host's own log to see which of it was accepted.

    python tools/mp_spoof.py --port 1318 --log <game dir>/JERICHO.log

Exit codes: 0 = every expectation met, 1 = a check failed, 2 = could not run
(no host listening / no WELCOME).

The frames are built with mp_test.py's structs rather than a second copy of the
protocol, so a wire change cannot leave this probe quietly testing the old one.
"""
import argparse
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mp_test as proto  # noqa: E402  (the wire mirror; import is side-effect free)

# A pad value a human cannot easily press, so the host's log line is unambiguous.
FAKE_PAD = 0xF0F0


def connect(host, port, name, timeout):
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    proto.send_frame(s, proto.TAG["hello"], proto.build_hello(name, mods=[("mp", proto.MP_VER)]))
    return s


def wait_welcome(sock, timeout):
    """Read frames until WELCOME; returns the assigned player id, or None."""
    deadline = time.time() + timeout

    while time.time() < deadline:
        tag, payload = proto.recv_frame(sock)

        if tag is None:
            return None

        if tag == proto.TAG["welcome"]:
            fields = proto.WELCOME.unpack(payload)

            # WELCOME is 12 x u8 then seed/hostCar/rules; the player id is the first byte.
            return fields[0]

        if tag == proto.TAG["reject"]:
            print("[spoof] host rejected the join: %r" % (payload,))
            return None

    return None


def send_input(sock, rows):
    body = proto.INPUT.pack(len(rows), 0, 0, 0)

    for player_id, pad in rows:
        body += proto.PLAYER_INPUT.pack(player_id, pad, 0)

    proto.send_frame(sock, proto.TAG["input"], body)


def send_channel(sock, name, payload):
    body = proto.CHANNEL.pack(name.encode()[:15], len(payload), 0, 1, 0, 0, 0)
    proto.send_frame(sock, proto.TAG["channel"], body + payload)


def new_log_text(path, offset):
    try:
        with open(path, "rb") as f:
            f.seek(offset)
            return f.read().decode(errors="replace")
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1318)
    ap.add_argument("--log", default=None, help="the host's JERICHO.log (enables the assertions)")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--settle", type=float, default=1.5, help="seconds to let the host log")
    a = ap.parse_args()

    offset = 0

    if a.log and os.path.exists(a.log):
        offset = os.path.getsize(a.log)

    sock = connect(a.host, a.port, "SpoofProbe", a.timeout)
    my_id = wait_welcome(sock, a.timeout)

    if my_id is None:
        print("[spoof] no WELCOME -- is a host listening on %d?" % a.port)
        return 2

    print("[spoof] joined as player %d" % my_id)

    victim = 0 if my_id != 0 else 1

    # 1) A row naming somebody ELSE: the spoof.
    send_input(sock, [(victim, FAKE_PAD)])
    time.sleep(0.3)

    # 2) A row naming OURSELVES: the ordinary case, which must still be accepted.
    send_input(sock, [(my_id, 0x0001)])
    time.sleep(0.3)

    # 3) A channel nobody registered, then one that declares more payload than
    #    jer_net_send can ever produce (its ceiling is MP_CHANNEL_MAX_PAYLOAD).
    send_channel(sock, "notregistered", b"hello")
    send_channel(sock, "notregistered", b"x" * 2000)
    time.sleep(a.settle)

    try:
        sock.close()
    except OSError:
        pass

    if not a.log:
        print("[spoof] frames sent; pass --log to assert on the host's own words")
        return 0

    text = new_log_text(a.log, offset)

    if not text.strip():
        print("[spoof] FAIL: the host logged nothing -- is %s the right log?" % a.log)
        return 1

    checks = [
        ("spoofed input row dropped",
         "input: dropped a row from player %d claiming player %d" % (my_id, victim), True),
        ("own input row accepted",
         "input: dropped a row from player %d claiming player %d" % (my_id, my_id), False),
        ("unregistered channel dropped",
         "channel: dropped 'notregistered'", True),
    ]

    failed = 0

    for label, needle, want in checks:
        found = needle in text

        if found == want:
            print("[spoof] ok   %s" % label)
        else:
            print("[spoof] FAIL %s (%s %r)" % (label, "missing" if want else "unexpected", needle))
            failed += 1

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
