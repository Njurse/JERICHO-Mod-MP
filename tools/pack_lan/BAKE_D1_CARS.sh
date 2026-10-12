#!/usr/bin/env bash
# ============================================================================
#  BAKE_D1_CARS.sh -- build the Driver 1 CAR files JERICHO reads, from YOUR
#                    own copy of Driver 1.
#
#  The Linux/macOS twin of BAKE_D1_CARS.bat: same three steps, same guards,
#  same output. Use whichever matches the machine you are on.
#
#  JERICHO does NOT ship Driver 1's content -- it is copyrighted. The five
#  Driver 1 car-data cities (MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE) read files
#  baked OUT of it, so this script makes them from data you already own.
#
#  WHAT TO DO
#    1. Put your Driver 1 game data in DRIVER/, beside DRIVER2/ -- the same
#       folder layout the game itself uses, so DRIVER/LEVELS/*.LEV is present.
#    2. Run this script once. It writes DRIVER/D1CARS/.
#    3. Play. carhacks offers the five cities from then on.
#
#  You do not need this if DRIVER/D1CARS already exists. Re-running is safe.
#
#  NEEDS: python3 on PATH; your Driver 2 data in DRIVER2/ (the wheel geometry
#  and ride height are derived from Driver 2's own car files, not guessed).
# ============================================================================
set -eu

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE"

T="$HERE/JERICHO/MODS/d1cars/tools"
LEV="DRIVER/LEVELS"
D2="DRIVER2/LEVELS"
OUT="DRIVER/D1CARS"
CITIES="MIAMI,FRISCO,LA,NEWYORK,NEWCASTLE"

fail() {
	echo
	echo "BAKE_D1_CARS: FAILED -- $1"
	echo "Nothing was changed in a way you cannot re-run."
	exit 1
}

echo "BAKE_D1_CARS: baking Driver 1 car data into $OUT"
echo

if [ ! -f "$T/bake.py" ]; then
	fail "\"$T/bake.py\" is missing. This package was built without the d1cars tools, so there is nothing to bake with. Re-download the package, or run the tools from the repo."
fi

if [ ! -d "$LEV" ]; then
	fail "no \"$LEV\". Put your Driver 1 game data in DRIVER/ first -- the layout the game uses, so that DRIVER/LEVELS/*.LEV exists."
fi

if [ ! -d "$D2" ]; then
	fail "no \"$D2\". The bake measures its wheels and ride height against Driver 2's own car files, so DRIVER2/ has to be the real game data too."
fi

PY=""
for cand in python3 python; do
	if command -v "$cand" >/dev/null 2>&1; then PY="$cand"; break; fi
done
if [ -z "$PY" ]; then
	fail "no python on PATH. Install Python 3, then run this again."
fi

mkdir -p "$OUT"

# The pool first: every distinct traffic shape in your Driver 1 levels.
echo "[1/3] reading Driver 1's car shapes ..."
"$PY" "$T/d1pool.py" --levels "$LEV" || fail "d1pool.py failed"

# One blob per city -- each is a complete small "level" holding that city's
# cars, their palette and their texture pages.
echo "[2/3] baking one blob per city ..."
for C in MIAMI FRISCO LA NEWYORK NEWCASTLE; do
	"$PY" "$T/bake.py" --pool "$T/out/pool.json" --levels "$LEV" --city "$C" --out "$OUT" || fail "bake.py $C failed"
done

# The cosmetics: a .LCF per city, carrying the per-car wheel placement. Driver
# 2's own files are the donor, so these describe YOUR copy's proportions.
echo "[3/3] writing the car cosmetics ..."
"$PY" "$T/cosmetics1.py" --levels "$LEV" --donor "$D2" --cities "$CITIES" --write "$OUT" || fail "cosmetics1.py failed"

echo
echo "BAKE_D1_CARS: done. $OUT now holds:"
ls -1 "$OUT"
echo
echo "Start the game and the five Driver 1 cities will be offered."
