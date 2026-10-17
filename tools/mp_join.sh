#!/bin/sh
# ============================================================================
# mp_join.sh [ip[:port]] [dry]               -- the Linux/macOS twin of mp_join.bat
#
#   Join a LAN game and go straight into it -- no frontend walking.
#
#       ./mp_join.sh                  host and client on this one machine
#       ./mp_join.sh 192.168.1.42     the host's LAN address (default port)
#       ./mp_join.sh 192.168.1.42:1400
#       ./mp_join.sh 192.168.1.42 dry print the command, launch nothing
#
#   Leave the port off in the normal case: a bare address joins on THIS machine's
#   mp.ini port, which is what the host uses too when you both run the same build.
#   Add `:port` only if the host's mp.ini really says something else.
#
#   Joining by address skips LAN discovery entirely and needs only the host's TCP
#   session port open, so it is the path to trust on a LAN with a firewall.
#
#   MP_AUTOSTART is cleared here: the client launches as soon as the host's start
#   arrives, and it must never be told to host as well.
#
#   THE LINUX DIFFERENCES from mp_join.bat: the binary has no .exe and Release on
#   Linux is lowercased to `jericho` (see mp_host.sh); and this backgrounds the game
#   with nohup and prints its PID, where the batch file's `start` detaches and loses
#   it.
# ============================================================================
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
EXEDIR=${MP_EXEDIR:-"$HERE/../../../../src_rebuild/bin/Release_dev"}

EXE=""
for cand in ${MP_EXE:-JERICHO_dev} JERICHO_dev jericho JERICHO_dbg; do
	if [ -x "$EXEDIR/$cand" ]; then
		EXE=$cand
		break
	fi
done
if [ -z "$EXE" ]; then
	echo "mp: no game binary in $EXEDIR" >&2
	echo "    build it, or set MP_EXEDIR to the directory that has it" >&2
	exit 1
fi

ADDR=${1:-127.0.0.1}

if [ "${1:-}" = "dry" ] || [ "${2:-}" = "dry" ]; then
	echo "mp: would run, in $EXEDIR"
	echo "  ./$EXE -nointro -nofmv -join $ADDR"
	exit 0
fi

echo "mp: joining $ADDR"
cd "$EXEDIR"
MP_AUTOSTART= nohup "./$EXE" -nointro -nofmv -join "$ADDR" >/tmp/mp-join.log 2>&1 &
echo "mp: pid $! (log: /tmp/mp-join.log)"
exit 0
