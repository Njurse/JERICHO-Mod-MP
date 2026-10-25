#!/usr/bin/env python3
"""jmpped_workspace.py - bake/source workspace paths, last-used state and backups.

The primed bake workspace holds `jmpped.json`, `entries/` and the archive being
edited. A source export holds the untouched Driver 2 extraction. They are
separate so refreshing the source cannot wipe the primed entries.

State lives under tools/out/jmppeds/:
    last_workspace.txt   selected primed bake workspace
    last_source.txt      selected source export workspace
    backups/<stamp>/     pre-reset/primes copies of jmpped.json + entries/
"""

from __future__ import annotations

import os
import shutil
import time

import jmpped_common as jc


HERE = os.path.dirname(os.path.abspath(__file__))
STATE_ROOT = os.path.join(HERE, "out", "jmppeds")
DEFAULT_BAKE = os.path.join(STATE_ROOT, "v3")
DEFAULT_SOURCE = os.path.join(STATE_ROOT, "source", "current")
LAST_BAKE = os.path.join(STATE_ROOT, "last_workspace.txt")
LAST_SOURCE = os.path.join(STATE_ROOT, "last_source.txt")
BACKUP_ROOT = os.path.join(STATE_ROOT, "backups")


def _read_last(path: str, fallback: str) -> str:
    try:
        with open(path, encoding="utf-8") as f:
            value = f.read().strip()
    except OSError:
        return fallback
    if not value:
        return fallback
    return os.path.abspath(value)


def _write_last(path: str, value: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(os.path.abspath(value) + "\n")


def bake_root(explicit: str | None = None) -> str:
    """Selected primed bake workspace (`--out` wins over the last-used file)."""
    if explicit:
        return os.path.abspath(explicit)
    return _read_last(LAST_BAKE, DEFAULT_BAKE)


def source_root(explicit: str | None = None) -> str:
    """Selected source export workspace (`--source-out`/wizard wins)."""
    if explicit:
        return os.path.abspath(explicit)
    return _read_last(LAST_SOURCE, DEFAULT_SOURCE)


def set_bake(path: str) -> None:
    _write_last(LAST_BAKE, path)


def set_source(path: str) -> None:
    _write_last(LAST_SOURCE, path)


def backup_workspace(root: str, reason: str) -> str | None:
    """Copy jmpped.json and the entries tree before a reset/prime.

    Returns the backup directory, or None when there is no manifest to protect.
    """
    root = os.path.abspath(root)
    manifest = os.path.join(root, "jmpped.json")
    if not os.path.isfile(manifest):
        return None

    stamp = time.strftime("%Y%m%d-%H%M%S")
    name = os.path.basename(root.rstrip("\\/")) or "workspace"
    dest = os.path.join(BACKUP_ROOT, "%s-%s" % (name, stamp))
    n = 1
    while os.path.exists(dest):
        dest = os.path.join(BACKUP_ROOT, "%s-%s-%d" % (name, stamp, n))
        n += 1

    os.makedirs(dest, exist_ok=True)
    shutil.copy2(manifest, os.path.join(dest, "jmpped.json"))
    entries = os.path.join(root, "entries")
    if os.path.isdir(entries):
        shutil.copytree(entries, os.path.join(dest, "entries"))
    with open(os.path.join(dest, "README.txt"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write("Pre-%s backup of %s\n" % (reason, root))
        f.write("jmpped.json and entries/ copied; source export is untouched.\n")
    return dest


def status(root: str, source: str | None = None) -> dict:
    """Small report used by the wizard and the status action."""
    root = os.path.abspath(root)
    manifest_path = os.path.join(root, "jmpped.json")
    out = {
        "workspace": root,
        "exists": os.path.isdir(root),
        "manifest": os.path.isfile(manifest_path),
        "entries": [],
        "base": None,
        "base_ok": None,
        "source": source or _read_last(LAST_SOURCE, DEFAULT_SOURCE),
        "source_exists": False,
        "source_mtime": None,
        "source_age_s": None,
        "prime_needed": None,
    }
    if not out["manifest"]:
        return out

    manifest = jc.load_json(manifest_path)
    out["entries"] = [e.get("name", "?") for e in manifest.get("entries", [])]
    out["base"] = manifest.get("base_archive")
    if out["base"]:
        base_path = os.path.join(root, out["base"].replace("/", os.sep))
        out["base_exists"] = os.path.isfile(base_path)
        if out["base_exists"]:
            out["base_ok"] = (jc.sha256_file(base_path) ==
                              manifest.get("base_archive_sha256"))
    src = source or manifest.get("source_workspace") or out["source"]
    out["source"] = src
    out["source_exists"] = os.path.isdir(src)
    if out["source_exists"]:
        src_manifest = os.path.join(src, "jmpped.json")
        src_time = (os.path.getmtime(src_manifest) if os.path.isfile(src_manifest)
                    else os.path.getmtime(src))
        out["source_mtime"] = src_time
        out["source_age_s"] = int(time.time() - src_time)
        out["prime_needed"] = src_time > os.path.getmtime(manifest_path)
    return out


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="Show the selected JMPPED workspaces")
    ap.add_argument("--bake", default=None)
    ap.add_argument("--source", default=None)
    args = ap.parse_args(argv)
    bake = bake_root(args.bake)
    source = source_root(args.source)
    st = status(bake, source)
    print("bake    :", bake)
    print("source  :", source, "(missing)" if not st["source_exists"] else "")
    print("manifest:", "yes" if st["manifest"] else "no")
    print("entries :", ", ".join(st["entries"]) if st["entries"] else "none")
    if st["base"]:
        print("base    :", st["base"], "ok" if st["base_ok"] else "MISSING/CHANGED")
    if st["source_exists"]:
        print("source age:", st["source_age_s"], "s; prime:",
              "needed" if st["prime_needed"] else "up to date")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
