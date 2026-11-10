"""dedi.markref -- the marked-player referee (MP_TAG_MARK 'JPMK', proto 23).

Pre-game lobby (countdown modes): after a map load -- reset(), or the present-set
emptying (a map vote kicks everyone to rejoin on the new map) -- and once someone is
present, LOBBY for LOBBY_MS (5-4-3-2-1, nothing ticks, nothing tags), once per map.

Round start (countdown modes, mp_mode.c countdown = 1): WAITING until two players are
present AND every one of them has picked a car (set_ready), then COUNTDOWN for
COUNTDOWN_MS (no clock, no tag), then LIVE (GO). Back to WAITING from COUNTDOWN when
someone leaves or a joiner without a car arrives; the same gate after intermission.
The server has no world, so it only referees the countdown: each game puts its OWN car
in the round-start cluster when it sees COUNTDOWN (mp_session.c MpMarkClusterTick).

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
COUNTDOWN_MS = 4000          # mp_mark.h MP_MARK_COUNTDOWN_MS (GET READY, 3, 2, 1)
LOBBY_MS = 5000              # mp_mark.h MP_MARK_LOBBY_MS (the pre-game 5, 4, 3, 2, 1)

EV_BEGIN = 0x01              # mp_mark.h MP_MARK_EV_*
EV_END = 0x02
EV_MOVED = 0x04
EV_COUNTDOWN = 0x08
EV_LOBBY = 0x10


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
        self.countdown = m.mode_countdown(self.gamemode)
        self.lobby = bool(self.countdown)   # a reset is a map load
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
        self.present = set()
        self.ready = set()           # has picked a car (MpMarkReady); any id, present or not
        self.remain_ms = {}

    @property
    def active(self):
        return self.timer != m.MODE_TIMER_NONE

    def _full_ms(self):
        return self.minutes * 60 * 1000

    def _pick(self, rnd):
        ids = sorted(self.present)
        return ids[rnd % len(ids)] if ids else -1

    def _fresh_clocks(self):
        personal = self.timer == m.MODE_TIMER_PERSONAL
        self.remain_ms = {pid: (self._full_ms() if personal else 0) for pid in self.present}
        self.shared_ms = self._full_ms() if self.timer == m.MODE_TIMER_SHARED else 0

    def _startable(self):
        """MpMarkStartable: two present, and (countdown mode) every one has a car."""
        if len(self.present) < 2:
            return False
        return not self.countdown or self.present <= self.ready

    def _begin(self, rnd):
        self.phase = m.MARK_PHASE_LIVE
        self.winner = -1
        self.countdown_ms = 0
        self._fresh_clocks()
        self.marked = self._pick(rnd)
        self.cooldown_until = self.clock_ms + TAG_COOLDOWN_MS
        self.seq = (self.seq + 1) & 0xFFFF
        self.revision += 1

    def _countdown(self):
        self.phase = m.MARK_PHASE_COUNTDOWN
        self.marked = -1
        self.winner = -1
        self.countdown_ms = COUNTDOWN_MS
        self._fresh_clocks()
        self.revision += 1

    def _start(self, rnd):
        if self.countdown:
            self._countdown()
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
        if marked_left and self.phase == m.MARK_PHASE_LIVE:
            self.marked = self._pick(rnd)
            self.cooldown_until = self.clock_ms + TAG_COOLDOWN_MS
            self.seq = (self.seq + 1) & 0xFFFF
            ev |= EV_MOVED
        elif marked_left:
            self.marked = -1
        if changed:
            self.revision += 1
        # everyone gone: the next arrival is loading the map -> the lobby again
        if changed and self.countdown and not self.present and self.phase != m.MARK_PHASE_LOBBY:
            self.lobby = True
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
            if self.lobby and n >= 1:
                self.lobby = False
                self.phase = m.MARK_PHASE_LOBBY
                self.countdown_ms = LOBBY_MS
                self.marked = -1
                self.winner = -1
                self.revision += 1
                return EV_LOBBY
            if self._startable():
                return self._start(rnd)
            return 0
        if self.phase == m.MARK_PHASE_LOBBY:
            if n == 0:
                self.countdown_ms = LOBBY_MS            # hold it full until someone is here
                return 0
            self.countdown_ms -= ms
            if self.countdown_ms > 0:
                return 0
            self.lobby = False
            self.phase = m.MARK_PHASE_WAITING
            self.countdown_ms = 0
            self.revision += 1
            if self._startable():
                return self._start(rnd)
            return 0
        if self.phase == m.MARK_PHASE_COUNTDOWN:
            if not self._startable():
                self.phase = m.MARK_PHASE_WAITING
                self.countdown_ms = 0
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
                    self._end(-1)                   # the holder (still `marked`) loses
                    return EV_END
            return 0
        # ENDED
        self.intermission_ms -= ms
        if self.intermission_ms > 0:
            return 0
        if not self.lobby and self._startable():
            return self._start(rnd)
        self.phase = m.MARK_PHASE_WAITING
        self.marked = -1
        self.winner = -1
        self.revision += 1
        return 0

    def contact(self, a, b):
        """MpMarkContact: True when the contact moved the mark."""
        if self.phase != m.MARK_PHASE_LIVE or not self.active:
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

    def to_bytes(self):
        rows = [(pid, _ds(self.remain_ms.get(pid, 0)),
                 m.MARK_ENTRY_F_READY if pid in self.ready else 0) for pid in sorted(self.present)]
        return m.build_mark(self.gamemode, self.phase, self.marked, self.winner, self.minutes,
                            self.seq, _ds(self.shared_ms), rows, _ds(self.countdown_ms))
