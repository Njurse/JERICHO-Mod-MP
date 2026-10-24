#!/bin/sh
# BAKE_JMPPED - thin POSIX shim over jmpped_bake.py
#
# Extracts Driver 2's Tanner and Jericho peds, exports a per-ped
# OBJ/PNG/skeleton workspace and rebuilds JMPPED v3.
#
# Steps:  export | split | build | preview | all
# Examples:
#   ./BAKE_JMPPED.sh all
#   ./BAKE_JMPPED.sh export --source DRIVER2/LEVELS/HAVANA.LEV
#   ./BAKE_JMPPED.sh split --and-build
#   ./BAKE_JMPPED.sh build --copy
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if command -v python3 >/dev/null 2>&1; then
    PY=python3
elif command -v python >/dev/null 2>&1; then
    PY=python
else
    echo "JMPPED: Python 3 was not found - install it and put it on PATH." >&2
    exit 3
fi

exec "$PY" "$DIR/jmpped_bake.py" "$@"
