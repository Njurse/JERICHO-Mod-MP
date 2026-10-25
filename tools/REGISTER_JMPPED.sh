#!/bin/sh
# Register a completed JMPPED entry folder and bake it.
set -e
[ $# -ge 1 ] || { echo "usage: $0 <entry-folder>" >&2; exit 1; }
exec python "$(dirname "$0")/jmpped_register.py" --folder "$1"
