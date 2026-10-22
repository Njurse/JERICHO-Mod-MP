#!/usr/bin/env bash
# ============================================================================
#  BAKE_D1_CARS.sh -- build the Driver 1 CAR files JERICHO reads, from YOUR
#                    own copy of Driver 1.
#
#  The Linux/macOS twin of BAKE_D1_CARS.bat: same five steps, same guards,
#  same output. Use whichever matches the machine you are on.
#
#  JERICHO does NOT ship Driver 1's content -- it is copyrighted. The seven retail
#  Driver 1 car-data cities (MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE, IVIEW, TRAIN)
#  read files baked OUT of it, so this script makes them from data you already
#  own. If you also have the Euro Demo 43, Beta Miami (BETAMIAMI) comes along.
#
#  WHAT TO DO
#    1. Put your Driver 1 game data in DRIVER/, beside DRIVER2/ -- the same
#       folder layout the game itself uses, so DRIVER/LEVELS/*.LEV is present.
#       For Beta Miami, put the Euro Demo 43 LEVELS and SOUND folders in
#       EURO43/ (its level is EURO43/LEVELS/MIAMI.LEV).
#    2. Run this script once. It writes JERICHO/MODS/carhacks/D1CARS/.
#    3. Play. carhacks offers the cities from then on.
#
#  The baked blobs live INSIDE carhacks: they are its modded level data, not
#  part of your Driver 1 disc, and the game reads them from there. The folder is
#  generated, gitignored, and never part of a release.
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
# The dev tree's game folder (src_rebuild/bin/<config>) does not carry the
# data-only d1cars module, so fall back to the repo's copy three levels up.
# A shipped package has the tools beside the script and takes the first path.
if [ ! -f "$T/bake.py" ] && [ -f "$HERE/../../../JERICHO/MODS/d1cars/tools/bake.py" ]; then
	T="$HERE/../../../JERICHO/MODS/d1cars/tools"
fi
LEV="DRIVER/LEVELS"
D2="DRIVER2/LEVELS"
OUT="JERICHO/MODS/carhacks/D1CARS"
CITIES="MIAMI,FRISCO,LA,NEWYORK,NEWCASTLE,IVIEW,TRAIN"
BETA=0

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

# Euro Demo 43 is optional. Detect it once, and let the player see that we found
# it: Beta Miami only exists in that leaked beta build.
if [ -f "EURO43/LEVELS/MIAMI.LEV" ]; then
	BETA=1
	echo "  * Euro Demo 43 detected in EURO43/ -- Beta Miami is joining the garage."
	echo
elif [ -f "EURO43/LEVELS/BETAMIAMI.LEV" ]; then
	BETA=1
	echo "  * Euro Demo 43 detected in EURO43/ (renamed BETAMIAMI.LEV) -- Beta Miami is joining the garage."
	echo
else
	echo "  (no EURO43/LEVELS/MIAMI.LEV -- Beta Miami stays parked; baking the seven retail cities)"
	echo
fi
if [ "$BETA" = 1 ]; then
	CITIES="$CITIES,BETAMIAMI"
fi

# BAKE_VERSION: bump when the bake gains something the game reads. 8 moved the
# output into carhacks and made the EURO-43 beta read the demo's MIAMI.LEV.
BAKE_VERSION=8
if [ -d "$OUT" ] && [ "$(cat "$OUT/BAKE_VERSION" 2>/dev/null)" != "$BAKE_VERSION" ]; then
	echo "Your existing $OUT is from an older bake (no D1 wheels/sounds, pre-Miami wheels, unpacked palette rows, or the old DRIVER/D1CARS location) -- rebuilding it."
	echo
elif [ ! -d "$OUT" ] && [ -d "DRIVER/D1CARS" ]; then
	echo "Found your old bake in DRIVER/D1CARS -- rebuilding it inside carhacks, where the game now reads it."
	echo
fi

mkdir -p "$OUT"

# The pool first: every distinct traffic shape in your Driver 1 levels.
echo "[1/5] reading Driver 1's car shapes ..."
"$PY" "$T/d1pool.py" --levels "$LEV" || fail "d1pool.py failed"

# One blob per city -- each is a complete small "level" holding that city's
# cars, their palette and their texture pages.
echo "[2/5] baking one blob per city ..."
for C in MIAMI FRISCO LA NEWYORK NEWCASTLE IVIEW TRAIN; do
	"$PY" "$T/bake.py" --pool "$T/out/pool.json" --levels "$LEV" --city "$C" --out "$OUT" || fail "bake.py $C failed"
done
if [ "$BETA" = 1 ]; then
	"$PY" "$T/bake.py" --pool "$T/out/pool.json" --levels "$LEV" --city BETAMIAMI --out "$OUT" || fail "bake.py BETAMIAMI failed"
fi

# The cosmetics: a .LCF per city, carrying the per-car wheel placement. Driver
# 2's own files are the donor, so these describe YOUR copy's proportions.
echo "[3/5] writing the car cosmetics ..."
"$PY" "$T/cosmetics1.py" --levels "$LEV" --donor "$D2" --cities "$CITIES" --write "$OUT" || fail "cosmetics1.py failed"

# The sounds: Driver 1's engine/idle/horn, from YOUR DRIVER/SOUND/VOICES.BLK,
# patched into each freshly baked city. BETAMIAMI uses the Euro-43 beta's own
# SOUND.SBK when it is present (pitched-down traffic idle + acceleration loop).
echo "[4/5] adding the car sounds ..."
if [ -f "DRIVER/SOUND/VOICES.BLK" ]; then
	for C in MIAMI FRISCO LA NEWYORK NEWCASTLE IVIEW TRAIN; do
		"$PY" "$T/add_car_sounds.py" --lev "$OUT/$C.LEV" --voices "DRIVER/SOUND/VOICES.BLK" --city "$C" || fail "add_car_sounds.py $C failed"
	done
else
	echo "  (no DRIVER/SOUND/VOICES.BLK -- Driver 1 cars will use the host city's sounds)"
fi
if [ "$BETA" = 1 ]; then
	if [ -f "EURO43/SOUND/SOUND.SBK" ]; then
		"$PY" "$T/add_car_sounds.py" --lev "$OUT/BETAMIAMI.LEV" --voices "EURO43/SOUND/SOUND.SBK" --city BETAMIAMI || fail "add_car_sounds.py BETAMIAMI failed"
	elif [ -f "DRIVER/SOUND/VOICES.BLK" ]; then
		"$PY" "$T/add_car_sounds.py" --lev "$OUT/BETAMIAMI.LEV" --voices "DRIVER/SOUND/VOICES.BLK" --city BETAMIAMI || fail "add_car_sounds.py BETAMIAMI failed"
	else
		echo "  (no EURO43/SOUND/SOUND.SBK or DRIVER/SOUND/VOICES.BLK -- BETAMIAMI will use the host city's sounds)"
	fi
fi

# Beta Miami is a runtime BANK, not one of the seven compile-time rows. The engine
# registers it automatically from the baked file; this cities.ini row is also
# written for older builds and to pin the slot. Add it idempotently if we baked it.
if [ "$BETA" = 1 ]; then
	echo "[5/5] registering Beta Miami with the city registry ..."
	REG=""
	for cand in "$HERE/d1city_register.py" "$HERE/tools/d1city_register.py" "$HERE/../../../tools/d1city_register.py"; do
		if [ -f "$cand" ]; then REG="$cand"; break; fi
	done
	if [ -n "$REG" ]; then
		"$PY" "$REG" --install "$HERE" || fail "d1city_register.py failed"
	else
		echo "  (no d1city_register.py -- Beta Miami is baked, but add its row to"
		echo "   JERICHO/CONFIG/cities.ini by hand to have the game offer it)"
	fi
else
	echo "[5/5] no Euro Demo 43 -- nothing to register for Beta Miami."
fi

echo "$BAKE_VERSION" > "$OUT/BAKE_VERSION"

echo
echo "BAKE_D1_CARS: done. $OUT now holds:"
ls -1 "$OUT"
echo
if [ "$BETA" = 1 ] && [ -f "$OUT/BETAMIAMI.LEV" ]; then
	echo "  Beta Miami is in. Go enjoy your favourite yellow Buick GX."
else
	echo "  Start the game and the seven retail Driver 1 cities will be offered."
	echo "  (Put the Euro Demo 43's LEVELS and SOUND in EURO43/ and re-run for Beta Miami.)"
fi
