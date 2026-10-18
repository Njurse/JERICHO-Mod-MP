#!/bin/sh
# ============================================================================
# mp_dedi.sh [options]                       -- the Linux/macOS twin of mp_dedi.bat
#
#   A headless DEDICATED SERVER: hosts a real session with no game window, so a
#   client has something to join, and so a server can be left up while the client
#   side restarts. Ports and the beacon behave like a hosted game's.
#
#   It is pure Python and needs NO game build on this machine.
#
#       ./mp_dedi.sh                   host on the port dediserver.json says
#       ./mp_dedi.sh --port 1400       ... on another one
#       ./mp_dedi.sh --name "Bob's 24/7" --no-beacon --idle-drop 0
#       ./mp_dedi.sh --master http://master.example.com:8100
#
#   Config lives in servers/dediserver/dediserver.json (CLI flags override it);
#   it is created from dediserver.example.json on the first start.
#   Ctrl-C stops it, and a clean stop UNREGISTERS from the master instead of
#   leaving an entry to lapse.
#
#   Ports to open and the full option list: servers/dediserver/README.md
#   (mp_dedi.bat carries the same notes for Windows.)
# ============================================================================
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PY=${MP_PYTHON:-$(command -v python3 || command -v python)}

exec "$PY" "$HERE/servers/dediserver/dediserver.py" "$@"
