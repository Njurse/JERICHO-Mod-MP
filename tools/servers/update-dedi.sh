#!/usr/bin/env bash
# update-dedi.sh -- bring the dedicated server up to whatever JERICHO main pins. One command.
#
#   bash ~/JERICHO/JERICHO/MODS/mp/tools/servers/update-dedi.sh     (or: JERICHO_DIR=/path ./update-dedi.sh)
#
# What it does, every time (safe to re-run; already-current just rebuilds the same thing):
#   1. superproject: switch to `main` if it is detached, then fast-forward pull
#   2. mp submodule: check out EXACTLY the commit main pins (git submodule update)
#   3. print the mp commit + the SDK/PROTO the dedi will advertise
#   4. docker compose build --no-cache dedi && up -d --force-recreate dedi
#   5. show SDK/PROTO from INSIDE the container + recent log/mismatch lines
#
# "You are not currently on a branch" inside JERICHO/MODS/mp is NORMAL: a submodule is
# pinned to one commit, not a branch. Never `git pull` inside mp -- run this instead.
# Your dediserver.json (untracked) is never touched; dediserver.py is still the entrypoint.
set -euo pipefail

main() {   # whole body in a function: bash parses it all before step 2 rewrites this file
  local here root branch="${BRANCH:-main}"
  here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  root="${JERICHO_DIR:-$(git -C "$here" rev-parse --show-superproject-working-tree 2>/dev/null || true)}"
  root="${root:-$HOME/JERICHO}"
  [ -d "$root/.git" ] || [ -f "$root/.git" ] || { echo "!! no JERICHO checkout at $root (set JERICHO_DIR)"; exit 1; }
  local mp="$root/JERICHO/MODS/mp" srv="$root/JERICHO/MODS/mp/tools/servers"
  cd "$root"

  echo "== JERICHO: $root"
  if [ -n "$(git status --porcelain --untracked-files=no --ignore-submodules=all)" ]; then
    echo "!! uncommitted edits in the JERICHO checkout -- stash or discard them first:"; git status --short --ignore-submodules=all; exit 1
  fi
  git fetch --quiet origin
  if [ "$(git symbolic-ref -q --short HEAD || true)" != "$branch" ]; then
    echo "   not on $branch (was: $(git symbolic-ref -q --short HEAD || echo detached @ $(git rev-parse --short HEAD))) -> switching"
    git checkout --quiet "$branch" 2>/dev/null || git checkout --quiet -b "$branch" "origin/$branch"
  fi
  git merge --ff-only --quiet "origin/$branch" || {
    echo "!! local $branch has commits origin doesn't. To just match origin: git -C $root reset --hard origin/$branch"; exit 1; }
  echo "   $branch @ $(git rev-parse --short HEAD)"

  echo "== mp submodule"
  if [ -n "$(git -C "$mp" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    echo "!! edits inside mp would be overwritten -- keep or drop them first:"; git -C "$mp" status --short; exit 1
  fi
  git submodule sync --quiet -- JERICHO/MODS/mp
  git submodule update --init --quiet -- JERICHO/MODS/mp
  echo "   pinned @ $(git -C "$mp" rev-parse --short HEAD)  (detached = correct)"
  grep -nE '^(SDK|PROTO) *=' "$mp/tools/mp_test.py" | sed 's/ *#.*//; s/^/   host  /'

  [ -f "$srv/dediserver/dediserver.json" ] || {
    echo "!! $srv/dediserver/dediserver.json missing -- cp dediserver/dediserver.example.json dediserver/dediserver.json and edit"; exit 1; }

  echo "== rebuild + restart dedi"
  cd "$srv"
  docker compose build --no-cache dedi
  docker compose up -d --force-recreate dedi
  sleep 3
  docker compose exec -T dedi grep -E '^(SDK|PROTO) *=' /mp/tools/mp_test.py | sed 's/ *#.*//; s/^/   container  /' || true
  echo "== last log lines (mismatch/refuse highlighted)"
  docker compose logs --tail 30 dedi | grep -iE --color=auto 'mismatch|refus|sdk|proto|$' || true
  echo "== done. Client 'sdk A/B' refuse: A = player's game, B = this server."
}
main "$@"
