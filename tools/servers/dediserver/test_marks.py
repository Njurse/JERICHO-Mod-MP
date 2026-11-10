"""Fox N Hounds and Car Bomb Tag on the dedicated server (MP_TAG_MARK, proto 23),
over real sockets: the server referees from the carstates it keeps.

  * a timer mode does not start round 1 at boot; one player waits, nothing ticks;
  * the pre-game wait (t515/t516): a new map (boot, or everyone gone as after a
    map vote) sits in WAITING until the mode's wait is satisfied, then one
    first-load 5-4-3-2-1; the next round on the same map uses the shorter count;
  * the round start (t508): two present but a car not picked -> nothing starts; every
    car in -> COUNTDOWN (no clock, no tag, no round yet), then GO: round 1 live, one of
    the two marked, JPMK sent; a joiner without a car aborts a countdown; the
    intermission leads into the same wait -> countdown -> GO;
  * a carstate contact with the marked car moves the mark (the position path), the
    same bump cannot bounce it straight back, far apart or slow is no tag, and a
    spawn bump (inside the arm) is no tag;
  * a client's JPHI is the fallback claim, accepted only when the kept cars agree;
  * a client cannot write the mark state: its JPMK is answered, never forwarded;
  * Fox N Hounds: the fox reaching 0:00 wins; Car Bomb Tag: the holder at 0:00 loses;
  * "mode_time" is optional: missing -> 3, clamped 1..5, junk -> 3, and the config
    file is never rewritten because of it;
  * Take a Ride sends no JPMK and starts round 1 at boot, as before.

The referee is stepped by hand (Dedicated._mark_step) instead of by mark_loop's wall
clock, so the test is exact and fast. No ports, no game.
"""
import json
import os
import socket
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)

import dediserver as d          # noqa: E402
import mp_test as m             # noqa: E402
from dedi import core           # noqa: E402

fails = 0


def check(name, ok, detail=""):
    global fails
    print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail else ""))
    fails += 0 if ok else 1


def recv_env(sock):
    head = b""
    while len(head) < m.ENV.size:
        chunk = sock.recv(m.ENV.size - len(head))
        if not chunk:
            return None
        head += chunk
    ln, tag, ver, _fl, _sp = m.ENV.unpack(head)
    body = b""
    while len(body) < ln - 8:
        chunk = sock.recv(ln - 8 - len(body))
        if not chunk:
            return None
        body += chunk
    return ver, tag, body


def frames_until(sock, tag, limit=60):
    seen = []
    for _ in range(limit):
        try:
            f = recv_env(sock)
        except socket.timeout:
            break
        if f is None:
            break
        seen.append(f)
        if f[1] == tag:
            break
    return seen


def last_mark(sock):
    seen = frames_until(sock, m.TAG["mark"])
    return m.parse_mark(seen[-1][2]) if seen and seen[-1][1] == m.TAG["mark"] else None


def drain(sock, secs=0.3):
    sock.settimeout(secs)
    out = []
    try:
        while True:
            f = recv_env(sock)
            if f is None:
                break
            out.append(f)
    except socket.timeout:
        pass
    sock.settimeout(5.0)
    return out


def server_for(tmp, **extra):
    cfg = dict(d.DEFAULTS)
    cfg.update({"name": "Mark Test", "master": "", "beacon": False, "plugins_dir": tmp,
                "_config_path": os.path.join(tmp, "dedi.json"), "motd": ""})
    cfg.update(extra)
    return d.Dedicated(cfg, d.Log("error"))


def connect(server, name):
    ours, theirs = socket.socketpair()
    ours.settimeout(5.0)
    threading.Thread(target=server.serve, args=(theirs, ("127.0.0.1", 41000 + len(name))),
                     daemon=True).start()
    m.send_frame(ours, m.TAG["hello"], m.build_hello(name, [("mp", m.MP_VER)]))
    return ours


def carstate(pid, x, z, vx=0, vz=0, model=1):
    row = m.CARSTATE_ENTRY.pack(pid, 1, 0, model, 0xFF, 0, 0, 0, 4096, x, -200, z, 0,
                                0, 0, 0, vx, 0, vz, 0, 0, 0, 2)
    return m.CARSTATE.pack(1, 1, 0, 0, 0) + row


def pick(sock, model=1):
    """JPCC: this player has picked a car (the round-start gate)."""
    m.send_frame(sock, m.TAG["car"], m.CAR_PICK.pack(model, 0xFF))


def wait_ready(server, pid, secs=2.0):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        with server.lock:
            if any(i["id"] == pid and i.get("car") is not None for i in server.clients.values()):
                return True
        time.sleep(0.02)
    return False


def settle(server, sock_list):
    time.sleep(0.15)
    for s in sock_list:
        drain(s, 0.15)


def arm_everyone(server):
    with server.lock:
        for pid in list(server.joined_at):
            server.joined_at[pid] -= 10.0


def check_minutes(tmp):
    check("mode_time missing -> 3 min", core.mode_minutes({}) == 3)
    check("mode_time 9 -> 5 (clamp)", core.mode_minutes({"mode_time": 9}) == 5)
    check("mode_time 0 -> 1 (clamp)", core.mode_minutes({"mode_time": 0}) == 1)
    check("mode_time 'x' -> 3", core.mode_minutes({"mode_time": "x"}) == 3)
    check("mode_time is not a DEFAULTS key (upgrade_config never adds it)", "mode_time" not in d.DEFAULTS)
    path = os.path.join(tmp, "operator.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(dict(d.DEFAULTS, gamemode=1), f, indent=2)
    before = open(path, encoding="utf-8").read()
    added = core.upgrade_config(path, d.DEFAULTS, say=lambda _m: None)
    check("an up-to-date operator config is not rewritten", added == [] and
          open(path, encoding="utf-8").read() == before and not os.path.exists(path + ".bak"), added)
    cfg, _p = core.build_config(["--config", path])
    check("--config without mode_time: 3 min", core.mode_minutes(cfg) == 3, cfg.get("mode_time"))
    cfg, _p = core.build_config(["--config", path, "--mode-time", "4"])
    check("--mode-time 4", core.mode_minutes(cfg) == 4, cfg.get("mode_time"))
    check("...and the file is still untouched", open(path, encoding="utf-8").read() == before)


def check_foxhounds(tmp):
    server = server_for(tmp, gamemode=m.GAMEMODE_FOXHOUNDS)
    try:
        check("F&H: no round 1 at boot (waits for two)", (server.round.number, server.round.phase)
              == (0, m.ROUND_PHASE_IDLE), (server.round.number, server.round.phase))
        check("F&H: default 3 minutes", server.mark.minutes == 3)

        alice = connect(server, "Alice")
        seen = frames_until(alice, m.TAG["mark"])
        tags = [f[1] for f in seen]
        check("a joiner gets ROSTER, WELCOME, START, ROUND, then MARK",
              tags[:5] == [m.TAG["roster"], m.TAG["welcome"], m.TAG["start"], m.TAG["round"], m.TAG["mark"]], tags)
        check("every envelope says proto 23", seen and all(f[0] == 23 for f in seen), [f[0] for f in seen])
        mk = m.parse_mark(seen[-1][2]) if seen else None
        check("one player: waiting, nobody marked, a full 3:00", mk is not None and
              mk["phase"] == m.MARK_PHASE_WAITING and mk["marked"] == -1 and mk["rows"] == [(1, 1800)]
              and mk["gamemode"] == 1 and mk["minutes"] == 3, mk)
        server._mark_step(5000)
        check("a new map: one player waits (the wait needs two)",
              server.mark.phase == m.MARK_PHASE_WAITING and
              server.mark.remain_ms[1] == 180000 and server.round.number == 0,
              (server.mark.phase, server.mark.countdown_ms))
        drain(alice, 0.2)
        server.mark_sent = -1
        server.broadcast_mark_if_due()
        mk = last_mark(alice)
        check("...and hears WAITING", mk is not None and mk["phase"] == m.MARK_PHASE_WAITING, mk)
        server._mark_step(4000)
        check("...still waiting (one player)", server.mark.phase == m.MARK_PHASE_WAITING)
        server._mark_step(1000)
        check("one player does not start the clock", server.mark.phase == m.MARK_PHASE_WAITING and
              server.mark.remain_ms[1] == 180000 and server.round.number == 0)

        bob = connect(server, "Bob")
        frames_until(bob, m.TAG["mark"])
        server._mark_step(33)
        server._mark_step(5000)
        check("two present, no car picked: nothing starts, no clock",
              server.mark.phase == m.MARK_PHASE_WAITING and server.round.number == 0 and
              server.mark.remain_ms == {1: 180000, 2: 180000}, (server.mark.phase, server.mark.remain_ms))
        pick(alice)
        check("Alice's JPCC lands", wait_ready(server, 1))
        server._mark_step(5000)
        check("one car still in the picker: still waiting", server.mark.phase == m.MARK_PHASE_WAITING and
              server.mark.ready == {1} and server.round.number == 0)
        drain(bob, 0.2)
        server.mark_sent = -1                       # force the next send
        server.broadcast_mark_if_due()
        mk = last_mark(bob)
        check("...and the snapshot says who has a car", mk is not None and mk["ready"] == [1] and
              mk["phase"] == m.MARK_PHASE_WAITING, mk)
        pick(bob)
        check("Bob's JPCC lands", wait_ready(server, 2))
        changed = server._mark_step(33)
        check("every car in: the countdown, no round yet, nobody marked", changed and
              server.mark.phase == m.MARK_PHASE_COUNTDOWN and server.mark.marked == -1 and
              server.round.number == 0, (server.mark.phase, server.round.number))
        drain(alice, 0.2)
        server.mark_sent = -1
        server.broadcast_mark_if_due()
        mk = last_mark(alice)
        check("everyone hears the countdown", mk is not None and mk["phase"] == m.MARK_PHASE_COUNTDOWN and
              mk["countdown_ds"] == 50 and mk["ready"] == [1, 2], mk)
        server._mark_step(1000)
        with server.lock:
            server.cars[1] = carstate(1, 1000, 1000)[m.CARSTATE.size:]
            server.cars[2] = carstate(2, 1500, 1000, vx=-20 * 4096)[m.CARSTATE.size:]
            server.car_seen[1] = server.car_seen[2] = time.monotonic()
            server.joined_at[1] -= 10.0
            server.joined_at[2] -= 10.0
        check("no tag during the countdown", not server.mark_carstate(1) and not server.mark_carstate(2)
              and server.mark.marked == -1)
        with server.lock:
            server.joined_at[1] += 10.0
            server.joined_at[2] += 10.0
        check("the countdown does not run the clocks", server.mark.remain_ms == {1: 180000, 2: 180000} and
              server.mark.countdown_ms == 4000, (server.mark.remain_ms, server.mark.countdown_ms))
        changed = server._mark_step(4000)
        check("GO: the countdown starts round 1", changed and server.mark.phase == m.MARK_PHASE_LIVE and
              (server.round.number, server.round.phase) == (1, m.ROUND_PHASE_LIVE) and
              server.mark.marked in (1, 2), (server.mark.phase, server.mark.marked))
        drain(alice, 0.2)
        server.mark_sent = -1
        server.broadcast_round_if_changed()
        server.broadcast_mark_if_due()
        mk = last_mark(alice)
        check("everyone hears who the fox is", mk is not None and mk["phase"] == m.MARK_PHASE_LIVE and
              mk["marked"] == server.mark.marked and mk["seq"] == 1, mk)
        fox = server.mark.marked
        hound = 3 - fox
        socks = {1: alice, 2: bob}
        settle(server, [alice, bob])

        # a spawn bump: inside the arm window, no tag even after the cooldown
        server._mark_step(2500)
        m.send_frame(socks[fox], m.TAG["carstate"], carstate(fox, 1000, 1000))
        m.send_frame(socks[hound], m.TAG["carstate"], carstate(hound, 1500, 1000, vx=-20 * 4096))
        settle(server, [alice, bob])
        check("a bump inside the spawn arm is no tag", server.mark.marked == fox)
        time.sleep(core.TAG_ROW_FRESH_S + 0.2)     # let those rows go stale

        arm_everyone(server)
        m.send_frame(socks[fox], m.TAG["carstate"], carstate(fox, 1000, 1000))
        m.send_frame(socks[hound], m.TAG["carstate"], carstate(hound, 5000, 1000, vx=-20 * 4096))
        settle(server, [alice, bob])
        check("far apart is no tag", server.mark.marked == fox)
        m.send_frame(socks[fox], m.TAG["carstate"], carstate(fox, 1000, 1000))
        m.send_frame(socks[hound], m.TAG["carstate"], carstate(hound, 1500, 1000, vx=-1 * 4096))
        settle(server, [alice, bob])
        check("touching but barely moving is no tag", server.mark.marked == fox)

        fox_clock = server.mark.remain_ms[fox]
        m.send_frame(socks[fox], m.TAG["carstate"], carstate(fox, 1000, 1000))
        m.send_frame(socks[hound], m.TAG["carstate"], carstate(hound, 1500, 1000, vx=-20 * 4096))
        mk = last_mark(socks[fox])
        check("a carstate contact moves the fox (position path)", server.mark.marked == hound and
              mk is not None and mk["marked"] == hound, (server.mark.marked, mk))
        fox, hound = hound, fox
        settle(server, [alice, bob])

        # the bump is still on: both kept rows closing, each owner's next row checked
        # (called in line, so the order is fixed -- two serve threads would race)
        with server.lock:
            now = time.monotonic()
            server.cars[fox] = carstate(fox, 1000, 1000)[m.CARSTATE.size:]
            server.cars[hound] = carstate(hound, 1500, 1000, vx=-20 * 4096)[m.CARSTATE.size:]
            server.car_seen[fox] = server.car_seen[hound] = now
        bounced = server.mark_carstate(hound) or server.mark_carstate(fox)
        check("the same bump does not bounce it straight back (2 s cooldown)",
              not bounced and server.mark.marked == fox)
        server._mark_step(1000)
        check("the old fox's clock is frozen", server.mark.remain_ms[hound] == fox_clock,
              (server.mark.remain_ms[hound], fox_clock))
        check("the new fox's clock runs", server.mark.remain_ms[fox] == 180000 - 1000,
              server.mark.remain_ms[fox])

        # the JPHI fallback, after the cooldown: accepted only near the kept cars
        server._mark_step(1500)
        far = m.HIT.pack(fox, 0, 0, 0, 0, 0, 0, 0, 0)
        with server.lock:
            server.cars[hound] = carstate(hound, 9000, 9000)[m.CARSTATE.size:]
            server.cars[fox] = carstate(fox, 1000, 1000)[m.CARSTATE.size:]
        m.send_frame(socks[hound], m.TAG["hit"], far)
        settle(server, [alice, bob])
        check("a JPHI claim the kept cars contradict is refused", server.mark.marked == fox)
        with server.lock:
            server.cars[hound] = carstate(hound, 1000, 2200)[m.CARSTATE.size:]
        m.send_frame(socks[hound], m.TAG["hit"], far)
        mk = last_mark(socks[fox])
        check("a JPHI claim near the kept cars is a tag (fallback path)", server.mark.marked == hound and
              mk is not None and mk["marked"] == hound, (server.mark.marked, mk))
        fox, hound = hound, fox
        settle(server, [alice, bob])

        # a client cannot write it
        drain(socks[hound], 0.3)
        forged = m.build_mark(1, m.MARK_PHASE_ENDED, hound, hound, 3, 99, 0, [(1, 0), (2, 0)])
        m.send_frame(socks[hound], m.TAG["mark"], forged)
        back = last_mark(socks[hound])
        check("a client's JPMK is answered with the server's own", back is not None and
              back["marked"] == fox and back["phase"] == m.MARK_PHASE_LIVE, back)
        leaked = drain(socks[fox], 0.4)
        check("...and never forwarded", all(m.parse_mark(f[2]) != m.parse_mark(forged)
                                            for f in leaked if f[1] == m.TAG["mark"]))

        # the fox reaching 0:00 wins
        left = server.mark.remain_ms[fox]
        server._mark_step(left - 1)
        check("one ms short: still live", server.mark.phase == m.MARK_PHASE_LIVE)
        server._mark_step(1)
        check("the fox at 0:00 wins the match", server.mark.phase == m.MARK_PHASE_ENDED and
              server.mark.winner == fox and server.round.phase == m.ROUND_PHASE_ENDED,
              (server.mark.phase, server.mark.winner, server.round.phase))
        server.broadcast_mark_if_due()
        mk = last_mark(socks[hound])
        check("...and everyone hears the winner", mk is not None and mk["winner"] == fox and
              mk["phase"] == m.MARK_PHASE_ENDED, mk)
        for s in (alice, bob):
            m.send_frame(s, m.TAG["leave"], b"")
            s.close()
    finally:
        server.stop.set()


def check_bombtag(tmp):
    server = server_for(tmp, gamemode=m.GAMEMODE_BOMBTAG, mode_time=1)
    try:
        a = connect(server, "Ann")
        frames_until(a, m.TAG["mark"])
        b = connect(server, "Ben")
        frames_until(b, m.TAG["mark"])
        pick(a)
        pick(b)
        check("both picks land", wait_ready(server, 1) and wait_ready(server, 2))
        server._mark_step(0)
        check("bomb: first-load countdown, the shared clock full and still",
              server.mark.phase == m.MARK_PHASE_COUNTDOWN and server.mark.countdown_ms == 5000 and
              server.mark.shared_ms == 60000, (server.mark.phase, server.mark.countdown_ms))
        server._mark_step(4999)
        check("...still counting one ms before GO", server.mark.phase == m.MARK_PHASE_COUNTDOWN and
              server.mark.shared_ms == 60000)
        server._mark_step(1)
        holder = server.mark.marked
        check("bomb: live with one holder, one shared 1:00", server.mark.phase == m.MARK_PHASE_LIVE and
              holder in (1, 2) and server.mark.shared_ms == 60000)
        server._mark_step(2000)
        check("the shared clock ticks", server.mark.shared_ms == 58000)
        socks = {1: a, 2: b}
        arm_everyone(server)
        settle(server, [a, b])
        other = 3 - holder
        m.send_frame(socks[holder], m.TAG["carstate"], carstate(holder, 0, 0))
        m.send_frame(socks[other], m.TAG["carstate"], carstate(other, 600, 0, vx=-10 * 4096))
        settle(server, [a, b])
        check("a contact passes the bomb", server.mark.marked == other)
        check("...the clock did not reset", server.mark.shared_ms == 58000)
        c = connect(server, "Cat")
        frames_until(c, m.TAG["mark"])
        check("a late joiner has no bomb", server.mark.marked == other and 3 in server.mark.present)
        server._mark_step(58000)
        check("the holder at 0:00 loses: ended, still marked, no winner",
              server.mark.phase == m.MARK_PHASE_ENDED and server.mark.marked == other and
              server.mark.winner == -1 and server.round.phase == m.ROUND_PHASE_ENDED)
        server._mark_step(10000)
        check("after the intermission: Cat has no car, so waiting (no round 2 yet)",
              server.round.number == 1 and server.mark.phase == m.MARK_PHASE_WAITING and
              server.mark.shared_ms == 0, (server.round.number, server.mark.phase))
        pick(c)
        check("Cat's pick lands", wait_ready(server, 3))
        server._mark_step(33)
        check("...then the same countdown", server.mark.phase == m.MARK_PHASE_COUNTDOWN and
              server.mark.shared_ms == 60000 and server.round.number == 1)
        server._mark_step(4000)
        check("after the countdown, round 2", server.round.number == 2 and
              server.mark.phase == m.MARK_PHASE_LIVE and server.mark.shared_ms == 60000)
        for s in (a, b, c):
            m.send_frame(s, m.TAG["leave"], b"")
            s.close()
    finally:
        server.stop.set()


def check_mark_loop(tmp):
    """The real wall-clock loop: two joiners -> live within a few ticks, and the
    cadence keeps re-sending the snapshot."""
    server = server_for(tmp, gamemode=m.GAMEMODE_FOXHOUNDS, mode_time=2)
    threading.Thread(target=server.mark_loop, daemon=True).start()
    try:
        a = connect(server, "Lou")
        frames_until(a, m.TAG["mark"])
        b = connect(server, "Max")
        frames_until(b, m.TAG["mark"])
        time.sleep(0.5)
        check("mark_loop: a new map, two without cars, waits", server.mark.phase == m.MARK_PHASE_WAITING and
              server.round.number == 0, server.mark.phase)
        time.sleep(1.0)
        check("mark_loop: still waiting without cars", server.mark.phase == m.MARK_PHASE_WAITING and
              server.round.number == 0, server.mark.phase)
        pick(a)
        pick(b)
        time.sleep(0.5)
        check("mark_loop: both cars in -> the first-load countdown on its own",
              server.mark.phase == m.MARK_PHASE_COUNTDOWN and server.round.number == 0, server.mark.phase)
        time.sleep(5.3)
        check("mark_loop starts the round on its own", server.mark.phase == m.MARK_PHASE_LIVE and
              server.round.number == 1 and server.mark.minutes == 2)
        drain(a, 0.1)
        mk = last_mark(a)
        mk2 = last_mark(a)
        check("...and re-sends the snapshot on its cadence", mk is not None and mk2 is not None and
              mk2["phase"] == m.MARK_PHASE_LIVE, (mk, mk2))
        for s in (a, b):
            m.send_frame(s, m.TAG["leave"], b"")
            s.close()
        time.sleep(0.4)
        check("everyone gone: the round ends, no winner", server.mark.phase != m.MARK_PHASE_LIVE and
              server.mark.winner == -1, (server.mark.phase, server.mark.winner))
        check("...and the next one in gets the first-load count again (a map vote's rejoin)",
              server.mark.first_map is True)
        a = connect(server, "Lou")
        frames_until(a, m.TAG["mark"])
        time.sleep(0.4)
        check("...after the intermission, one player still waits",
              server.mark.phase in (m.MARK_PHASE_ENDED, m.MARK_PHASE_WAITING),
              server.mark.phase)
        m.send_frame(a, m.TAG["leave"], b"")
        a.close()
    finally:
        server.stop.set()


def check_take_a_ride(tmp):
    server = server_for(tmp)
    try:
        check("Take a Ride: round 1 live at boot, the referee inactive",
              (server.round.number, server.round.phase) == (1, m.ROUND_PHASE_LIVE) and not server.mark.active)
        s = connect(server, "Tara")
        seen = frames_until(s, m.TAG["round"])
        seen += drain(s, 0.5)
        check("Take a Ride sends no JPMK", all(f[1] != m.TAG["mark"] for f in seen), [f[1] for f in seen])
        rd = [m.parse_round(f[2]) for f in seen if f[1] == m.TAG["round"]][0]
        check("Take a Ride snapshot unchanged", rd == (0, m.ROUND_PHASE_LIVE, 1, [(1, m.ROUND_PLAYER_PLAYING, -1, 0)]), rd)
        check("...and its status has no mark line", "mark:" not in server.cmd_status(""))
        m.send_frame(s, m.TAG["leave"], b"")
        s.close()
    finally:
        server.stop.set()


def main():
    tmp = tempfile.mkdtemp()
    check_minutes(tmp)
    check_foxhounds(tmp)
    check_bombtag(tmp)
    check_mark_loop(tmp)
    check_take_a_ride(tmp)
    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
