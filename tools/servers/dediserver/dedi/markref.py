"""dedi.markref -- the marked-player referee (MP_TAG_MARK 'JPMK', proto 23).

Pre-game (modes that opt in via mp_mode.c: waitMinPlayers, waitForCars,
firstCountdownSec, roundCountdownSec): after a map load -- reset(), or the
present-set emptying (a map vote kicks everyone to rejoin on the new map) --
WAITING until the mode's wait is satisfied, then one COUNTDOWN. A fresh map
uses firstCountdownSec as N..1 then GO; later rounds on the same map use
COUNTDOWN_SPAWN_MS (GET READY / the start cluster) plus roundCountdownSec,
then GO. first_map is only cleared at GO, so an aborted first-load count is
retried. Back to WAITING from COUNTDOWN when someone leaves or a joiner without
a car arrives; the same gate after intermission. PHASE_LOBBY is retired (the
wire value is unused). Take a Ride leaves the four fields at 0 and this
referee stays inert.

The server has no world, so it only referees the countdown: each game runs the
round-reset respawn and puts its OWN car in the start cluster the moment it
sees COUNTDOWN begin (mp_session.c MpMarkRoundReset / MpMarkClusterTick), not
during WAITING and not mid-count. That path also clears the bomb wreck latch.

The Python twin of mp_mark.c's host half, step for step: Fox N Hounds (a clock per
player that ticks only while they are the fox; first to 0:00 wins) and Car Bomb Tag
(one shared clock; whoever holds the bomb at 0:00 loses). tools/mode_check runs the
same script through both and compares every snapshot byte for byte, so keep the
two in lockstep: same order of checks, same rounding, same random pick.

No clock and no randomness of its own: the caller passes elapsed milliseconds and a
random number, exactly as the in-game host does. Not thread-safe on its own:
Dedicated holds its lock around every call.
"""
import mp_test as m

TAG_COOLDOWN_MS = 2000       # mp_mark.h MP_MARK_TAG_COOLDOWN_MS
INTERMISSION_MS = 10000      # mp_mark.h MP_MARK_INTERMISSION_MS
COUNTDOWN_MS = 4000          # default later-round total: SPAWN + 3 s
COUNTDOWN_SPAWN_MS = 1000    # mp_mark.h MP_MARK_COUNTDOWN_SPAWN_MS
LOBBY_MS = 5000              # default firstCountdownSec (5-4-3-2-1), in ms

EV_BEGIN = 0x01              # mp_mark.h MP_MARK_EV_*
EV_END = 0x02
EV_MOVED = 0x04
EV_COUNTDOWN = 0x08
EV_LOBBY = 0x10              # retired: never set

# Cops n Robbers (mp_mark.h MP_MARK_CNR_*)
CNR_HIT_COOLDOWN_MS = 1000
CNR_HIT_MAX = 20
CNR_HIT_POINTS = 5
CNR_BUST_POINTS = 1000
CNR_DRIVE_BASE = 10
CNR_DRIVE_BONUS = 5
CNR_DRIVE_TOPSPEED = 300
CNR_ESCAPE_DIST = 32000
CNR_ESCAPE_HOLD_MS = 5000


def clamp_minutes(minutes):
    """MpMarkClampMinutes: 1..5. A missing/unreadable value is the caller's (3)."""
    return max(m.MARK_MINUTES_MIN, min(m.MARK_MINUTES_MAX, int(minutes)))


def _ds(ms):
    """Deciseconds, rounded UP (0:00 only when it really is), u16."""
    if ms <= 0:
        return 0
    return min((ms + 99) // 100, 0xFFFF)


class MarkReferee:
    def __init__(self, gamemode=0, minutes=m.MARK_MINUTES_DEFAULT):
        self.reset(gamemode, minutes)

    def reset(self, gamemode, minutes):
        self.gamemode = int(gamemode)
        self.timer = m.mode_timer(self.gamemode)
        self.wait_min = m.mode_wait_min(self.gamemode)
        self.wait_cars = m.mode_wait_cars(self.gamemode)
        self.first_sec = m.mode_first_countdown(self.gamemode)
        self.round_sec = m.mode_round_countdown(self.gamemode)
        self.first_map = bool(m.mode_has_pre_game(self.gamemode))  # a reset is a map load
        self.phase = m.MARK_PHASE_WAITING
        self.minutes = clamp_minutes(minutes)
        self.marked = -1
        self.winner = -1
        self.seq = 0
        self.revision = 0
        self.shared_ms = 0
        self.clock_ms = 0
        self.cooldown_until = 0
        self.intermission_ms = 0
        self.countdown_ms = 0
        self.countdown_total_ms = 0
        self.countdown_first = False
        self.present = set()
        self.ready = set()           # has picked a car (MpMarkReady); any id, present or not
        self.remain_ms = {}
        # MODE_TIMER_GETAWAY (Cops n Robbers)
        self.last_getaway = -1
        self.first_pick = -1
        self.heat = 0
        self.drive_acc = 0
        self.hit_until = {}

    @property
    def active(self):
        return self.timer != m.MODE_TIMER_NONE

    def _one_clock(self):
        return self.timer in (m.MODE_TIMER_SHARED, m.MODE_TIMER_GETAWAY)

    def _next_getaway(self, rnd):
        """MpMarkNextGetaway: random first (kept across an aborted count), then in turn."""
        if self.last_getaway < 0:
            if self.first_pick not in self.present:
                self.first_pick = self._pick(rnd)
            return self.first_pick
        for i in range(1, m.MAX_PLAYERS + 1):
            pid = (self.last_getaway + i) % m.MAX_PLAYERS
            if pid in self.present:
                return pid
        return -1

    def _full_ms(self):
        return self.minutes * 60 * 1000

    def _pick(self, rnd):
        ids = sorted(self.present)
        return ids[rnd % len(ids)] if ids else -1

    def _fresh_clocks(self):
        personal = self.timer == m.MODE_TIMER_PERSONAL
        self.remain_ms = {pid: (self._full_ms() if personal else 0) for pid in self.present}
        self.shared_ms = self._full_ms() if self._one_clock() else 0

    def _startable(self):
        """MpMarkStartable: the mode's wait (min players, and wait-for-cars)."""
        need = self.wait_min
        if need < 1:
            need = 1 if (self.wait_cars or self.first_sec > 0 or self.round_sec > 0) else 2
        if len(self.present) < need:
            return False
        return not self.wait_cars or self.present <= self.ready

    def _begin(self, rnd):
        self.phase = m.MARK_PHASE_LIVE
        self.winner = -1
        self.countdown_ms = 0
        self.countdown_total_ms = 0
        self.countdown_first = False
        self.first_map = False
        self._fresh_clocks()
        if self.timer == m.MODE_TIMER_GETAWAY:
            if self.marked not in self.present:
                self.marked = self._next_getaway(rnd)
            self.last_getaway = self.marked
            self.heat = 0
            self.drive_acc = 0
            self.hit_until = {}
        else:
            self.marked = self._pick(rnd)
        self.cooldown_until = self.clock_ms + TAG_COOLDOWN_MS
        self.seq = (self.seq + 1) & 0xFFFF
        self.revision += 1

    def _countdown(self, first, rnd=0):
        self.phase = m.MARK_PHASE_COUNTDOWN
        self.marked = self._next_getaway(rnd) if self.timer == m.MODE_TIMER_GETAWAY else -1
        self.heat = 0
        self.winner = -1
        self.countdown_first = bool(first)
        if first and self.first_sec > 0:
            ms = self.first_sec * 1000
        elif self.round_sec > 0:
            ms = COUNTDOWN_SPAWN_MS + self.round_sec * 1000
        else:
            ms = 0
        self.countdown_ms = ms
        self.countdown_total_ms = ms
        self._fresh_clocks()
        self.revision += 1

    def _start(self, rnd):
        first = self.first_map and self.first_sec > 0
        if first or self.round_sec > 0:
            self._countdown(first, rnd)
            if self.countdown_ms > 0:
                return EV_COUNTDOWN
        self._begin(rnd)
        return EV_BEGIN

    def _end(self, winner):
        self.phase = m.MARK_PHASE_ENDED
        self.winner = winner
        self.intermission_ms = INTERMISSION_MS
        self.revision += 1

    def sync(self, ids, rnd=0):
        """MpMarkSync: present-set = `ids`. Returns EV_* bits."""
        if not self.active:
            return 0
        ids = {int(i) for i in ids if 0 <= int(i) < m.MAX_PLAYERS}
        changed = marked_left = False
        for pid in range(m.MAX_PLAYERS):
            want = pid in ids
            if want == (pid in self.present):
                continue
            changed = True
            if want:
                self.present.add(pid)
                self.remain_ms[pid] = self._full_ms() if self.timer == m.MODE_TIMER_PERSONAL else 0
            else:
                self.present.discard(pid)
                self.remain_ms.pop(pid, None)
                if pid == self.marked:
                    marked_left = True
        ev = 0
        if marked_left and self.phase == m.MARK_PHASE_LIVE and self.timer == m.MODE_TIMER_GETAWAY:
            self._end(-1)                       # the getaway left: nobody won it
            ev |= EV_END
        elif marked_left and self.phase == m.MARK_PHASE_LIVE:
            self.marked = self._pick(rnd)
            self.cooldown_until = self.clock_ms + TAG_COOLDOWN_MS
            self.seq = (self.seq + 1) & 0xFFFF
            ev |= EV_MOVED
        elif marked_left:
            self.marked = -1
        if changed:
            self.revision += 1
        # everyone gone: the next arrival is loading the map -> first-load count again
        if changed and m.mode_has_pre_game(self.gamemode) and not self.present:
            self.first_map = True
        return ev

    def set_ready(self, ids):
        """MpMarkReady: ready-set = `ids` (players who have picked a car)."""
        if not self.active:
            return
        ids = {int(i) for i in ids if 0 <= int(i) < m.MAX_PLAYERS}
        changed = (ids ^ self.ready) & self.present
        self.ready = ids
        if changed:
            self.revision += 1

    def tick(self, ms, rnd=0):
        """MpMarkTick: advance by `ms`. Returns EV_* bits."""
        if not self.active or ms < 0:
            return 0
        self.clock_ms += ms
        n = len(self.present)
        if self.phase == m.MARK_PHASE_WAITING:
            if self._startable():
                return self._start(rnd)
            return 0
        if self.phase == m.MARK_PHASE_LOBBY:
            # retired: treat a stale snapshot as WAITING
            self.phase = m.MARK_PHASE_WAITING
            self.countdown_ms = 0
            self.countdown_total_ms = 0
            self.countdown_first = False
            self.revision += 1
            if self._startable():
                return self._start(rnd)
            return 0
        if self.phase == m.MARK_PHASE_COUNTDOWN:
            if not self._startable():
                self.phase = m.MARK_PHASE_WAITING
                self.marked = -1
                self.countdown_ms = 0
                self.countdown_total_ms = 0
                self.countdown_first = False
                self.revision += 1
                return 0
            self.countdown_ms -= ms
            if self.countdown_ms > 0:
                return 0
            self._begin(rnd)                        # GO
            return EV_BEGIN
        if self.phase == m.MARK_PHASE_LIVE:
            if n < 2:
                self._end(-1)
                return EV_END
            if self.timer == m.MODE_TIMER_PERSONAL:
                if self.marked not in self.remain_ms:
                    return 0
                self.remain_ms[self.marked] -= ms
                if self.remain_ms[self.marked] <= 0:
                    self.remain_ms[self.marked] = 0
                    self._end(self.marked)          # first to 0:00 wins
                    return EV_END
            else:
                self.shared_ms -= ms
                if self.shared_ms <= 0:
                    self.shared_ms = 0
                    # bomb: the holder loses; Cops n Robbers: the getaway lasted and wins
                    self._end(self.marked if self.timer == m.MODE_TIMER_GETAWAY else -1)
                    return EV_END
            return 0
        # ENDED
        self.intermission_ms -= ms
        if self.intermission_ms > 0:
            return 0
        if self._startable():
            return self._start(rnd)
        self.phase = m.MARK_PHASE_WAITING
        self.marked = -1
        self.winner = -1
        self.revision += 1
        return 0

    def contact(self, a, b):
        """MpMarkContact: True when the contact moved the mark."""
        if self.phase != m.MARK_PHASE_LIVE or not self.active or self.timer == m.MODE_TIMER_GETAWAY:
            return False
        if a == b or a not in self.present or b not in self.present:
            return False
        if self.marked == a:
            to = b
        elif self.marked == b:
            to = a
        else:
            return False
        if self.clock_ms < self.cooldown_until:
            return False
        self.marked = to
        self.cooldown_until = self.clock_ms + TAG_COOLDOWN_MS
        self.seq = (self.seq + 1) & 0xFFFF
        self.revision += 1
        return True

    def hit(self, a, b, strength):
        """MpMarkHit: (points the cop earned, EV_* bits)."""
        if self.phase != m.MARK_PHASE_LIVE or self.timer != m.MODE_TIMER_GETAWAY:
            return 0, 0
        if a == b or a not in self.present or b not in self.present:
            return 0, 0
        if self.marked == a:
            cop = b
        elif self.marked == b:
            cop = a
        else:
            return 0, 0
        if strength <= 0 or self.clock_ms < self.hit_until.get(cop, 0):
            return 0, 0
        strength = min(strength, CNR_HIT_MAX)
        pts = strength * CNR_HIT_POINTS
        self.hit_until[cop] = self.clock_ms + CNR_HIT_COOLDOWN_MS
        self.heat += pts
        self.revision += 1
        if self.heat >= CNR_BUST_POINTS:
            self.heat = CNR_BUST_POINTS
            self._end(-1)                       # busted: the cops win
            return pts, EV_END
        return pts, 0

    def drive(self, ms, speed):
        """MpMarkDrive: the getaway's whole points for `ms` alive at `speed`."""
        if self.phase != m.MARK_PHASE_LIVE or self.timer != m.MODE_TIMER_GETAWAY or self.marked < 0 or ms <= 0:
            return 0
        speed = max(0, min(CNR_DRIVE_TOPSPEED, int(speed)))
        den = 1000 * CNR_DRIVE_TOPSPEED
        self.drive_acc += ms * (CNR_DRIVE_BASE * CNR_DRIVE_TOPSPEED + CNR_DRIVE_BONUS * speed)
        pts = self.drive_acc // den
        self.drive_acc %= den
        return pts

    def escape(self):
        """MpMarkEscape: the getaway is clear of every cop -> it wins now."""
        if self.phase != m.MARK_PHASE_LIVE or self.timer != m.MODE_TIMER_GETAWAY or self.marked < 0:
            return 0
        self._end(self.marked)
        return EV_END

    def to_bytes(self):
        if self.timer == m.MODE_TIMER_GETAWAY:   # the getaway's row carries the heat meter
            rows = [(pid, self.heat if pid == self.marked else 0,
                     m.MARK_ENTRY_F_READY if pid in self.ready else 0) for pid in sorted(self.present)]
            return m.build_mark(self.gamemode, self.phase, self.marked, self.winner, self.minutes,
                                self.seq, _ds(self.shared_ms), rows, _ds(self.countdown_ms))
        rows = [(pid, _ds(self.remain_ms.get(pid, 0)),
                 m.MARK_ENTRY_F_READY if pid in self.ready else 0) for pid in sorted(self.present)]
        return m.build_mark(self.gamemode, self.phase, self.marked, self.winner, self.minutes,
                            self.seq, _ds(self.shared_ms), rows, _ds(self.countdown_ms))
