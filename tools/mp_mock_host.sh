#!/bin/sh
# ============================================================================
# mp_mock_host.sh [options]              -- the Linux/macOS twin of mp_mock_host.bat
#
#   A headless mock HOST: speaks the mp protocol, spawns no game. Use it to drive
#   the real game as a CLIENT without a second machine, and to create conditions
#   on demand -- a rejection, a chosen build hash, a chosen city, a peer that goes
#   quiet.
#
#       ./mp_mock_host.sh --start                  accept, then run a match
#       ./mp_mock_host.sh --start --peer-pad 0x40  and drive the "host" car
#       ./mp_mock_host.sh --reject mods            refuse the join, with a reason
#
#   Start the game against it with ./mp_join.sh 127.0.0.1:<port>.
#
#   It is one-sided by nature: one real engine in the room, so it can prove the
#   wire format and the client's own behaviour, never what two engines do to each
#   other. mp_pair.sh is for that.
# ============================================================================
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PY=${MP_PYTHON:-$(command -v python3 || command -v python)}

exec "$PY" "$HERE/mp_test.py" host --start "$@"
