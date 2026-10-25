#!/usr/bin/env python3
"""jmpped_crops.py - split a combined part crop into one editable crop per material.

The old one-image format gave a whole part one 16-colour palette, even when the
part used several texture ids (e.g. Jericho's head uses rows 24 and 25). The
builder supports per-material crops with `texture`/`display_clut`; this tool
regenerates them from the entry page's indexed pixels and each material's own
CLUT row, so editing a part texture only changes that material's palette.
"""

from __future__ import annotations

import argparse
import os
import sys

import jmpped_common as jc
import jmpped_entries as je
import jmpped_v5 as jv


def _entry_folder(out_root: str, entry: dict) -> str:
    return os.path.join(out_root, entry["directory"].replace("/", os.sep))


def _split_entry(out_root: str, arch: jc.Archive, entry: dict) -> int:
    root = _entry_folder(out_root, entry)
    parts_dir = os.path.join(root, "png", "parts")
    os.makedirs(parts_dir, exist_ok=True)
    pages = {int(p["set"]): p for p in entry.get("pages", [])}
    states = {}
    changed = 0
    for slot in entry.get("slots", []):
        materials = slot.get("materials") or []
        if not materials:
            continue
        crops = slot.get("crops") or []
        if crops and all(c.get("texture") is not None and
                         c.get("display_clut") is not None for c in crops):
            continue
        new_crops = []
        for m in materials:
            page_set = int(m["set"])
            texture = int(m["id"])
            page = pages.get(page_set)
            if page is None:
                raise jc.JMPPEDError("%s: no page record for set %d"
                                     % (entry["name"], page_set))
            if page_set not in states:
                states[page_set] = jv._entry_page_state(page, arch, out_root)
            state = states[page_set]
            if texture >= len(state["cluts"]):
                raise jc.JMPPEDError(
                    "%s/%s: texture %d is outside page %d's %d CLUT rows"
                    % (entry["name"], slot["part"], texture, page_set,
                       len(state["cluts"])))
            clut = jc.clut_rgb(state["cluts"][texture])
            x0, x1 = (int(v) for v in m["u"])
            y0, y1 = (int(v) for v in m["v"])
            x0, y0 = max(0, x0), max(0, y0)
            x1, y1 = min(jc.PAGE_W - 1, x1), min(jc.PAGE_H - 1, y1)
            w, h = x1 - x0 + 1, y1 - y0 + 1
            if w <= 0 or h <= 0:
                continue
            indices = state["indices"]
            pixels = [clut[indices[y * jc.PAGE_W + x]]
                      for y in range(y0, y1 + 1)
                      for x in range(x0, x1 + 1)]
            path = os.path.join(parts_dir, "%s_set%d_t%d.png"
                                % (slot["part"], page_set, texture))
            jc.save_rgb_png(path, w, h, pixels)
            new_crops.append({
                "page_set": page_set,
                "texture": texture,
                "display_clut": texture,
                "rect": [x0, y0, w, h],
                "rgb": je.rel(path, out_root),
                "rgb_sha256": jc.sha256_file(path),
                "polys": int(m.get("polys", 0)),
            })
        if new_crops:
            slot["crops"] = new_crops
            changed += len(new_crops)
    return changed


def split_crops(out_root: str, entry_name: str | None) -> int:
    out_root = os.path.abspath(out_root)
    manifest = je.load_manifest(out_root)
    base = os.path.join(out_root, manifest["base_archive"].replace("/", os.sep))
    arch = jc.read_archive(base)
    entries = ([je.entry(manifest, entry_name)] if entry_name
               else manifest["entries"])
    changed = 0
    for entry in entries:
        n = _split_entry(out_root, arch, entry)
        print("%s: %d per-material crop(s)" % (entry["name"], n))
        changed += n
    if changed:
        jc.save_json(os.path.join(out_root, "jmpped.json"), manifest)
    return changed


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Split combined JMPPED part crops into per-material crops")
    ap.add_argument("--out", default=None)
    ap.add_argument("--entry", default=None,
                    help="entry name (default: every entry)")
    args = ap.parse_args(argv)
    out = args.out
    if not out:
        import jmpped_workspace as jws
        out = jws.bake_root()
    try:
        split_crops(out, args.entry)
    except jc.JMPPEDError as e:
        print("jmpped_crops: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
