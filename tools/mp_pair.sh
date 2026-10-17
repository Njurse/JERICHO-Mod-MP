#!/bin/sh
# ============================================================================
# mp_pair.sh [options]                       -- the Linux/macOS twin of mp_pair.bat
#
#   Run TWO REAL instances on this one PC -- one hosting, one joining -- and print
#   both sides' logs. Reach for this first: nearly every multiplayer bug so far
#   showed up here, and the mock could never have found them because it only ever
#   puts one real engine in the room.
#
#   Thin wrapper around mp_localpair.py, which does the work: two throwaway run
#   directories beside the game (junctions/symlinks, so the big trees are shared
#   and nothing is copied -- separate working directories are what stop the two
#   JERICHO.log and mp.ini files fighting), MP_AUTOSTART=host on the host seat
#   only, and both executables launched DIRECTLY so the PIDs are real and the
#   cleanup stops exactly what it started.
#
#       ./mp_pair.sh                      host + join, report, clean up
#       ./mp_pair.sh --players 3 --stagger 12
#       ./mp_pair.sh --seconds 90 --keep
#
#   The game binary is found the way mp_host.sh finds it (MP_EXEDIR / MP_EXE).
# ============================================================================
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PY=${MP_PYTHON:-$(command -v python3 || command -v python)}

exec "$PY" "$HERE/mp_localpair.py" "$@"
