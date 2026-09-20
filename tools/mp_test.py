#!/usr/bin/env python3
"""mp_test.py -- headless protocol harness for the JERICHO Multiplayer module.

The game writes REDRIVER2.log relative to its CWD, so two live instances
cannot run from one folder. Instead this harness drives the real module
against a scriptable Python peer, exercising the actual wire protocol on
whichever side the game is playing:

  host   LISTEN -- accept the game's MP_HELLO and answer MP_WELCOME (so the
                  game acts as the CLIENT).  Verifies the client handshake,
                  HELLO encoding, WELCOME decoding and the player registry.

  client HOST -- connect to the game's listener, send an MP_HELLO and print
                  the reply (so the game acts as the HOST).  --mods sends an
                  empty/garbage manifest; --build forces a version mismatch.

  beacon PORT -- bind UDP PORT and print every MP_BEACON received (verifies
                  the host's discovery advertisement).

Wire format mirrors JERICHO/MODS/mp/mp_proto.h (little-endian, pack(1)).
"""
import argparse
import math
import socket
import struct
import sys
import threading
import time

PROTO = 1
SDK = 1                       # JERICHO_SDK_VERSION
MP_VER = "0.1.0"              # JERICHO/MODS/mp/mod.toml version -- always advertised
NUL = bytes([0])

TAG = {
    "hello": b"JPHL", "welcome": b"JPWL", "reject": b"JPRJ",
    "session": b"JPSS", "start": b"JPST", "input": b"JPIN",
    "carstate": b"JPCS", "ping": b"JPPN", "pong": b"JPPO",
    "channel": b"JPCH", "leave": b"JPLV", "chat": b"JPCX",
    "spawn": b"JPSW", "roster": b"JPRS",
}
UDP_MAGIC = 0x31504D4A

ENV = struct.Struct("<I4sBBH")
HELLO = struct.Struct("<HHHH32s4B")      # 44 bytes
MODI = struct.Struct("<24s16sBB")        # 42 bytes
WELCOME = struct.Struct("<12BIB")        # 17 bytes: 12xu8, u32 seed, u8 hostCar (matches mp_proto.h; NOT <13BI, which shifts seed/hostCar by one)
REJECT = struct.Struct("<4B64s")         # 68 bytes
BEACON = struct.Struct("<IHH32s6BHH")    # 50 bytes
SPAWN = struct.Struct("<4i")             # 16 bytes: x, y, z, headingCHANNEL = struct.Struct("<16sHIBB2B")    # 26 bytes
CHAT = struct.Struct("<B3B96s")          # 100 bytes: playerId, reserved[3], text[96]
SESSION = struct.Struct("<4BIBBH")       # 12 bytes (gamemode,city,tod,weather,seed,numPlayers,state,spare)
INPUT = struct.Struct("<IB3B")           # frame, count, reserved[3] = 8
PLAYER_INPUT = struct.Struct("<BHB")     # playerId, pad, spare = 4
CARSTATE = struct.Struct("<IB3B")        # frame, count, reserved[3] = 8
CARSTATE_ENTRY = struct.Struct("<BBBBB4h3ii3i3i")  # pid, flags, palette, model, carSlot, orient[4], x,y,z, heading, angVel[3], vel[3] = 53 (model 0xFF = on foot; angVel is int32)

REJECT_NAMES = {0: "NONE", 1: "FULL", 2: "VERSION", 3: "MODS", 4: "INPROGRESS", 5: "CUSTOM"}
REJECT_IDS = {v.lower(): k for k, v in REJECT_NAMES.items() if k != 0}


def cstr(raw):
    return raw.split(NUL)[0].decode(errors="replace")


def send_frame(sock, tag, payload=b""):
    sock.sendall(ENV.pack(8 + len(payload), tag, PROTO, 0, 0) + payload)


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_frame(sock):
    head = _recv_exact(sock, ENV.size)
    if head is None:
        return None, None
    length, tag, ver, flags, _ = ENV.unpack(head)
    payload = _recv_exact(sock, length - 8) if length > 8 else b""
    if payload is None:
        return None, None
    return tag, payload


def build_hello(name, mods=(), build=0):
    body = HELLO.pack(PROTO, SDK, build, 0, name.encode()[:31], len(mods), 0, 0, 0)
    for mid, ver in mods:
        body += MODI.pack(mid.encode()[:23], ver.encode()[:15], 1, 0)
    return body


# ---------------------------------------------------------------- modes
def mode_host(args):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(4)
    print(f"[mock-host] listening on {args.port}; waiting for the game's HELLO")

    if args.beacon:
        beacon_start(args.beacon_port, args.beacon_name, args.port,
                     args.beacon_in_progress, args.beacon_interval)

    srv.settimeout(args.timeout)
    try:
        conn, addr = srv.accept()
    except socket.timeout:
        print("[mock-host] FAIL: no connection")
        return 1
    print(f"[mock-host] connection from {addr}")
    conn.settimeout(args.timeout)
    tag, payload = recv_frame(conn)
    if tag != TAG["hello"]:
        print(f"[mock-host] FAIL: expected HELLO, got {tag!r}")
        return 1
    proto, sdk, build, _spare, name, modc, _r0, _r1, _r2 = HELLO.unpack_from(payload, 0)
    mods = [MODI.unpack_from(payload, HELLO.size + i * MODI.size) for i in range(modc)]
    modnames = [cstr(m[0]) for m in mods]
    print(f"[mock-host] HELLO: proto={proto} sdk={sdk} build={build:#06x} name={cstr(name)!r}")
    print(f"[mock-host]         mods({modc}): {', '.join(modnames) or '(none)'}")
    if proto != PROTO or sdk != SDK:
        print("[mock-host] FAIL: protocol/SDK mismatch from the game")
        return 1

    # Stay silent for a while, so the game sits in the handshake and keeps its
    # discovery browser open -- that is the window same-machine discovery needs.
    if args.hold_hello > 0:
        print(f"[mock-host] holding the handshake {args.hold_hello}s (staying silent)")
        time.sleep(args.hold_hello)

    # Mimic the real host's reject path: send MP_REJECT, then close. --reject-hold
    # keeps the socket open first (isolates "the REJECT was lost" from "the close
    # raced the REJECT"); --reject-graceful half-closes and drains instead of
    # slamming the socket shut (isolates the RST race).
    if args.reject and args.reject != "none":
        rid = REJECT_IDS[args.reject]
        text = args.reject_text or f"mock reject: {args.reject}"
        send_frame(conn, TAG["reject"], REJECT.pack(rid, 0, 0, 0, text.encode()[:63]))
        print(f"[mock-host] sent REJECT {args.reject.upper()} ({rid}) -- {text!r}")
        if args.reject_hold > 0:
            time.sleep(args.reject_hold)
        if args.reject_graceful:
            try:
                conn.shutdown(socket.SHUT_WR)
                conn.settimeout(2.0)
                while True:
                    if not conn.recv(4096):
                        break
            except OSError:
                pass
        conn.close()
        return 0
    welcome = WELCOME.pack(1, 8, 0, 1, 1 if args.start else 0, 0, 0, 0, 0, args.city, 1, 0, 1234, 0xFF)
    send_frame(conn, TAG["welcome"], welcome)
    print("[mock-host] sent WELCOME (playerId=1) -- PASS")

    if args.start:
        sess = SESSION.pack(0, args.city, 1, 0, 1234, 2, 1, 0)
        send_frame(conn, TAG["start"], sess)
        print(f"[mock-host] sent START (city={args.city})")

    # Service the connection. A real host does NOT hang up the moment it has said
    # hello: it answers keepalives and stays up. Closing 0.5 s after WELCOME is
    # exactly why a joining client logged "Lost the server" seconds into an
    # otherwise clean handshake. So: always reply to PING with PONG, service
    # input/car-state with --lockstep, and stay up until the client goes away
    # (or --hold seconds pass).
    conn.settimeout(1.0)
    seen = 0
    carstate = 0
    last = None
    deadline = time.time() + args.hold if args.hold > 0 else None
    while deadline is None or time.time() < deadline:
        try:
            tag, payload = recv_frame(conn)
        except socket.timeout:
            continue
        except OSError:
            break
        if tag is None:
            print("[mock-host] client closed the connection")
            break
        if tag == TAG["ping"]:
            send_frame(conn, TAG["pong"], payload if payload else b"\0\0\0\0")
        elif tag == TAG["input"] and args.lockstep:
            frame, count, _a, _b, _c = INPUT.unpack_from(payload, 0)
            rows = [PLAYER_INPUT.unpack_from(payload, INPUT.size + i * PLAYER_INPUT.size) for i in range(count)]
            peer_pad = rows[0][1] if rows else 0
            out = INPUT.pack(frame, 2, 0, 0, 0)
            out += PLAYER_INPUT.pack(0, args.peer_pad, 0)   # the fake host player's own pad
            out += PLAYER_INPUT.pack(1, peer_pad, 0)       # echo the client's own pad (player 1)
            send_frame(conn, TAG["input"], out)
            seen += 1
        elif tag == TAG["carstate"] and args.lockstep and not args.no_carstate:
            frame, count, _a, _b, _c = CARSTATE.unpack_from(payload, 0)
            if count >= 1:
                pid, _fl, _pal, _md, _sl, _o0, _o1, _o2, _o3, x, y, z, hd, _a0, _a1, _a2, _v0, _v1, _v2 = CARSTATE_ENTRY.unpack_from(payload, CARSTATE.size)
                last = (x, y, z, hd)
                if not hasattr(args, "_fixed") or args._fixed is None:
                    args._fixed = (x + args.peer_dist, y, z)
                    print(f"[mock-host] pinning 'host' car at {args._fixed} "
                          f"(client spawn {x},{y},{z}, {args.peer_dist} away)")
                    # a real host hands out its own car's position as the meeting
                    # point; line everybody up where the client already is
                    if args.start:
                        send_frame(conn, TAG["spawn"], SPAWN.pack(x, y, z, 0))
                        print(f"[mock-host] sent SPAWN meeting point {x},{y},{z}")
                fx, fy, fz = args._fixed
                # static "host" car: it must NOT follow the client's car
                cs = CARSTATE.pack(frame, 1, 0, 0, 0)
                cs += CARSTATE_ENTRY.pack(0, 1, 0, 0, 0xFF, 0, 0, 0, 0, fx, fy, fz, 0, 0, 0, 0, 0, 0, 0)
                send_frame(conn, TAG["carstate"], cs)
                carstate += 1
    print(f"[mock-host] served {seen} input / {carstate} car-state frame(s); last client car {last}")
    conn.close()
    return 0


def beacon_start(port, name, session_port, in_progress, interval):
    """Broadcast MP_BEACON like a real host, in a daemon thread.

    Deliberately NOT bound to the port: sending needs no bind, so a mock host
    can advertise while a real game listens on the same machine.
    """
    def run():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        body = BEACON.pack(UDP_MAGIC, PROTO, session_port,
                           name.encode()[:31], 1, 8, 0, 0, 0, in_progress, 0x1234, 0)
        n = 0
        while True:
            try:
                s.sendto(body, ("255.255.255.255", port))
                n += 1
                if n == 1:
                    print(f"[mock-host] beaconing '{name}' on UDP/{port} (session {session_port})")
            except OSError as e:
                print(f"[mock-host] beacon failed: {e}")
            time.sleep(interval)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def mode_client(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(args.timeout)
    s.connect((args.host, args.port))
    print(f"[mock-client] connected to {args.host}:{args.port}")

    # The mp mod is ALWAYS advertised: it *is* the multiplayer system and the
    # game side always has it enabled (JERICHO lists every enabled module in
    # its HELLO). --mods adds a SECOND entry so a deliberate mismatch can still
    # be exercised; it never removes mp.
    mods = [("mp", MP_VER)]
    if args.mods not in ("empty", "mp"):
        mods.append((args.mods, "1.0.0"))
    send_frame(s, TAG["hello"], build_hello(args.name, mods, args.build))
    print(f"[mock-client] sent HELLO (name={args.name}, mods={[m[0] for m in mods]}, build={args.build:#06x})")

    # The host pings on a keepalive timer, and it can ping before it has even
    # answered us, so skip pings/pongs while waiting for the actual reply.
    while True:
        tag, payload = recv_frame(s)
        if tag is None or tag not in (TAG["ping"], TAG["pong"]):
            break

    if tag == TAG["welcome"]:
        pid, maxp, enforce, matched, running, subgame, _r1, _r2, gm, city, tod, wx, seed, hostcar = WELCOME.unpack_from(payload, 0)
        print(f"[mock-client] WELCOME: playerId={pid} max={maxp} enforced={enforce} "
              f"matched={matched} running={running} subgame={subgame} gamemode={gm} city={city} tod={tod} weather={wx} seed={seed}")
        if args.channel:
            body = b"hello-from-python"
            hdr = CHANNEL.pack(args.channel.encode()[:15], len(body), 0, 0, 1, 0, 0)
            send_frame(s, TAG["channel"], hdr + body)
            print(f"[mock-client] sent CHANNEL '{args.channel}' ({len(body)} bytes)")
            time.sleep(0.5)
        if args.drive:
            # wait for the host's START, then stream our (moving) car state
            deadline = time.time() + 15
            started = False
            while time.time() < deadline and not started:
                t, pl = recv_frame(s)
                if t is None:
                    break
                if t == TAG["start"]:
                    started = True
            if started:
                # wait for the host's own car-state so we spawn right next to it
                # (the host only starts broadcasting once ITS level has loaded,
                # which can take ~15 s, hence the generous deadline)
                hx, hz, hy = None, None, 30
                t_end = time.time() + 30
                while time.time() < t_end and hx is None:
                    try:
                        s.settimeout(0.5)
                        t, pl = recv_frame(s)
                    except socket.timeout:
                        continue        # not started broadcasting yet - keep waiting
                    except OSError:
                        break
                    if t is None:
                        break
                    if t == TAG["carstate"]:
                        _fr, _cnt, _a, _b, _c = CARSTATE.unpack_from(pl, 0)
                        for k in range(_cnt):
                            pk, _fl, _pal, _md, _sl, _o0, _o1, _o2, _o3, xk, yk, zk, hk, _a0, _a1, _a2, _v0, _v1, _v2 = CARSTATE_ENTRY.unpack_from(
                                pl, CARSTATE.size + k * CARSTATE_ENTRY.size)
                            if pk == 0:
                                hx, hz, hy = xk, zk, yk
                                break
                if hx is None:
                    hx, hz = -258443, -236292
                # tight grid right on the vanilla spawn (where the host's car
                # starts) so every car is visible together
                x = hx + (pid % 4) * 220 - 330
                lane = hz + (pid // 4) * 220 - 220
                print(f"[mock-client] spawning at {x},{lane} (vanilla spawn {hx},{hz})")
                drift = (pid - 4) * 0.45      # every client drives off in its own direction
                dxs = int(round(math.cos(drift) * 25))
                dzs = int(round(math.sin(drift) * 25))
                for f in range(900):
                    x += dxs
                    cs = CARSTATE.pack(f, 1, 0, 0, 0)
                    cs += CARSTATE_ENTRY.pack(pid, 1, 0, 0, 0xFF, 0, 0, 0, 0, x, hy, lane, 0, 0, 0, 0, 0, 0, 0)   # pid = us, at the host's ground height
                    send_frame(s, TAG["carstate"], cs)
                    lane += dzs
                    try:
                        s.settimeout(0.03)
                        while True:
                            recv_frame(s)
                    except (socket.timeout, OSError):
                        pass
                    time.sleep(1.0 / 30)
                    if f % 120 == 0:
                        print(f"[mock-client] driving: f={f} x={x}")
                print(f"[mock-client] streamed 900 car-state frames, final x={x}")
            else:
                print("[mock-client] no START received (host never started)")

        if args.chat:
            payload = CHAT.pack(1, 0, 0, 0, args.chat.encode()[:95])
            send_frame(s, TAG["chat"], payload)
            print(f"[mock-client] sent CHAT {args.chat!r}")
            time.sleep(0.6)

        print("[mock-client] PASS" if pid == 1 else "[mock-client] FAIL: unexpected playerId")
        s.close()
        return 0 if pid == 1 else 1
    if tag == TAG["reject"]:
        reason, _a, _b, _c, text = REJECT.unpack_from(payload, 0)
        print(f"[mock-client] REJECT: {REJECT_NAMES.get(reason, reason)} -- {cstr(text)!r}")
        print("[mock-client] PASS")
        s.close()
        return 0
    print(f"[mock-client] FAIL: unexpected reply {tag!r}")
    return 1


def mode_beacon(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("0.0.0.0", args.port))
    s.settimeout(args.timeout)
    print(f"[beacon] listening on UDP/{args.port} for host beacons")
    seen = 0
    t0 = time.time()
    while time.time() - t0 < args.duration:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < BEACON.size:
            continue
        magic, proto, port, host, players, maxp, gm, enforce, city, inprog, mhash, _ = BEACON.unpack_from(data, 0)
        if magic != UDP_MAGIC:
            continue
        print(f"[beacon] from {addr[0]}: proto={proto} port={port} "
              f"host={cstr(host)!r} players={players}/{maxp} "
              f"mode={gm} city={city} enforced={enforce} inProgress={inprog} modHash={mhash:#06x}")
        seen += 1
    print(f"[beacon] {'PASS' if seen else 'FAIL'}: {seen} beacon(s)")
    return 0 if seen else 1


def main():
    ap = argparse.ArgumentParser(description="JERICHO mp protocol harness")
    sub = ap.add_subparsers(dest="mode", required=True)

    h = sub.add_parser("host", help="mock host: accept the game's HELLO")
    h.add_argument("--port", type=int, default=1318)
    h.add_argument("--timeout", type=float, default=15.0)
    h.add_argument("--city", type=int, default=1)
    h.add_argument("--start", action="store_true", help="after WELCOME, send MP_START to launch a level")
    h.add_argument("--lockstep", action="store_true", help="after START, service the client's MP_INPUT frames")
    h.add_argument("--drive", action="store_true", help="after START, stream a moving MP_CARSTATE for player 1")
    h.add_argument("--reject", default="none", choices=["none"] + list(REJECT_IDS),
                   help="send MP_REJECT then close, exactly like the real host does")
    h.add_argument("--reject-text", default="", help="text to carry in the REJECT")
    h.add_argument("--reject-hold", type=float, default=0.0,
                   help="seconds to keep the socket open after the REJECT (0 = close at once, like the real host)")
    h.add_argument("--reject-graceful", action="store_true",
                   help="half-close and drain after the REJECT instead of closing the socket")
    h.add_argument("--peer-dist", type=int, default=260,
                   help="how far the fake host car sits from the client's (default just in view)")
    h.add_argument("--no-carstate", action="store_true",
                   help="ignore the client's car-state, so only replicated input moves the cars")
    h.add_argument("--peer-pad", type=lambda v: int(v, 0), default=0,
                   help="pad the fake host player holds, in engine mapped bits (e.g. 0x40 = accelerate)")
    h.add_argument("--hold-hello", type=float, default=0.0,
                   help="seconds to stay silent after HELLO, stretching the client's handshake")
    h.add_argument("--beacon", action="store_true", help="broadcast discovery beacons while hosting")
    h.add_argument("--beacon-port", type=int, default=1318, help="discovery port to beacon on")
    h.add_argument("--beacon-name", default="MockBob's game", help="advertised host name")
    h.add_argument("--beacon-in-progress", type=int, default=0, help="1 = advertise as a live match")
    h.add_argument("--beacon-interval", type=float, default=1.0)
    h.add_argument("--hold", type=float, default=0.0,
                   help="seconds to keep the connection up after the handshake "
                        "(0 = until the client disconnects). A real host does not "
                        "close after saying hello; closing early made a client see "
                        "'Lost the server' right after a clean join.")
    h.set_defaults(func=mode_host)

    c = sub.add_parser("client", help="mock client: connect to the game's host")
    c.add_argument("host")
    c.add_argument("--name", default="PyTester", help="player name announced in HELLO")
    c.add_argument("--drive", action="store_true", help="after START, stream a moving MP_CARSTATE for player 1")
    c.add_argument("--chat", default="", help="after WELCOME, send one MP_CHAT line")
    c.add_argument("--port", type=int, default=1318)
    c.add_argument("--timeout", type=float, default=10.0)
    c.add_argument("--mods", default="empty", help="extra mod id to declare; 'mp' is ALWAYS included")
    c.add_argument("--build", type=lambda v: int(v, 0), default=0x74cc,
                   help="game-build hash to send (default = current build; pass 0 to force a mismatch)")
    c.add_argument("--channel", default=None, help="after WELCOME, send an MP_CHANNEL payload on this channel name")
    c.set_defaults(func=mode_client)

    b = sub.add_parser("beacon", help="UDP listener for host beacons")
    b.add_argument("--port", type=int, default=1318)
    b.add_argument("--timeout", type=float, default=2.0)
    b.add_argument("--duration", type=float, default=6.0)
    b.set_defaults(func=mode_beacon)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
