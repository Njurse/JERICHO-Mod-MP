#!/usr/bin/env python3
"""jmpped_register.py - register a completed entry folder and bake it.

Drag a character folder onto REGISTER_JMPPED.bat (or pass it here). The folder
must be a v5 entry directory: `ped.json`, `obj/raw|edit` for all 14 slots, the
three pages/palette strips and the bindpose. The script adds it to
`jmpped.json`'s `entries[]`, runs the builder, and copies the new bake to the
runtime locations.

Usage:
    python jmpped_register.py --folder out/jmppeds/v3/entries/jerichod3
    python jmpped_register.py --folder ... --no-build
    python jmpped_register.py --folder ... --no-copy
"""

from __future__ import annotations

import argparse
import copy
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import jmpped_common as jc
import jmpped_entries as je
import jmpped_import as ji
import jmpped_workspace as jws


HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
DEFAULT_OUT = os.path.join(HERE, "out", "jmppeds", "v3")
BUILD = os.path.join(HERE, "jmpped_build.py")


def _need(path: str, what: str) -> None:
    if not os.path.isfile(path):
        raise jc.JMPPEDError("%s is missing: %s" % (what, path))


def _resolve_folder(folder: str, out_root: str, manifest: dict) -> tuple[str, dict]:
    folder = os.path.abspath(folder)
    if not os.path.isdir(folder):
        raise jc.JMPPEDError("not a folder: %s" % folder)

    ped_path = os.path.join(folder, "ped.json")
    _need(ped_path, "ped.json")
    ped = jc.load_json(ped_path)

    # A legacy ped export has parts[] but no slots[]. If the manifest already
    # describes this directory, repair the local ped.json from that entry
    # instead of rejecting a working entry.
    if "slots" not in ped:
        rel_dir = je.rel(folder, out_root)
        existing = next((e for e in manifest["entries"]
                         if e.get("directory") == rel_dir), None)
        if existing is not None:
            repaired = copy.deepcopy(existing)
            repaired["ped"] = os.path.basename(folder.rstrip("\\/"))
            repaired["label"] = existing.get("name", repaired["ped"])
            repaired["entry_index"] = existing.get("index", 0)
            for page in repaired.get("pages", []):
                page.pop("png", None)
                page.pop("png_sha256", None)
                page.pop("texture_pages", None)
            for slot in repaired.get("slots", []):
                for crop in slot.get("crops", []):
                    crop.pop("indexed", None)
                    crop.pop("indexed_sha256", None)
            jc.save_json(ped_path, repaired)
            return folder, repaired

        alt = os.path.join(out_root, "entries", os.path.basename(folder.rstrip("\\/")))
        alt_ped = os.path.join(alt, "ped.json")
        if os.path.isfile(alt_ped):
            alt_data = jc.load_json(alt_ped)
            if "slots" in alt_data:
                return _resolve_folder(alt, out_root, manifest)
        raise jc.JMPPEDError(
            "%s/ped.json has no slots[] and the manifest has no entry for %s. "
            "Use entries/<slug> (or add-entry to clone one) instead."
            % (folder, rel_dir))

    rel_dir = os.path.relpath(folder, out_root)
    if rel_dir == ".." or rel_dir.startswith(".." + os.sep):
        raise jc.JMPPEDError(
            "%s is outside the workspace. Put the folder under %s/entries/ "
            "(or clone one with add-entry) and try again."
            % (folder, out_root))
    return folder, ped


def _entry_from_ped(out_root: str, folder: str, ped: dict, index: int,
                    name: str | None) -> dict:
    rel_dir = je.rel(folder, out_root)
    page_sets = [int(s) for s in ped.get("page_sets", [])]
    if len(page_sets) != 3:
        raise jc.JMPPEDError("%s/ped.json has %d page sets; expected 3"
                             % (folder, len(page_sets)))
    want = list(jc.entry_page_sets(index))
    if page_sets != want:
        raise jc.JMPPEDError(
            "entry %d owns page sets %s, but %s/ped.json uses %s. "
            "Use add-entry to allocate a new entry and remap it."
            % (index, want, folder, page_sets))

    e = {
        "index": index,
        "name": name or ped.get("label") or ped.get("name") or
                os.path.basename(folder.rstrip("\\/")),
        "directory": rel_dir,
        "source_ped": ped.get("source_ped", "jericho"),
        "variant": ped.get("variant", "jericho"),
        "page_sets": page_sets,
        "pages": ped.get("pages", []),
        "slots": ped.get("slots", []),
        "bindpose": ped.get("bindpose", {}),
        "palette_crops": ped.get("palette_crops", []),
    }
    je.validate_entry({"entries": [e]}, e)

    for s in e["slots"]:
        for key in ("obj_raw", "obj_edit"):
            _need(os.path.join(out_root, s[key]), "%s/%s" % (e["name"], key))
        for crop in s.get("crops", []):
            _need(os.path.join(out_root, crop["rgb"]),
                  "%s/crop %s" % (e["name"], crop["rgb"]))
            if crop.get("indexed"):
                _need(os.path.join(out_root, crop["indexed"]),
                      "%s/crop %s" % (e["name"], crop["indexed"]))
    for p in e["pages"]:
        if p.get("png"):
            _need(os.path.join(out_root, p["png"]),
                  "%s/page %s/png" % (e["name"], p.get("set")))
        for key in ("indexed_png", "palette_png"):
            _need(os.path.join(out_root, p[key]),
                  "%s/page %s/%s" % (e["name"], p.get("set"), key))
    for key in ("raw", "edit"):
        _need(os.path.join(out_root, e["bindpose"][key]),
              "%s/bindpose %s" % (e["name"], key))
    return e


def _normalize_obj_sets(out_root: str, manifest: dict, entry: dict) -> int:
    """Remap both OBJ copies from the source entry's page sets to this entry's.

    A hand-copied entry can have `usemtl s23_...` in both the edit and raw OBJs
    while its page records are 26/27/3. Leave target IDs alone and rewrite only
    the old source IDs, then refresh the hashes so the builder sees a consistent
    pair.
    """
    if not manifest.get("entries"):
        return 0
    source = manifest["entries"][0]
    target = tuple(int(s) for s in entry.get("page_sets", []))
    if len(target) != 3:
        return 0
    if [int(s) for s in source.get("page_sets", [])] == list(target):
        return 0

    mapping = je.material_map(source, target)
    mapping.update({s: s for s in target})
    changed = 0
    for slot in entry.get("slots", []):
        for key in ("obj_edit", "obj_raw"):
            rel = slot.get(key)
            if not rel:
                continue
            path = os.path.join(out_root, rel.replace("/", os.sep))
            with open(path, encoding="utf-8") as f:
                text = f.read()
            try:
                new = je.remap_materials(text, mapping)
            except jc.JMPPEDError as e:
                raise jc.JMPPEDError("%s/%s: %s" % (entry["name"], slot["part"], e))
            if new != text:
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(new)
                slot[key + "_sha256"] = jc.sha256_file(path)
                changed += 1
    return changed


def _register(out_root: str, folder: str, name: str | None) -> tuple[dict, int]:
    manifest = je.load_manifest(out_root)
    folder, ped = _resolve_folder(folder, out_root, manifest)
    rel_dir = je.rel(folder, out_root)

    existing = next((i for i, e in enumerate(manifest["entries"])
                     if e.get("directory") == rel_dir),
                    None)
    if existing is not None:
        index = existing
    else:
        index = int(ped.get("entry_index", len(manifest["entries"])))
        if index > len(manifest["entries"]):
            index = len(manifest["entries"])
        if index < len(manifest["entries"]):
            raise jc.JMPPEDError(
                "entry index %d is already occupied by %r; move the folder to "
                "a free entry index or use add-entry"
                % (index, manifest["entries"][index].get("name")))

    entry = _entry_from_ped(out_root, folder, ped, index, name)
    remapped = _normalize_obj_sets(out_root, manifest, entry)
    if remapped:
        print("remapped %d OBJ cop%s to page sets %s"
              % (remapped, "y" if remapped == 1 else "ies", entry["page_sets"]))
    if existing is None:
        manifest["entries"].append(entry)
    else:
        manifest["entries"][existing] = entry
    manifest["archive_version"] = jc.VERSION_V5
    jc.save_json(os.path.join(out_root, "jmpped.json"), manifest)

    repaired = copy.deepcopy(entry)
    repaired["ped"] = os.path.basename(folder.rstrip("\\/"))
    repaired["label"] = entry["name"]
    repaired["entry_index"] = index
    jc.save_json(os.path.join(folder, "ped.json"), repaired)
    return entry, index


def _runtime_targets() -> list[str]:
    targets = [os.path.join(HERE, "JMPPED.LEV")]
    for config in ("Release", "Release_dev"):
        levels = os.path.join(REPO, "src_rebuild", "bin", config,
                              "DRIVER2", "LEVELS")
        if os.path.isdir(os.path.dirname(os.path.dirname(levels))):
            targets.append(os.path.join(levels, "JMPPED.LEV"))
    return targets


def _should_import(folder: str, out_root: str, manifest: dict) -> bool:
    """Use the external-folder importer for outside folders / manifest bundles."""
    folder = os.path.abspath(folder)
    rel_dir = je.rel(folder, out_root)
    if rel_dir == ".." or rel_dir.startswith(".." + os.sep):
        return True
    try:
        _, data = ji._doc(folder)
    except jc.JMPPEDError:
        return False
    if isinstance(data.get("entries"), list):
        return True
    if data.get("parts") and not data.get("slots"):
        return True
    if data.get("slots"):
        existing = any(e.get("directory") == rel_dir
                       for e in manifest.get("entries", []))
        return not existing
    return False


def register(folder: str, out_root: str, name: str | None,
             do_build: bool, do_copy: bool, use: str = "edit") -> int:
    out_root = os.path.abspath(out_root)
    manifest = je.load_manifest(out_root)
    if _should_import(folder, out_root, manifest):
        imported = ji.import_folder(folder, out_root, name)
        for entry in imported:
            print('imported "%s" (mp character id %d)'
                  % (entry["name"], int(entry["index"]) + 1))
        if not do_build:
            print("--no-build: JSON updated, archive not rebuilt")
            return 0
        subprocess.run([sys.executable, BUILD, "--out", out_root, "--use", use],
                       check=True)
        bake = os.path.join(out_root, "JMPPED.LEV")
        _need(bake, "built JMPPED.LEV")
        if do_copy:
            for target in _runtime_targets():
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copy2(bake, target)
                print("copied -> %s" % target)
        else:
            print("--no-copy: bake left at %s" % bake)
        return 0

    entry, index = _register(out_root, folder, name)
    print('registered entry %d "%s" -> %s (mp character id %d)'
          % (index, entry["name"], entry["directory"], index + 1))

    if not do_build:
        print("--no-build: JSON updated, archive not rebuilt")
        return 0

    subprocess.run([sys.executable, BUILD, "--out", out_root, "--use", use],
                   check=True)
    bake = os.path.join(out_root, "JMPPED.LEV")
    _need(bake, "built JMPPED.LEV")

    if do_copy:
        for target in _runtime_targets():
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy2(bake, target)
            print("copied -> %s" % target)
    else:
        print("--no-copy: bake left at %s" % bake)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Register a completed JMPPED entry folder and bake it")
    ap.add_argument("--folder", required=True,
                    help="entry folder containing ped.json, obj/, png/")
    ap.add_argument("--out", default=None,
                    help="JMPPED workspace (default: last used under tools/out/jmppeds)")
    ap.add_argument("--name", default=None, help="override the entry name")
    ap.add_argument("--use", choices=("auto", "edit", "raw"), default="edit",
                    help="which OBJ copy the builder reads (default: edit, "
                         "because a repaired entry can have both copies changed)")
    ap.add_argument("--no-build", action="store_true",
                    help="register the entry but do not rebuild the archive")
    ap.add_argument("--no-copy", action="store_true",
                    help="do not copy the bake to tools/ and bin/")
    args = ap.parse_args(argv)
    out_root = jws.bake_root(args.out)
    jws.set_bake(out_root)
    try:
        return register(args.folder, out_root, args.name,
                        not args.no_build, not args.no_copy, args.use)
    except jc.JMPPEDError as e:
        print("jmpped_register: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
