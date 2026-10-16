#!/bin/sh
# ============================================================================
#  update_jericho.sh -- update this JERICHO install from the latest RELEASE.
#
#  Run it from the folder the game is installed in. It fetches the newest
#  published release -- NOT the rolling "alpha" pre-release, which is a build of
#  whatever is on main right now and is not meant to be played -- verifies its
#  checksum, and copies it over this install.
#
#  WHAT IT WILL NOT TOUCH
#    CONFIG/    your module toggles and settings: modlist.ini, carhacks.ini,
#               mp.ini, crumple.ini, config.ini. Never written.
#    config.ini your window size and fullscreen settings. This is the one at the
#               install root (the release ships a default), NOT CONFIG/config.ini.
#               Never written.
#    DRIVER/    your baked Driver 1 car data, if you have any. Never written.
#    DRIVER2/   your own game data (levels, models, cutscenes). Never written.
#    MODS/<x>/  any module you added yourself. Files are only ever overwritten,
#               never deleted, so nothing of yours can disappear.
#  It only ever overwrites files the release itself carries.
#
#  USAGE
#    ./update_jericho.sh          the latest release
#    ./update_jericho.sh dev      the latest release's Release_dev build, which
#                                 carries every module and prints more
#
#  NEEDS: curl or wget, and tar.
# ============================================================================
set -eu

cd "$(dirname "$0")" || exit 1
HERE=$(pwd)

REPO=Njurse/JERICHO
ASSET=JERICHO_Release_linux-x64.tar.gz
KIND=release
if [ "${1:-}" = "dev" ]; then
	ASSET=JERICHO_Release_dev_linux-x64.tar.gz
	KIND="release dev"
fi

# releases/latest/download resolves to the newest NON-prerelease release, so
# alpha is excluded without asking the API and without parsing any JSON.
BASE="https://github.com/$REPO/releases/latest/download"
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

say_fail() {
	echo "  FAILED: $1" >&2
	echo "  Nothing was changed." >&2
	echo "  You can also download by hand from https://github.com/$REPO/releases/latest" >&2
	exit 1
}

fetch() {
	# $1 = url, $2 = destination
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL -o "$2" "$1" && return 0
	elif command -v wget >/dev/null 2>&1; then
		wget -q -O "$2" "$1" && return 0
	else
		say_fail "neither curl nor wget is installed, so the release cannot be fetched."
	fi
	return 1
}

echo "update_jericho: updating this install from the latest $KIND"
echo "  folder: $HERE"
echo

echo "[1/5] downloading $ASSET ..."
fetch "$BASE/$ASSET" "$TMP/$ASSET" || say_fail "could not download $ASSET."
echo "      $(wc -c < "$TMP/$ASSET") bytes"

# Report the version, best effort: the tag is only a nicety, never a gate.
if command -v curl >/dev/null 2>&1; then
	TAG=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
		| sed -n 's/.*"tag_name" *: *"\([^"]*\)".*/\1/p' | head -1) || TAG=
	[ -n "${TAG:-}" ] && echo "      release: $TAG"
fi

echo "[2/5] downloading SHA256SUMS ..."
fetch "$BASE/SHA256SUMS" "$TMP/SHA256SUMS" \
	|| say_fail "could not download SHA256SUMS, so the download cannot be verified."

echo "[3/5] verifying the checksum ..."
# Accept both "hash  name" and "hash *name": GNU sha256sum uses a leading * for
# binary mode, and refusing a perfectly good release over a separator would be a
# worse failure than the one this check exists to catch.
WANT=$(awk -v a="$ASSET" '{ n=$2; sub(/^\*/, "", n); if (n == a) { print $1; exit } }' \
	"$TMP/SHA256SUMS")
[ -n "$WANT" ] || say_fail "SHA256SUMS has no line for $ASSET."
if command -v sha256sum >/dev/null 2>&1; then
	GOT=$(sha256sum "$TMP/$ASSET" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
	GOT=$(shasum -a 256 "$TMP/$ASSET" | cut -d' ' -f1)
else
	say_fail "no sha256sum or shasum to verify with, so the download cannot be trusted."
fi
[ "$WANT" = "$GOT" ] || say_fail "checksum mismatch -- the download is not the published file.
    published: $WANT
    yours:     $GOT"
echo "      ok: $GOT"

echo "[4/5] unpacking ..."
mkdir -p "$TMP/x" || say_fail "could not create a temporary folder."
tar -xzf "$TMP/$ASSET" -C "$TMP/x" || say_fail "could not unpack the release."

echo "[5/5] copying over this install, leaving CONFIG/, config.ini, DRIVER/ and DRIVER2/ alone ..."
# Directories first so an empty one the release carries still exists.
( cd "$TMP/x" && find . -type d -exec mkdir -p "$HERE/{}" \; ) \
	|| say_fail "could not create the release's folders."
# Every file EXCEPT the ones under the directories that must never be written,
# and except the ROOT config.ini: it holds the player's own window size and
# fullscreen, and the release carries a default one (from data/). CONFIG/ is kept
# whole above, so nothing else called config.ini is affected.
# DRIVER/ and DRIVER2/ are absent from a release already, but the belt keeps
# them safe if one ever ships data there again.
copied=$( cd "$TMP/x" && find . -type f ! -path './JERICHO/CONFIG/*' ! -path './DRIVER/*' ! -path './DRIVER2/*' ! -path './config.ini' | wc -l )
( cd "$TMP/x" && find . -type f ! -path './JERICHO/CONFIG/*' ! -path './DRIVER/*' ! -path './DRIVER2/*' ! -path './config.ini' -exec cp -f {} "$HERE/{}" \; ) \
	|| say_fail "could not copy the release over this install."

# cp -f does not carry the mode; the things you run need to stay runnable.
[ -f "$HERE/jericho" ] && chmod +x "$HERE/jericho"
for s in BAKE_D1_CARS.sh update_jericho.sh; do
	[ -f "$HERE/$s" ] && chmod +x "$HERE/$s"
done

echo
echo "update_jericho: done. $copied file(s) written."
echo "  updated:  the game, JERICHO/ and its released modules"
echo "  kept:     CONFIG/   your toggles and settings"
echo "            config.ini  your window size and fullscreen"
echo "            DRIVER/   your baked Driver 1 car data"
echo "            DRIVER2/  your own game data (levels, models, cutscenes)"
echo "            any module you added under MODS/"
echo
echo "Note: files are overwritten, never deleted, so an old file this release no"
echo "longer ships may still be here. That is what keeps your own mods safe."
echo "Turn crumple and Caine's Crossfire on in Options - JERICHO if you want them."
