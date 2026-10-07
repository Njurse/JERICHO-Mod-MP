#!/usr/bin/env bash
# Build the Micheal-shaped LAN package from the CURRENT tree.
#
# The same shape as tmp/make_lan_zip_micheal.sh beside it: NO DRIVER2\ (he has the
# game data), his name in mp.ini, PACKAGE_NOTES.txt at the root, and DRIVER\D1CARS\.
# What this one adds on top of that older script:
#
#   * the DEV exe staged as JERICHO.exe, not as JERICHO_dev.exe -- the launchers in
#     pack_lan/ now call JERICHO.exe (the public package's name), and the notes tell
#     him the layout is <somewhere>\JERICHO.exe.
#   * JERICHO\MODS\d1cars shipped out of the REPO: the dev mirror excludes d1cars and
#     gaildrv2 (premake JER_MIRROR_EXCLUDE), and the loader reads MODS\<id>\mod.toml
#     even for a compiled-in module (jer_loader.c) -- so without this the d1cars
#     module has no metadata to load with.
#   * a JERICHO\CONFIG\modlist.ini with SIX modules on: carhacks, crumple,
#     levelhacks, mp, d1cars, sandbox. (The dev tree's own modlist cannot be shipped
#     as-is: it also turns sandbox's neighbours on/off to taste.)
#   * this build's notes, as PACKAGE_NOTES.txt.
#
# NOT TRACKED, like the tmp/ helpers beside it: a one-off hand-delivery, not the
# public package. The tracked, public packager is
# JERICHO/MODS/mp/tools/pack_lan/make_lan_package.bat, which ships the RELEASE exe
# and cannot carry d1cars at all (premake JERICHO_RELEASE_MODS).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../../.." && pwd)"
EXE="$ROOT/src_rebuild/bin/Release_dev"
REPOMODS="$ROOT/JERICHO/MODS"
PACK="$ROOT/JERICHO/MODS/mp/tools/pack_lan"
REMOTE="$ROOT/JERICHO/MODS/mp/tools/remote"
SZ="/c/Program Files/7-Zip/7z.exe"
SHA=$(cd "$ROOT" && git rev-parse --short HEAD)
OUT="$ROOT/JERICHO_mp_lan_micheal_$SHA.zip"
STAGE="${TMPDIR:-/tmp}/micheal_stage_$SHA"

test -x "$SZ" || { echo "no 7-Zip"; exit 1; }
test -f "$EXE/JERICHO_dev.exe" || { echo "no dev exe"; exit 1; }
test -f "$REPOMODS/d1cars/mod.toml" || { echo "no d1cars module in the repo"; exit 1; }
test -d "$EXE/DRIVER/D1CARS" || { echo "no DRIVER/D1CARS in the dev tree"; exit 1; }

rm -f "$OUT"
rm -rf "$STAGE"
mkdir -p "$STAGE/JERICHO/MODS"

echo "== game: the dev exe, under the name the launchers use =="
cp "$EXE/JERICHO_dev.exe" "$STAGE/JERICHO.exe"
cd "$EXE"
# The exclusions are dev leftovers that must not reach him: .git is the d1cars
# submodule's gitlink, build.log / *.old / the modlist backup are this machine's
# own scratch.
"$SZ" a -tzip -mx=5 -xr'!.git' -xr'!build.log' -xr'!*.old' -xr'!modlist.ini.bak-*' "$OUT" \
    SDL2.dll OpenAL32.dll soft_oal.dll config.ini VERSION.txt JERICHO | tail -2
cd "$STAGE"
"$SZ" a -tzip -mx=5 "$OUT" JERICHO.exe | tail -2

echo "== the d1cars module folder (the dev mirror excludes it) =="
cp -r "$REPOMODS/d1cars" "$STAGE/JERICHO/MODS/d1cars"
# obj/lib are build output, tools is the offline bake pipeline: not runtime. The
# module is compiled into the exe; the loader only wants mod.toml (and the docs
# and sources ship because that is the shape the mirrored modules have). .git is
# the submodule's gitlink and must never ship.
rm -rf "$STAGE/JERICHO/MODS/d1cars/obj" "$STAGE/JERICHO/MODS/d1cars/lib" \
       "$STAGE/JERICHO/MODS/d1cars/tools" "$STAGE/JERICHO/MODS/d1cars/.git"
cd "$STAGE"
"$SZ" a -tzip -mx=5 "$OUT" 'JERICHO/MODS/d1cars' | tail -2

echo "== Driver 1 car data =="
cd "$EXE"
"$SZ" a -tzip -mx=5 "$OUT" 'DRIVER\D1CARS' | tail -2

echo "== launchers + readmes =="
cd "$PACK"
"$SZ" a -tzip -mx=5 "$OUT" PLAY_HOST.bat PLAY_JOIN.bat mp_bot_client.bat \
    FIREWALL_FIX.bat README_LAN.txt README_LAN_TEST.txt | tail -2

echo "== his notes, as PACKAGE_NOTES.txt =="
"$SZ" a -tzip -mx=5 "$OUT" PACKAGE_NOTES_micheal_d1cars.txt
# 7z stores the name we gave it; rename the member in place.
"$SZ" rn "$OUT" PACKAGE_NOTES_micheal_d1cars.txt PACKAGE_NOTES.txt

echo "== remote test agent =="
cd "$REMOTE"
"$SZ" a -tzip -mx=5 "$OUT" mp_agent.ps1 START_AGENT.bat README_REMOTE.txt | tail -2

echo "== staged mp.ini (his name) + modlist.ini (six modules on) =="
mkdir -p "$STAGE/JERICHO/CONFIG"
cp "$HERE/micheal_package/mp.ini" "$STAGE/JERICHO/CONFIG/mp.ini"
cp "$HERE/micheal_package/modlist.ini" "$STAGE/JERICHO/CONFIG/modlist.ini"
cd "$STAGE"
"$SZ" a -tzip -mx=5 "$OUT" 'JERICHO/CONFIG/mp.ini' 'JERICHO/CONFIG/modlist.ini' | tail -2

echo "== verify =="
"$SZ" t "$OUT" | tail -2
echo "-- the modlist that actually landed in the zip --"
"$SZ" e -so "$OUT" 'JERICHO/CONFIG/modlist.ini' | grep -v '^#' | grep .
echo "-- nothing that should not ship got in --"
if "$SZ" l "$OUT" | grep -aiE '(^|[\\/])\.git($|[\\/])|build\.log|modlist\.ini\.bak|combatd2\.ini\.old'; then
    echo "!! junk found in the package"; exit 1
else
    echo "ok: no .git, no build.log, no stale modlist backup"
fi
echo "-- the exe that landed, and that d1cars' mod.toml is beside it --"
"$SZ" l "$OUT" JERICHO.exe 'JERICHO/MODS/d1cars/mod.toml' 'DRIVER/D1CARS/*' | tail -6
echo "-- size --"
ls -la "$OUT"
