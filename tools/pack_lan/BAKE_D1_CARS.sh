#!/usr/bin/env bash
# ============================================================================
#  BAKE_D1_CARS.sh -- build the Driver 1 CAR files JERICHO reads, from YOUR
#                    own copy of Driver 1.
#
#  The Linux/macOS twin of BAKE_D1_CARS.bat: same four steps, same guards,
#  same output. Use whichever matches the machine you are on.
#
#  JERICHO does NOT ship Driver 1's content -- it is copyrighted. The seven
#  Driver 1 car-data cities (MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE, IVIEW, TRAIN) read files
#  baked OUT of it, so this script makes them from data you already own.
#
#  WHAT TO DO
#    1. Put your Driver 1 game data in DRIVER/, beside DRIVER2/ -- the same
#       folder layout the game itself uses, so DRIVER/LEVELS/*.LEV is present.
#    2. Run this script once. It writes DRIVER/D1CARS/.
#    3. Play. carhacks offers the seven cities from then on.
#
#  Re-run it after updating JERICHO if the game says D1CARS is out of date
#  (bakes from before v0.10.9 lack the D1 wheels and sounds). Re-running is safe.
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
CITIES="MIAMI,FRISCO,LA,NEWYORK,NEWCASTLE,IVIEW,TRAIN"

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

# BAKE_VERSION: bump when the bake gains something the game reads
BAKE_VERSION=5
if [ -d "$OUT" ] && [ "$(cat "$OUT/BAKE_VERSION" 2>/dev/null)" != "$BAKE_VERSION" ]; then
	echo "Your existing $OUT is from an older bake (no D1 wheels/sounds, or pre-Miami wheels) -- rebuilding it."
	echo
fi

mkdir -p "$OUT"

# The pool first: every distinct traffic shape in your Driver 1 levels.
echo "[1/4] reading Driver 1's car shapes ..."
"$PY" "$T/d1pool.py" --levels "$LEV" || fail "d1pool.py failed"

# One blob per city -- each is a complete small "level" holding that city's
# cars, their palette and their texture pages.
echo "[2/4] baking one blob per city ..."
for C in MIAMI FRISCO LA NEWYORK NEWCASTLE IVIEW TRAIN; do
	"$PY" "$T/bake.py" --pool "$T/out/pool.json" --levels "$LEV" --city "$C" --out "$OUT" || fail "bake.py $C failed"
done

# The cosmetics: a .LCF per city, carrying the per-car wheel placement. Driver
# 2's own files are the donor, so these describe YOUR copy's proportions.
echo "[3/4] writing the car cosmetics ..."
"$PY" "$T/cosmetics1.py" --levels "$LEV" --donor "$D2" --cities "$CITIES" --write "$OUT" || fail "cosmetics1.py failed"

# The sounds: Driver 1's engine/idle/horn, from YOUR DRIVER/SOUND/VOICES.BLK,
# patched into each freshly baked city.
echo "[4/4] adding the car sounds ..."
if [ -f "DRIVER/SOUND/VOICES.BLK" ]; then
	for C in MIAMI FRISCO LA NEWYORK NEWCASTLE IVIEW TRAIN; do
		"$PY" "$T/add_car_sounds.py" --lev "$OUT/$C.LEV" --voices "DRIVER/SOUND/VOICES.BLK" || fail "add_car_sounds.py $C failed"
	done
else
	echo "  (no DRIVER/SOUND/VOICES.BLK -- Driver 1 cars will use the host city's sounds)"
fi

echo "$BAKE_VERSION" > "$OUT/BAKE_VERSION"

echo
echo "BAKE_D1_CARS: done. $OUT now holds:"
ls -1 "$OUT"
echo
echo "Start the game and the seven Driver 1 cities will be offered."
