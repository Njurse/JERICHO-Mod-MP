#!/usr/bin/env python3
"""jmpped_prime.py - copy a source export into the primed bake workspace.

Only source-owned assets are replaced: base archives, pages, skeleton, the
source directories and entry 0. Custom/external entries and the existing
JMPPED.LEV are preserved. The bake manifest and entries are backed up first.
"""

from __future__ import annotations

import argparse
import copy
import os
import shutil
import sys
import time

import jmpped_common as jc
import jmpped_entries as je
import jmpped_workspace as jws


def prime(source: str, out_root: str) -> str | None:
    source = os.path.abspath(source)
    out_root = os.path.abspath(out_root)
    sm = je.load_manifest(source)
    bm = je.load_manifest(out_root)
    if not sm.get("entries") or not bm.get("entries"):
        raise jc.JMPPEDError("source and bake must each have an entries[] table")

    se = sm["entries"][0]
    source_sets = set(int(s) for s in se["page_sets"])
    for e in bm["entries"][1:]:
        overlap = source_sets & set(int(s) for s in e["page_sets"])
        if overlap:
            raise jc.JMPPEDError(
                "prime would collide: entry %r owns page sets %s"
                % (e.get("name"), sorted(overlap)))

    backup = jws.backup_workspace(out_root, "prime")
    for name in ("jericho", "tanner", "skeleton", "jmpped"):
        src = os.path.join(source, name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(out_root, name), dirs_exist_ok=True)
    for key in ("base_archive", "base_v5_archive"):
        rel = sm.get(key)
        if not rel:
            continue
        src = os.path.join(source, rel.replace("/", os.sep))
        dst = os.path.join(out_root, rel.replace("/", os.sep))
        if os.path.isfile(src):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)

    for key in ("peds", "pages", "skeleton", "bake", "base_archive",
                "base_archive_sha256", "base_v5_archive", "base_v5_archive_sha256",
                "source_level", "source_sha256"):
        if key in sm:
            bm[key] = copy.deepcopy(sm[key])
    entry = copy.deepcopy(se)
    entry["index"] = 0
    bm["entries"][0] = entry
    bm["source_workspace"] = source
    bm["prime_time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    jc.save_json(os.path.join(out_root, "jmpped.json"), bm)
    return backup


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Prime a bake workspace from a source export")
    ap.add_argument("--source", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    try:
        backup = prime(args.source, args.out)
    except jc.JMPPEDError as e:
        print("jmpped_prime: %s" % e, file=sys.stderr)
        return 1
    print("primed %s from %s" % (args.out, args.source))
    if backup:
        print("backup -> %s" % backup)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
