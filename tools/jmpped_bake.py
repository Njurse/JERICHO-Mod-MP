#!/usr/bin/env python3
"""jmpped_bake.py - one driver for exporting, rebuilding and previewing JMPPED.

The toolkit is v3-only. It extracts Tanner and Jericho from a Driver 2 level,
writes a per-ped OBJ/PNG workspace, rebuilds the 14-slot JMPPED v3 archive, and
renders a preview.

    export   Driver 2 level -> Tanner/Jericho subexports + skeleton + jmpped.json
    split    edited bindpose_edit.obj -> local per-part OBJs (+ optional build)
    build    edited OBJ/PNG -> JMPPED.LEV
    preview  JMPPED.LEV -> 360-degree GIF
    all      export, build, preview

A v3 archive is copied to DRIVER2/LEVELS only when --copy is passed.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
OUT_ROOT = os.path.join(HERE, "out", "jmppeds", "v3")
DEFAULT_COPY = os.path.join(REPO, "src_rebuild", "bin", "Release", "DRIVER2",
                            "LEVELS", "JMPPED.LEV")
EXPORT = os.path.join(HERE, "jmpped_export.py")
BUILD = os.path.join(HERE, "jmpped_build.py")
SPLIT = os.path.join(HERE, "jmpped_split.py")
ADD_ENTRY = os.path.join(HERE, "jmpped_add_entry.py")
PREVIEW = os.path.join(HERE, "preview_jmpped.py")


def _run(cmd: list[str]) -> int:
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=REPO).returncode


def _export(args, out: str) -> int:
    cmd = [sys.executable, EXPORT, "--out", out]
    if args.source:
        cmd += ["--source", args.source]
    return _run(cmd)


def _build(args, out: str) -> int:
    cmd = [sys.executable, BUILD, "--out", out, "--use", args.use,
           "--entry", args.entry]
    if args.copy and not args.no_copy:
        cmd += ["--copy-to", args.copy_to or DEFAULT_COPY]
    return _run(cmd)


def _add_entry(args, out: str) -> int:
    cmd = [sys.executable, ADD_ENTRY, "--out", out, "--name", args.name,
           "--from-entry", args.from_entry]
    if args.slug:
        cmd += ["--slug", args.slug]
    return _run(cmd)


def _split(args, out: str) -> int:
    cmd = [sys.executable, SPLIT, "--out", out, "--entry", args.entry]
    if args.scene:
        cmd += ["--scene", args.scene]
    if args.raw_parts:
        cmd.append("--raw")
    if args.and_build:
        cmd.append("--and-build")
    return _run(cmd)


def _preview(args, out: str) -> int:
    lev = os.path.join(out, "JMPPED.LEV")
    if not os.path.isfile(lev):
        raise SystemExit("jmpped_bake: %s is missing; run the build step first" % lev)
    preview_out = args.preview_out or os.path.join(out, "preview_360.gif")
    return _run([sys.executable, PREVIEW, "--lev", lev, "--entry", args.entry,
                 "--out", preview_out,
                 "--frames", str(args.frames), "--size", str(args.size)])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="BAKE the Driver 2 Tanner/Jericho "
                                             "peds into JMPPED v3")
    ap.add_argument("step", nargs="?", default="all",
                    choices=("export", "add-entry", "split", "build", "preview", "all"))
    ap.add_argument("--out", default=None,
                    help="workspace (default: tools/out/jmppeds/v3)")
    ap.add_argument("--source", default=None,
                    help="Driver 2 level (default: exporter's Havana path)")
    ap.add_argument("--name", default=None,
                    help="add-entry display name (e.g. 'Jericho (Driv3r)')")
    ap.add_argument("--from-entry", default="Jericho",
                    help="add-entry source entry (default Jericho)")
    ap.add_argument("--slug", default=None,
                    help="add-entry directory slug (default from name)")
    ap.add_argument("--entry", default="Jericho",
                    help="entry name from jmpped.json (default Jericho)")
    ap.add_argument("--scene", default=None,
                    help="combined bindpose OBJ for split (default: the variant's)")
    ap.add_argument("--raw-parts", action="store_true",
                    help="split writes engine-space obj/raw parts instead of obj/edit")
    ap.add_argument("--and-build", action="store_true",
                    help="split then run the builder")
    ap.add_argument("--use", choices=("auto", "edit", "raw"), default="auto",
                    help="which OBJ copy the builder reads")
    ap.add_argument("--copy", action="store_true",
                    help="copy the v3 rebuild to DRIVER2/LEVELS")
    ap.add_argument("--no-copy", action="store_true",
                    help="never copy, even if a caller passes --copy")
    ap.add_argument("--copy-to", default=None,
                    help="copy target (default: src_rebuild/bin/Release/DRIVER2/LEVELS/JMPPED.LEV)")
    ap.add_argument("--preview-out", default=None,
                    help="GIF path (default: <out>/preview_360.gif)")
    ap.add_argument("--frames", type=int, default=36)
    ap.add_argument("--size", type=int, default=512)
    args = ap.parse_args(argv)

    if args.copy and args.no_copy:
        raise SystemExit("jmpped_bake: --copy and --no-copy are mutually exclusive")

    out = args.out or OUT_ROOT
    os.makedirs(out, exist_ok=True)

    if args.step in ("export", "all"):
        rc = _export(args, out)
        if rc:
            return rc
    if args.step == "add-entry":
        if not args.name:
            raise SystemExit("jmpped_bake: add-entry requires --name")
        rc = _add_entry(args, out)
        if rc:
            return rc
    if args.step == "split":
        rc = _split(args, out)
        if rc:
            return rc
    if args.step in ("build", "all"):
        rc = _build(args, out)
        if rc:
            return rc
    if args.step in ("preview", "all"):
        rc = _preview(args, out)
        if rc:
            return rc
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
