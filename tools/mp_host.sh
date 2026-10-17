#!/bin/sh
# ============================================================================
# mp_host.sh [port] [lobby] [dry]            -- the Linux/macOS twin of mp_host.bat
#
#   Host a LAN game and go straight into hosting -- no frontend walking.
#
#       ./mp_host.sh                host on 1318 (what mp.ini says)
#       ./mp_host.sh 1400           ... on another port
#       ./mp_host.sh 1400 lobby     wait for somebody to press Start
#       ./mp_host.sh dry            print the command, launch nothing
#
#   The port has to MATCH JERICHO/CONFIG/mp.ini: the game loads that file at boot
#   and binds the listener before the command line is read, so a port given here
#   cannot MOVE an already-bound host -- it only desyncs what the host advertises
#   from what it listens on. To really change it, edit `port` in mp.ini.
#
#   MP_AUTOSTART=host is set for you (an MP_AUTOSTART you set yourself is left
#   alone), so the match starts as soon as a player is in. Pass "lobby" to sit and
#   wait instead.
#
#   THE LINUX DIFFERENCES from mp_host.bat (which carries the full notes):
#     * the binary has no .exe, and Release on Linux is lowercased to `jericho`
#       (premake renames it so it can sit beside the JERICHO/ runtime tree on a
#       case-sensitive filesystem). Release_dev, which the rigs use, is JERICHO_dev.
#       Both are looked for here; MP_EXE names one explicitly.
#     * the batch file uses `start`, which detaches and loses the PID. Here the game
#       is backgrounded with nohup and its PID is printed, so you can wait on it or
#       stop it deliberately:  kill "$(cat /tmp/mp-host.pid)"
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

PORT=${1:-1318}
if [ "${2:-}" = "lobby" ]; then
	MP_AUTOSTART=""
else
	: "${MP_AUTOSTART:=host}"
fi

if [ "${1:-}" = "dry" ] || [ "${2:-}" = "dry" ]; then
	echo "mp: would run, in $EXEDIR"
	echo "  ./$EXE -nointro -nofmv -host $PORT"
	echo "  with MP_AUTOSTART=$MP_AUTOSTART"
	exit 0
fi

echo "mp: hosting on port $PORT, autostart '$MP_AUTOSTART'"
cd "$EXEDIR"
MP_AUTOSTART=$MP_AUTOSTART nohup "./$EXE" -nointro -nofmv -host "$PORT" >/tmp/mp-host.log 2>&1 &
echo "mp: pid $! (log: /tmp/mp-host.log)"
exit 0
