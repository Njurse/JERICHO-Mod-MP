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
#               Never replaced: a key the release adds that yours lacks is appended,
#               and every value you already have is left alone.
#    DRIVER/    your baked Driver 1 car data, if you have any. Never written.
#    DRIVER2/   your own game data (levels, models, cutscenes). Never written.
#    MODS/<x>/  any module you added yourself. Files are only ever overwritten,
#               never deleted, so nothing of yours can disappear.
#  It only ever overwrites files the release itself carries.
#
#  USAGE
#    ./update_jericho.sh          the latest release, in this install's flavour
#                                 (Release_dev if JERICHO/CONFIG/installed.json
#                                 says so, else Release)
#    ./update_jericho.sh dev      the latest release's Release_dev build, which
#                                 carries every module and prints more
#    ./update_jericho.sh release  the plain Release build, whatever is installed
#
#  The game's "Update JERICHO (Restart)" button adds (see update_jericho.bat):
#    --tag vX.Y.Z  that exact release   --wait PID  wait for the game to exit
#    --relaunch EXE  start the game again afterwards (after a failure too)
#  and records the result in JERICHO/CONFIG/installed.json (tag, asset, SHA256).
#
#  NEEDS: curl or wget, and tar.
# ============================================================================
set -eu

# Run from a COPY. The release carries this script too, and sh reads a script as
# it goes, so the copy step overwriting it mid-run ended the run with a syntax
# error. The copy is told where the install is (--here) and removes itself.
if [ "${1:-}" != "--here" ]; then
	cd "$(dirname "$0")" || exit 1
	self=$(mktemp) || exit 1
	cp "$0" "$self" || exit 1
	exec sh "$self" --here "$(pwd)" "$@"
fi
SELF=$0
cd "$2" || exit 1
shift 2
HERE=$(pwd)

REPO=Njurse/JERICHO
FLAVOUR= TAG= WAITPID= RELAUNCH=
while [ $# -gt 0 ]; do
	case "$1" in
		dev|release) FLAVOUR=$1 ;;
		--tag) TAG=${2:-}; shift ;;
		--wait) WAITPID=${2:-}; shift ;;
		--relaunch) RELAUNCH=${2:-}; shift ;;
		*) echo "update_jericho: ignoring unknown argument $1" ;;
	esac
	[ $# -gt 0 ] && shift
done
# No flavour asked for: keep the one this install was last updated to.
if [ -z "$FLAVOUR" ] && grep -q Release_dev JERICHO/CONFIG/installed.json 2>/dev/null; then
	FLAVOUR=dev
fi
ASSET=JERICHO_Release_linux-x64.tar.gz
KIND=release
if [ "$FLAVOUR" = "dev" ]; then
	ASSET=JERICHO_Release_dev_linux-x64.tar.gz
	KIND="release dev"
fi

# releases/latest/download resolves to the newest NON-prerelease release, so
# alpha is excluded without asking the API and without parsing any JSON. With
# --tag (the game's button) it is that exact release.
BASE="https://github.com/$REPO/releases/latest/download"
if [ -n "$TAG" ]; then
	BASE="https://github.com/$REPO/releases/download/$TAG"
	KIND="$KIND $TAG"
fi

# The button starts us while the game is still closing: wait (about a minute at
# most) so its files can be replaced. Relaunch on ANY exit -- after a failure
# nothing was changed, so the old build still runs.
if [ -n "$WAITPID" ]; then
	echo "waiting for JERICHO (pid $WAITPID) to close ..."
	n=0
	while kill -0 "$WAITPID" 2>/dev/null && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
fi
relaunch() {
	if [ -n "$RELAUNCH" ] && [ -x "$RELAUNCH" ]; then
		echo "Starting JERICHO again ..."
		( cd "$HERE" && nohup "$RELAUNCH" >/dev/null 2>&1 & )
	fi
}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"; rm -f "$SELF"; relaunch' EXIT
trap 'exit 1' INT TERM

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
#
# The DEDICATED SERVER's config is the OPERATOR's -- port, name, city, player
# cap, whether it beacons -- so an update must not replace one that is there.
# It is excluded from the copy and INSTALLED ONLY when this machine has none,
# just below: the release's copy is a default, not a replacement.
DEDI_CFG_REL='./JERICHO/MODS/mp/tools/servers/dediserver/dediserver.json'
copied=$( cd "$TMP/x" && find . -type f ! -path './JERICHO/CONFIG/*' ! -path './DRIVER/*' ! -path './DRIVER2/*' ! -path './config.ini' ! -path "$DEDI_CFG_REL" | wc -l )
( cd "$TMP/x" && find . -type f ! -path './JERICHO/CONFIG/*' ! -path './DRIVER/*' ! -path './DRIVER2/*' ! -path './config.ini' ! -path "$DEDI_CFG_REL" -exec cp -f {} "$HERE/{}" \; ) \
	|| say_fail "could not copy the release over this install."

# The dedicated server's default config, installed ONCE: an operator who has one
# keeps it exactly as it is, and a fresh install still gets a file to edit. Newer
# releases ship only dediserver.example.json (the server creates dediserver.json
# from it on first start), so this only fires for an older package carrying one.
DEDI_CFG="$HERE/JERICHO/MODS/mp/tools/servers/dediserver/dediserver.json"
if [ ! -f "$DEDI_CFG" ] && [ -f "$TMP/x/$DEDI_CFG_REL" ]; then
	mkdir -p "$(dirname "$DEDI_CFG")"
	cp "$TMP/x/$DEDI_CFG_REL" "$DEDI_CFG"
fi

# config.ini is never REPLACED - it holds your window size, fullscreen, binds and options -
# but a release may carry a key yours has never seen, so add just those and leave everything
# you already have alone. Your value always wins: the game's ini reader (utils/ini.c)
# returns the FIRST match for a section+key, so a duplicate could not override one of yours.
merge_config_keys() {
	# a release ships config.default.ini (build.yml); an older one config.ini
	rel="$TMP/x/config.default.ini"
	[ -f "$rel" ] || rel="$TMP/x/config.ini"
	usr="$HERE/config.ini"

	[ -f "$rel" ] || return 0

	if [ ! -f "$usr" ]; then
		cp -f "$rel" "$usr"
		return 0
	fi

	awk '
		function skey(l) { sub(/[ \t]*=.*/, "", l); gsub(/[ \t\r]/, "", l); return tolower(l) }
		function sect(l) { sub(/[ \t\r].*$/, "", l); return l }
		FNR == NR {				# $1 = the release, as the template
			if ($0 ~ /^[ \t]*\[/) s = sect($0)
			else if ($0 ~ /^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=/) {
				n++; tk[n] = s "|" skey($0); tl[n] = $0
			}
			next
		}
		{					# $2 = your file, echoed through unchanged
			if ($0 ~ /^[ \t]*\[/) s = sect($0)
			else if ($0 ~ /^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=/) have[s "|" skey($0)] = 1
			print
		}
		END {
			for (i = 1; i <= n; i++) {
				if (tk[i] in have) continue
				have[tk[i]] = 1
				split(tk[i], a, "|")
				if (a[1] != opened) { opened = a[1]; print ""; print opened }
				print tl[i]
			}
		}
	' "$rel" "$usr" > "$usr.jericho-merge" && mv -f "$usr.jericho-merge" "$usr"
}
merge_config_keys

# cp -f does not carry the mode; the things you run need to stay runnable.
[ -f "$HERE/jericho" ] && chmod +x "$HERE/jericho"
for s in BAKE_D1_CARS.sh update_jericho.sh; do
	[ -f "$HERE/$s" ] && chmod +x "$HERE/$s"
done

# What is installed now: the game reads this for re-cut detection and flavour.
mkdir -p "$HERE/JERICHO/CONFIG"
printf '{\n  "tag": "%s",\n  "asset": "%s",\n  "sha256": "%s"\n}\n' "$TAG" "$ASSET" "$GOT" \
	> "$HERE/JERICHO/CONFIG/installed.json"

echo
echo "update_jericho: done. $copied file(s) written."
echo "  updated:  the game, JERICHO/ and its released modules"
echo "  kept:     CONFIG/   your toggles and settings"
echo "            config.ini  your window size and fullscreen (new keys added, yours kept)"
echo "            dediserver.json  your dedicated server's own settings"
echo "            DRIVER/   your baked Driver 1 car data"
echo "            DRIVER2/  your own game data (levels, models, cutscenes)"
echo "            any module you added under MODS/"
echo
echo "Note: files are overwritten, never deleted, so an old file this release no"
echo "longer ships may still be here. That is what keeps your own mods safe."
echo "Turn crumple and Caine's Crossfire on in Options - JERICHO if you want them."
