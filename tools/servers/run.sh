#!/bin/sh
# ============================================================================
# run.sh -- one way in to the server-side tooling, for Linux/macOS.
#           run.bat is the same thing for Windows.
#
#   ./run.sh master             the DISCOVERY SERVER: the registry players list from
#   ./run.sh dedi [options]     a headless DEDICATED SERVER, registered with it
#   ./run.sh stack [options]    master + dedi + N real clients on this one PC (the rig)
#   ./run.sh checks             the checks CI runs for this folder
#   ./run.sh docker             docker compose up -d --build, for a VPS
#   ./run.sh help
#
#   Everything after the verb goes to the tool itself, so the full option lists are
#   in masterserver/README.md, dediserver/README.md and TESTING.md. The verbs are
#   only a shortcut: each one is one python3 invocation, printed as it runs.
#
#   MP_PYTHON names the interpreter when `python3` is not the one you want.
# ============================================================================
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PY=${MP_PYTHON:-$(command -v python3 || command -v python)}

verb=${1:-help}
[ $# -gt 0 ] && shift

case "$verb" in
master)
	echo "run: masterserver (the discovery registry)"
	exec "$PY" "$HERE/masterserver/masterserver.py" "$@"
	;;
dedi)
	echo "run: dediserver (a headless dedicated server)"
	exec "$PY" "$HERE/dediserver/dediserver.py" "$@"
	;;
stack)
	echo "run: the local stack rig (master + dedi + real clients on this PC)"
	exec "$PY" "$HERE/rigs/local_stack.py" "$@"
	;;
checks)
	# the same set the CI job runs, in the same order, stopping at the first
	# failure -- a green run here is what "the servers are fine" means
	fail=0
	for t in proto_check.py check_images.py console_selftest.py masterserver/test_masterserver.py masterserver/test_console.py masterserver/test_probe.py dediserver/test_console.py test_console_stdin.py rigs/test_e2e.py; do
		echo
		echo "== $t"
		"$PY" "$HERE/$t" || fail=1
	done
	echo
	if [ "$fail" -eq 0 ]; then
		echo "run: checks OK -- wire mirror, image copies, the master, the servers' consoles,"
		echo "       and master+dedi+client end to end"
	else
		echo "run: checks FAILED (see above)" >&2
	fi
	exit "$fail"
	;;
docker)
	if ! command -v docker >/dev/null 2>&1; then
		echo "run: no docker on PATH" >&2
		exit 1
	fi
	exec docker compose -f "$HERE/docker-compose.yml" up -d --build
	;;
help | --help | -h | "")
	sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'
	;;
*)
	echo "run: unknown verb '$verb' (try: master, dedi, stack, checks, docker, help)" >&2
	exit 2
	;;
esac
