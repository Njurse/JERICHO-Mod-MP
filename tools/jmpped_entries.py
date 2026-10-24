#!/usr/bin/env python3
"""Helpers shared by the multi-entry authoring tools.

The manifest's `entries[]` table is the single description of a selectable
character: its name, directory, 14 slot descriptors, page sets/files, bindpose
and palette crops. These helpers keep the add-entry/split/build/preview tools
reading the same table.
"""

from __future__ import annotations

import os
import re
import shutil

import jmpped_common as jc


def rel(path: str, root: str) -> str:
    return os.path.relpath(path, root).replace("\\", "/")


def load_manifest(out_root: str) -> dict:
    path = os.path.join(out_root, "jmpped.json")
    if not os.path.isfile(path):
        raise jc.JMPPEDError("%s not found; run jmpped_export.py first" % path)
    manifest = jc.load_json(path)
    if manifest.get("manifest_version") != 2:
        raise jc.JMPPEDError("unsupported manifest version %r; re-export the "
                             "Tanner/Jericho workspace"
                             % manifest.get("manifest_version"))
    if not manifest.get("entries"):
        raise jc.JMPPEDError("manifest has no entries[] table; re-export")
    return manifest


def entries(manifest: dict) -> list[dict]:
    return manifest["entries"]


def entry_index(manifest: dict, name: str | int) -> int:
    if isinstance(name, int):
        if name < 0 or name >= len(manifest["entries"]):
            raise jc.JMPPEDError("entry index %d is out of range" % name)
        return name
    for i, e in enumerate(manifest["entries"]):
        if e["name"] == name:
            return i
    raise jc.JMPPEDError("no entry named %r (entries: %s)"
                         % (name, ", ".join(e["name"] for e in manifest["entries"])))


def entry(manifest: dict, name: str | int = "Jericho") -> dict:
    return manifest["entries"][entry_index(manifest, name)]


def slugify(name: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9]+", "-", name.strip()).strip("-").lower()
    if not slug:
        raise jc.JMPPEDError("entry name %r has no usable directory name" % name)
    return slug


def validate_entry(manifest: dict, e: dict) -> None:
    if not e.get("name") or not e.get("directory"):
        raise jc.JMPPEDError("entry is missing name/directory")
    if len(e.get("slots", [])) != len(jc.MODEL_NAMES):
        raise jc.JMPPEDError("entry %r has %d slots; expected %d"
                             % (e["name"], len(e.get("slots", [])), len(jc.MODEL_NAMES)))
    slots = [int(s["slot"]) for s in e["slots"]]
    if sorted(slots) != list(range(len(jc.MODEL_NAMES))):
        raise jc.JMPPEDError("entry %r slots are not 0..%d"
                             % (e["name"], len(jc.MODEL_NAMES) - 1))
    sets = [int(s) for s in e.get("page_sets", [])]
    if len(sets) != 3:
        raise jc.JMPPEDError("entry %r has %d page sets; expected 3"
                             % (e["name"], len(sets)))
    page_sets = {int(p["set"]) for p in e.get("pages", [])}
    if page_sets != set(sets):
        raise jc.JMPPEDError("entry %r page records %s do not match page_sets %s"
                             % (e["name"], sorted(page_sets), sorted(sets)))
    if not e.get("bindpose", {}).get("edit"):
        raise jc.JMPPEDError("entry %r has no bindpose edit OBJ" % e["name"])


def material_map(source_entry: dict, target_sets: tuple[int, int, int]) -> dict[int, int]:
    old = [int(s) for s in source_entry["page_sets"]]
    if len(old) != 3:
        raise jc.JMPPEDError("source entry %r has %d page sets"
                             % (source_entry["name"], len(old)))
    return {old[i]: int(target_sets[i]) for i in range(3)}


_MATERIAL = re.compile(r"usemtl s(\d+)_t(\d+)")


def remap_materials(text: str, mapping: dict[int, int]) -> str:
    def sub(m):
        setid = int(m.group(1))
        new = mapping.get(setid)
        if new is None:
            raise jc.JMPPEDError("OBJ references page set %d, which is not in the "
                                 "source entry" % setid)
        return "usemtl s%d_t%s" % (new, m.group(2))

    return _MATERIAL.sub(sub, text)


def copy_remapped_text(src: str, dst: str, mapping: dict[int, int]) -> None:
    with open(src, encoding="utf-8") as f:
        text = f.read()
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(remap_materials(text, mapping))


def copy_file(src: str, dst: str) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    shutil.copy2(src, dst)


def abs_path(root: str, rel_path: str) -> str:
    return os.path.join(root, rel_path.replace("/", os.sep))
