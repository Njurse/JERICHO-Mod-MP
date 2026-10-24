#!/usr/bin/env python3
"""jmpped_add_entry.py - add a selectable character to a JMPPED workspace.

The new entry starts as a copy of an existing entry (Jericho by default): all 14
slot OBJs, its three pages and palette strips, and its bindpose. Everything is
moved into ``entries/<slug>/`` and its materials are remapped from the source
page sets to the entry's own disjoint sets. Replace the copied meshes/textures
with the new character, then run the normal builder.

Usage:
    python jmpped_add_entry.py --name "Jericho (Driv3r)"
    python jmpped_add_entry.py --name "My Ped" --from-entry Jericho --out DIR
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import jmpped_common as jc
import jmpped_entries as je


HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "out", "jmppeds", "v3")


def _hash(path: str) -> str:
    return jc.sha256_file(path)


def _copy_page(p: dict, out_root: str, entry_dir: str, slug: str,
               mapping: dict[int, int]) -> dict:
    old = int(p["set"])
    new = mapping[old]
    src_png = je.abs_path(out_root, p["png"])
    src_idx = je.abs_path(out_root, p["indexed_png"])
    src_pal = je.abs_path(out_root, p["palette_png"])
    dst_png = os.path.join(entry_dir, "png", "pages", "page_%d.png" % new)
    dst_idx = os.path.join(entry_dir, "png", "pages", "page_%d_indexed.png" % new)
    dst_pal = os.path.join(entry_dir, "png", "palettes", "page_%d.png" % new)
    je.copy_file(src_png, dst_png)
    je.copy_file(src_idx, dst_idx)
    je.copy_file(src_pal, dst_pal)

    q = dict(p)
    q["set"] = new
    q["ped"] = slug
    q["png"] = je.rel(dst_png, out_root)
    q["png_sha256"] = _hash(dst_png)
    q["indexed_png"] = je.rel(dst_idx, out_root)
    q["indexed_png_sha256"] = _hash(dst_idx)
    q["palette_png"] = je.rel(dst_pal, out_root)
    q["palette_png_sha256"] = _hash(dst_pal)
    return q


def _copy_slot(slot: dict, out_root: str, entry_dir: str,
               mapping: dict[int, int]) -> dict:
    part = slot["part"]
    src_raw = je.abs_path(out_root, slot["obj_raw"])
    src_edit = je.abs_path(out_root, slot["obj_edit"])
    dst_raw = os.path.join(entry_dir, "obj", "raw", part + ".obj")
    dst_edit = os.path.join(entry_dir, "obj", "edit", part + ".obj")
    je.copy_remapped_text(src_raw, dst_raw, mapping)
    je.copy_remapped_text(src_edit, dst_edit, mapping)

    crops = []
    for crop in slot.get("crops", []):
        old = int(crop["page_set"])
        new = mapping[old]
        src_rgb = je.abs_path(out_root, crop["rgb"])
        src_idx = je.abs_path(out_root, crop["indexed"])
        dst_rgb = os.path.join(entry_dir, "png", "parts",
                               "%s_set%d.png" % (part, new))
        dst_idx = os.path.join(entry_dir, "png", "parts",
                               "%s_set%d_indexed.png" % (part, new))
        je.copy_file(src_rgb, dst_rgb)
        je.copy_file(src_idx, dst_idx)
        q = dict(crop)
        q["page_set"] = new
        q["rgb"] = je.rel(dst_rgb, out_root)
        q["rgb_sha256"] = _hash(dst_rgb)
        q["indexed"] = je.rel(dst_idx, out_root)
        q["indexed_sha256"] = _hash(dst_idx)
        crops.append(q)

    q = dict(slot)
    q["page_sets"] = [mapping[int(s)] for s in slot.get("page_sets", [])]
    q["obj_raw"] = je.rel(dst_raw, out_root)
    q["obj_raw_sha256"] = _hash(dst_raw)
    q["obj_edit"] = je.rel(dst_edit, out_root)
    q["obj_edit_sha256"] = _hash(dst_edit)
    q["materials"] = [dict(m, set=mapping.get(int(m["set"]), int(m["set"])))
                      for m in slot.get("materials", [])]
    q["crops"] = crops
    return q


def _copy_bindpose(source: dict, out_root: str, entry_dir: str,
                   mapping: dict[int, int]) -> dict:
    src_raw = je.abs_path(out_root, source["raw"])
    src_edit = je.abs_path(out_root, source["edit"])
    dst_raw = os.path.join(entry_dir, "obj", "bindpose_raw.obj")
    dst_edit = os.path.join(entry_dir, "obj", "bindpose_edit.obj")
    je.copy_remapped_text(src_raw, dst_raw, mapping)
    je.copy_remapped_text(src_edit, dst_edit, mapping)
    return {
        "raw": je.rel(dst_raw, out_root),
        "raw_sha256": _hash(dst_raw),
        "edit": je.rel(dst_edit, out_root),
        "edit_sha256": _hash(dst_edit),
    }


def _copy_palette_crops(source: dict, out_root: str, entry_dir: str,
                        mapping: dict[int, int]) -> list[dict]:
    out = []
    for crop in source.get("palette_crops", []):
        old = int(crop["set"])
        new = mapping[old]
        src = je.abs_path(out_root, crop["palette_png"])
        dst = os.path.join(entry_dir, "png", "palettes", "page_%d.png" % new)
        if not os.path.isfile(dst):
            je.copy_file(src, dst)
        q = dict(crop)
        q["set"] = new
        q["palette_png"] = je.rel(dst, out_root)
        q["palette_png_sha256"] = _hash(dst)
        out.append(q)
    return out


def add_entry(out_root: str, name: str, from_entry: str, slug_arg: str | None) -> dict:
    out_root = os.path.abspath(out_root)
    manifest = je.load_manifest(out_root)
    source = je.entry(manifest, from_entry)
    je.validate_entry(manifest, source)

    if any(e["name"] == name for e in manifest["entries"]):
        raise jc.JMPPEDError("entry %r already exists" % name)
    index = len(manifest["entries"])
    if index >= jc.JMPPED_MAX_ENTRIES:
        raise jc.JMPPEDError("the archive supports %d entries; cannot add %r"
                             % (jc.JMPPED_MAX_ENTRIES, name))
    slug = slug_arg or je.slugify(name)
    directory = "entries/" + slug
    entry_dir = os.path.join(out_root, "entries", slug)
    if os.path.exists(entry_dir):
        raise jc.JMPPEDError("entry directory %s already exists" % entry_dir)

    target_sets = jc.entry_page_sets(index)
    mapping = je.material_map(source, target_sets)
    if len(set(mapping.values())) != 3:
        raise jc.JMPPEDError("entry %r needs three distinct target page sets"
                             % name)

    pages = [_copy_page(p, out_root, entry_dir, slug, mapping)
             for p in source["pages"]]
    slots = [_copy_slot(s, out_root, entry_dir, mapping)
             for s in source["slots"]]
    bindpose = _copy_bindpose(source["bindpose"], out_root, entry_dir, mapping)
    palette_crops = _copy_palette_crops(source, out_root, entry_dir, mapping)

    new_entry = {
        "index": index,
        "name": name,
        "directory": directory,
        "source_ped": source.get("source_ped", "jericho"),
        "variant": source.get("variant", "jericho"),
        "page_sets": list(target_sets),
        "pages": pages,
        "slots": slots,
        "bindpose": bindpose,
        "palette_crops": palette_crops,
    }
    je.validate_entry(manifest, new_entry)
    manifest["entries"].append(new_entry)
    manifest["archive_version"] = jc.VERSION_V5
    jc.save_json(os.path.join(out_root, "jmpped.json"), manifest)

    ped_json = {
        "ped": slug,
        "label": name,
        "entry_index": index,
        "source_entry": source["name"],
        "directory": directory,
        "page_sets": list(target_sets),
        "pages": pages,
        "slots": slots,
        "bindpose": bindpose,
        "palette_crops": palette_crops,
        "skeleton": manifest["skeleton"]["json"],
        "note": "edit this entry's 14 obj/edit parts, its pages/palette strips, "
                "or its bindpose_edit.obj; the builder writes every entry into "
                "the v5 JMPPED.LEV",
    }
    jc.save_json(os.path.join(entry_dir, "ped.json"), ped_json)
    return new_entry


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Add a selectable ped entry to a JMPPED workspace")
    ap.add_argument("--name", required=True, help="display name, e.g. 'Jericho (Driv3r)'")
    ap.add_argument("--from-entry", default="Jericho", help="source entry (default Jericho)")
    ap.add_argument("--slug", default=None, help="directory slug (default from name)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="workspace (default tools/out/jmppeds/v3)")
    args = ap.parse_args(argv)
    try:
        e = add_entry(args.out, args.name, args.from_entry, args.slug)
    except jc.JMPPEDError as e:
        print("jmpped_add_entry: %s" % e, file=sys.stderr)
        return 1
    print("added entry %d: %s -> %s (pages %s)"
          % (e["index"], e["name"], e["directory"], e["page_sets"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
