#!/usr/bin/env python3
"""Dedicated LAN server for the JERICHO mp mod -- headless, no game needed.

It accepts players, hands out ids, and relays their car-state so several game
instances can drive around each other without anybody hosting from inside the
game. It also beacons on UDP so the in-game LAN browser lists it.

    python mp_dediserver.py                     # TCP + UDP on 1318
    python mp_dediserver.py --port 1400 --no-beacon

Then point a client at it:

    REDRIVER2_dev.exe -nointro -nofmv -join 127.0.0.1
    REDRIVER2_dev.exe -nointro -nofmv -join 127.0.0.1:1400

The wire format lives in mp_test.py, which is imported so there is only one
copy of it.
"""

import argparse
import os
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_test as m

LOCK = threading.Lock()
CLIENTS = {}        # conn -> {"id": int, "name": str, "addr": str}
CARS = {}           # playerId -> one MP_CARSTATE_ENTRY (m.CARSTATE_ENTRY.size bytes)
CHAT = {}
STARTED = time.time()

MAX_PLAYERS = 8


def log(msg):
    print(f"[dedi] {time.strftime('%H:%M:%S')} {msg}", flush=True)


def free_id():
    for i in range(1, MAX_PLAYERS):
        if all(c["id"] != i for c in CLIENTS.values()):
            return i
    return None


def carstate_union(frame):
    """MP_CARSTATE covering every player we know the position of."""
    with LOCK:
        rows = list(CARS.items())
    hdr = m.CARSTATE.pack(frame & 0xFFFFFFFF, len(rows), 0, 0, 0)
    return hdr + b"".join(e for _, e in rows)


def broadcast(payload, tag, skip=None):
    with LOCK:
        conns = [c for c in CLIENTS if c is not skip]
    for c in conns:
        try:
            m.send_frame(c, tag, payload)
        except OSError:
            pass


def serve(conn, addr, args):
    conn.settimeout(args.timeout)
    who = f"{addr[0]}:{addr[1]}"

    try:
        tag, payload = m.recv_frame(conn)
    except (OSError, socket.timeout):
        conn.close()
        return

    if tag != m.TAG["hello"]:
        log(f"{who}: bad handshake, dropping")
        conn.close()
        return

    try:
        proto, sdk, build, _sp, name32, _mesh, modc, _a, _b = m.HELLO.unpack_from(payload, 0)
    except Exception:
        conn.close()
        return

    name = name32.split(b"\0")[0].decode("latin1", "replace") or "Player"

    with LOCK:
        pid = free_id()
        if pid is None:
            welcome = m.WELCOME.pack(0, MAX_PLAYERS, 0, 0, 1, 0, 0, 0, 0, args.city, 1, 0, 0, 0xFF)
            try:
                m.send_frame(conn, m.TAG["welcome"], welcome)
                m.send_frame(conn, m.TAG["reject"], m.REJECT.pack(1, 0, 0, 0, b"server is full"))
            except OSError:
                pass
            conn.close()
            log("refused a player: full")
            return
        CLIENTS[conn] = {"id": pid, "name": name, "addr": who}

    log(f"player {pid} '{name}' joined from {who} (mods={modc})")

    welcome = m.WELCOME.pack(pid, MAX_PLAYERS, 0, 1, 1, 0, 0, 0, 0, args.city, 1, 0, int(time.time()) & 0xFFFFFFFF, 0xFF)
    sess = m.SESSION.pack(0, args.city, 1, 0, 1234, max(2, len(CLIENTS) + 1), 1, 0)
    try:
        m.send_frame(conn, m.TAG["welcome"], welcome)
        m.send_frame(conn, m.TAG["start"], sess)
    except OSError:
        pass

    # Poll fast so we can keep the link fresh from OUR side too: a dedicated
    # server should not depend on the client to prove the link is alive.
    conn.settimeout(1.0)
    silent = 0

    while True:
        try:
            tag, payload = m.recv_frame(conn)
        except socket.timeout:
            # A second of silence. Send our own keepalive PING and only give the
            # client up after `--timeout` seconds of real silence. Without this a
            # quiet client (a lobby, a load) would be dropped by our own timeout.
            silent += 1
            if silent >= args.timeout:
                break
            try:
                m.send_frame(conn, m.TAG["ping"],
                             struct.pack("<I", int(time.time() * 1000) & 0xFFFFFFFF))
            except OSError:
                break
            continue
        except Exception:
            break

        if tag is None:
            break

        silent = 0

        if tag == m.TAG["carstate"]:
            with LOCK:
                _, count, _a, _b, _c = m.CARSTATE.unpack_from(payload, 0)
                for i in range(count):
                    off = m.CARSTATE.size + i * m.CARSTATE_ENTRY.size
                    e = payload[off:off + m.CARSTATE_ENTRY.size]
                    if len(e) == m.CARSTATE_ENTRY.size:
                        CARS[e[0]] = e
            broadcast(carstate_union(int(time.time() * 30)), m.TAG["carstate"], skip=conn)
        elif tag == m.TAG["chat"]:
            broadcast(payload, m.TAG["chat"], skip=conn)
        elif tag == m.TAG["ping"]:
            # Liveness. A client pings on its keepalive interval and expects the
            # tick echoed back; with no PONG the client's last-receive clock stops
            # advancing and it drops itself after its idle timeout. Echo it.
            try:
                m.send_frame(conn, m.TAG["pong"], payload or b"\x00\x00\x00\x00")
            except OSError:
                break
        elif tag == m.TAG["leave"]:
            break

    with LOCK:
        info = CLIENTS.pop(conn, None)
        if info:
            CARS.pop(info["id"], None)
    log(f"player {info['id'] if info else '?'} '{info['name'] if info else '?'}' left")
    try:
        conn.close()
    except OSError:
        pass


def beacon(args, stop):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    host = (args.name.encode()[:31] + b"\0" * 32)[:32]
    while not stop.is_set():
        with LOCK:
            n = len(CLIENTS)
        pkt = m.BEACON.pack(m.UDP_MAGIC, m.PROTO, args.port, host, n, MAX_PLAYERS,
                            0, 0, args.city, 1, 0, 0)
        try:
            s.sendto(pkt, ("255.255.255.255", args.port))
        except OSError:
            pass
        time.sleep(args.beacon_ms / 1000.0)


def main():
    ap = argparse.ArgumentParser(description="dedicated server for the JERICHO mp mod")
    ap.add_argument("--port", type=int, default=1318)
    ap.add_argument("--city", type=int, default=0, help="GameLevel the clients should load (0..3)")
    ap.add_argument("--name", default="Dedicated", help="name shown in the LAN browser")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--beacon-ms", type=int, default=1000)
    ap.add_argument("--no-beacon", action="store_true", help="do not advertise on UDP")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(MAX_PLAYERS)
    log(f"listening on TCP/{args.port} (city {args.city}); Ctrl-C to stop")

    stop = threading.Event()
    if not args.no_beacon:
        threading.Thread(target=beacon, args=(args, stop), daemon=True).start()

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=serve, args=(conn, addr, args), daemon=True).start()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        stop.set()
        srv.close()


if __name__ == "__main__":
    main()
