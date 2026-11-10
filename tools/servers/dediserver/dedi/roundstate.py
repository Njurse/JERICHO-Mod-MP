"""dedi.roundstate -- the round state this server referees (MP_TAG_ROUND, proto 22).

The Python twin of mp_round.c's host half. A dedicated server is a match in
progress from the moment it starts (every joiner gets a START), so round 1 begins
live when the server comes up. The present-set is the connected clients -- the same
rows the roster names, and like the roster no row for the server itself, which has
no car. A player who arrives during a live round is playing (Take a Ride's
late-join policy); one who leaves loses their row.

Take a Ride: one round, live for the whole run, every player playing, no team,
score 0. Nothing here ends a round yet; end() exists for the modes that will.

Not thread-safe on its own: Dedicated holds its lock around every call.
"""
import mp_test as m


class RoundState:
    def __init__(self, gamemode=0):
        self.gamemode = int(gamemode)
        self.number = 0
        self.phase = m.ROUND_PHASE_IDLE
        self.players = {}        # playerId -> {"state", "team", "score"}
        self.revision = 0

    def begin(self, gamemode=None):
        """The next round: number + 1, live, every present player playing, score 0."""
        if gamemode is not None:
            self.gamemode = int(gamemode)
        self.number = 1 if self.number >= 0xFFFF else self.number + 1
        self.phase = m.ROUND_PHASE_LIVE
        keep = m.mode_keeps_score(self.gamemode)  # Cops n Robbers: the match total
        for row in self.players.values():
            row.update(state=m.ROUND_PLAYER_PLAYING, team=-1, score=row["score"] if keep else 0)
        self.revision += 1

    def add_score(self, pid, points):
        """MpRoundAddScore: True when the row exists."""
        row = self.players.get(int(pid))
        if row is None or not points:
            return False
        row["score"] = max(-0x7FFFFFFF, min(0x7FFFFFFF, row["score"] + int(points)))
        self.revision += 1
        return True

    def end(self):
        if self.phase != m.ROUND_PHASE_ENDED:
            self.phase = m.ROUND_PHASE_ENDED
            self.revision += 1

    def _arrival_state(self):
        # MP_MODE_LATEJOIN_LIVE (Take a Ride): straight in. Not in a live round: wait.
        return m.ROUND_PLAYER_PLAYING if self.phase == m.ROUND_PHASE_LIVE else m.ROUND_PLAYER_WAITING

    def sync(self, ids):
        """Make the present-set equal `ids`. True when anything changed."""
        ids = {int(i) for i in ids if 0 <= int(i) < m.MAX_PLAYERS}
        changed = False
        for pid in list(self.players):
            if pid not in ids:
                del self.players[pid]
                changed = True
        for pid in ids:
            if pid not in self.players:
                self.players[pid] = {"state": self._arrival_state(), "team": -1, "score": 0}
                changed = True
        if changed:
            self.revision += 1
        return changed

    def rows(self):
        return [(pid, r["state"], r["team"], r["score"]) for pid, r in sorted(self.players.items())]

    def to_bytes(self):
        return m.build_round(self.gamemode, self.phase, self.number, self.rows())
