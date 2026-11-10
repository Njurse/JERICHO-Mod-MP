#!/usr/bin/env bash
# Fail if commit metadata credits a banned coding agent.
# Scans author, committer, trailers, and the message body only — not diffs.
set -euo pipefail

# Case-insensitive extended regex. Keep this list tight so a message about a
# text/mouse cursor does not trip the check.
PATTERN='cursor agent|cursoragent|cursor\[bot\]|@cursor\.com|cursor\.com|[Mm]ade with [Cc]ursor|[Cc]o-authored-by:.*[Cc]ursor'

usage() {
  cat <<'U'
Usage:
  no_cursor_attribution.sh                 infer a range (CI event or HEAD)
  no_cursor_attribution.sh <git-range>     scan commits in that range
  no_cursor_attribution.sh --commit-msg FILE
  no_cursor_attribution.sh --pre-push [remote url]   (stdin: pre-push lines)
U
}

die() { printf '%s\n' "$@" >&2; exit 1; }

scan_text() {
  local label=$1 text=$2
  if printf '%s\n' "$text" | grep -Eiq -- "$PATTERN"; then
    printf 'DISALLOWED ATTRIBUTION in %s:\n%s\n' "$label" "$text" >&2
    return 1
  fi
  return 0
}

scan_commit() {
  local sha=$1 ident subject body
  ident=$(git log -1 --format='author: %an <%ae>%ncommitter: %cn <%ce>' "$sha")
  subject=$(git log -1 --format='%s' "$sha")
  body=$(git log -1 --format='%B' "$sha")
  scan_text "commit $sha identity" "$ident" || return 1
  scan_text "commit $sha message" "$body" || return 1
  return 0
}

scan_range() {
  local range=$1 sha failed=0
  # Empty range (no commits) is a pass. --reverse is not required.
  local shas
  shas=$(git rev-list "$range" 2>/dev/null || true)
  if [[ -z "$shas" ]]; then
    return 0
  fi
  while IFS= read -r sha; do
    scan_commit "$sha" || failed=1
  done <<< "$shas"
  [[ $failed -eq 0 ]]
}

infer_ci_range() {
  local before after base head zero
  zero=0000000000000000000000000000000000000000
  case "${GITHUB_EVENT_NAME:-}" in
    pull_request)
      base=$(python3 -c 'import json,os; print(json.load(open(os.environ["GITHUB_EVENT_PATH"]))["pull_request"]["base"]["sha"])')
      head=$(python3 -c 'import json,os; print(json.load(open(os.environ["GITHUB_EVENT_PATH"]))["pull_request"]["head"]["sha"])')
      printf '%s\n' "${base}..${head}"
      ;;
    push)
      before=$(python3 -c 'import json,os; print(json.load(open(os.environ["GITHUB_EVENT_PATH"]))["before"])')
      after=$(python3 -c 'import json,os; print(json.load(open(os.environ["GITHUB_EVENT_PATH"]))["after"])')
      if [[ "$after" == "$zero" ]]; then
        # branch deletion — nothing to scan
        printf '\n'
        return 0
      fi
      if [[ "$before" == "$zero" ]]; then
        # new branch: commits not on the default branch, if present
        if git rev-parse --verify -q origin/main >/dev/null; then
          printf '%s\n' "origin/main..${after}"
        else
          printf '%s\n' "$after"
        fi
      else
        printf '%s\n' "${before}..${after}"
      fi
      ;;
    *)
      if git rev-parse --verify -q origin/main >/dev/null; then
        printf '%s\n' "origin/main..HEAD"
      else
        printf '%s\n' "HEAD"
      fi
      ;;
  esac
}

scan_commit_msg_hook() {
  local file=$1 msg ident
  msg=$(cat "$file")
  ident=$(
    printf 'author: %s <%s>\ncommitter: %s <%s>\n' \
      "${GIT_AUTHOR_NAME:-$(git var GIT_AUTHOR_IDENT | sed 's/ <.*//')}" \
      "${GIT_AUTHOR_EMAIL:-$(git var GIT_AUTHOR_IDENT | sed -n 's/.*<\([^>]*\)>.*/\1/p')}" \
      "${GIT_COMMITTER_NAME:-$(git var GIT_COMMITTER_IDENT | sed 's/ <.*//')}" \
      "${GIT_COMMITTER_EMAIL:-$(git var GIT_COMMITTER_IDENT | sed -n 's/.*<\([^>]*\)>.*/\1/p')}"
  )
  scan_text "pending author/committer" "$ident" || return 1
  scan_text "pending commit message" "$msg" || return 1
  return 0
}

scan_pre_push() {
  local local_ref local_sha remote_ref remote_sha zero range failed=0
  zero=0000000000000000000000000000000000000000
  while read -r local_ref local_sha remote_ref remote_sha; do
    [[ -n "${local_sha:-}" ]] || continue
    if [[ "$local_sha" == "$zero" ]]; then
      continue  # deleting a remote ref
    fi
    if [[ "$remote_sha" == "$zero" ]]; then
      if git rev-parse --verify -q origin/main >/dev/null; then
        range="origin/main..${local_sha}"
      else
        range="$local_sha"
      fi
    else
      range="${remote_sha}..${local_sha}"
    fi
    scan_range "$range" || failed=1
  done
  [[ $failed -eq 0 ]]
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

case "${1:-}" in
  --commit-msg)
    [[ -n "${2:-}" ]] || die "missing message file"
    scan_commit_msg_hook "$2"
    ;;
  --pre-push)
    scan_pre_push
    ;;
  "")
    range=$(infer_ci_range)
    if [[ -z "$range" ]]; then
      exit 0
    fi
    scan_range "$range"
    ;;
  *)
    scan_range "$1"
    ;;
esac
