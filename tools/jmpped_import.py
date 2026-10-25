#!/usr/bin/env python3
"""jmpped_import.py - import one or many entries from a character folder's JSON.

The folder is referenced in place; nothing in it is rewritten or copied. Entry
page sets are remapped to the bake workspace's allocation through entry-level
`page_set_map`, which the builder applies while reading the referenced OBJs.

Supported JSON shapes:
  * `entries[]` from a jmpped.json
  * an entry-style ped.json with slots/pages
  * a legacy ped.json with parts/pages
"""

from __future__ import annotations

import copy
import json
import os
import re

import jmpped_common as jc
import jmpped_entries as je


def _doc(folder: str) -> tuple[str, dict]:
    folder = os.path.abspath(folder)
    if os.path.isfile(folder) and folder.lower().endswith(".json"):
        return os.path.dirname(folder), jc.load_json(folder)
    for name in ("ped.json", "jmpped.json"):
        path = os.path.join(folder, name)
        if os.path.isfile(path):
            return folder, jc.load_json(path)
    for name in sorted(os.listdir(folder)):
        if name.lower().endswith(".json"):
            return folder, jc.load_json(os.path.join(folder, name))
    raise jc.JMPPEDError("%s has no ped.json or jmpped.json" % folder)


def _resolve(value: str, folder: str, out_root: str) -> str:
    value = value.replace("/", os.sep)
    if os.path.isabs(value):
        return value
    for base in (folder, out_root):
        candidate = os.path.join(base, value)
        if os.path.exists(candidate):
            return candidate
    return os.path.join(folder, value)


def _rel(value: str, folder: str, out_root: str) -> str:
    return je.rel(_resolve(value, folder, out_root), out_root)


def _sha(value: str, folder: str, out_root: str) -> str:
    return jc.sha256_file(_resolve(value, folder, out_root))


def _sets(data: dict) -> list[int]:
    """Source (OBJ/material) page sets.

    An already-imported bundle carries `page_set_map` from its original source
    sets to its current display sets; the keys are the sets the referenced OBJs
    actually use.
    """
    psm = data.get("page_set_map") or {}
    if isinstance(psm, dict) and len(psm) == 3:
        return sorted(int(k) for k in psm)
    if isinstance(data.get("page_sets"), list) and len(data["page_sets"]) == 3:
        return [int(s) for s in data["page_sets"]]
    found = []
    for p in data.get("pages", []):
        if isinstance(p, dict) and "set" in p:
            found.append(int(p["set"]))
    for s in data.get("slots", []):
        for value in s.get("page_sets", []):
            found.append(int(value))
        for m in s.get("materials", []):
            found.append(int(m["set"]))
        for c in s.get("crops", []):
            found.append(int(c["page_set"]))
    for p in data.get("parts", []):
        for value in p.get("page_sets", []):
            found.append(int(value))
        for m in p.get("materials", []):
            found.append(int(m["set"]))
        for c in p.get("crops", []):
            found.append(int(c["page_set"]))
    unique = sorted(set(found))
    if len(unique) == 3:
        return unique
    raise jc.JMPPEDError("cannot determine exactly three source page sets (found %s)"
                         % unique)


def _current_sets(data: dict) -> list[int]:
    """Page sets the bundle's page/slot records currently name."""
    if isinstance(data.get("page_sets"), list) and len(data["page_sets"]) == 3:
        return [int(s) for s in data["page_sets"]]
    psm = data.get("page_set_map") or {}
    if isinstance(psm, dict) and len(psm) == 3:
        return [int(psm[k]) for k in sorted(psm, key=lambda x: int(x))]
    return _sets(data)


def _cluts(palette: str, folder: str, out_root: str) -> list[str]:
    path = _resolve(palette, folder, out_root)
    w, h, pixels = jc.load_png_rgb(path)
    if w != jc.CLUT_ROW_ENTRIES:
        raise jc.JMPPEDError("%s is %dx%d, expected 16xN" % (path, w, h))
    return [jc.rgb_to_clut(pixels[y * w:(y + 1) * w]).hex() for y in range(h)]


def _pages(data: dict, folder: str, out_root: str, mapping: dict[int, int],
           target_sets: list[int]) -> list[dict]:
    pages = data.get("pages", [])
    out = []
    if pages and isinstance(pages[0], dict):
        for p in pages:
            old = int(p["set"])
            if old not in mapping:
                raise jc.JMPPEDError("page set %d has no target allocation" % old)
            q = copy.deepcopy(p)
            q["source_set"] = p.get("source_set", old)
            q["set"] = mapping[old]
            for key in ("png", "indexed_png", "palette_png"):
                if q.get(key):
                    q[key] = _rel(q[key], folder, out_root)
                    hk = key + "_sha256"
                    if q.get(hk):
                        q[hk] = _sha(q[key], folder, out_root)
            if q.get("palette_png") and not q.get("cluts_hex"):
                q["cluts_hex"] = _cluts(q["palette_png"], folder, out_root)
            if q.get("cluts_hex"):
                q["npalettes"] = len(q["cluts_hex"])
            out.append(q)
        return out

    source_sets = [int(s) for s in pages] if pages else _sets(data)
    for old in source_sets:
        if old not in mapping:
            raise jc.JMPPEDError("page set %d has no target allocation" % old)
        new = mapping[old]
        idx = _rel("png/pages/page_%d_indexed.png" % old, folder, out_root)
        pal = _rel("png/palettes/page_%d.png" % old, folder, out_root)
        if not os.path.isfile(_resolve(idx, folder, out_root)):
            raise jc.JMPPEDError("page %d has no indexed png (%s)" % (old, idx))
        if not os.path.isfile(_resolve(pal, folder, out_root)):
            raise jc.JMPPEDError("page %d has no palette strip (%s)" % (old, pal))
        q = {
            "set": new,
            "source_set": old,
            "ped": data.get("ped", "imported"),
            "storage": "raw",
            "flags": 0,
            "npalettes": len(_cluts(pal, folder, out_root)),
            "image_bytes": 0,
            "display_clut": 0,
            "indexed_png": idx,
            "indexed_png_sha256": _sha(idx, folder, out_root),
            "palette_png": pal,
            "palette_png_sha256": _sha(pal, folder, out_root),
            "cluts_hex": _cluts(pal, folder, out_root),
        }
        out.append(q)
    return out


def _map_crops(crops, folder, out_root, mapping):
    out = []
    for crop in crops:
        old = int(crop["page_set"])
        if old not in mapping:
            raise jc.JMPPEDError("crop page set %d has no target allocation" % old)
        q = copy.deepcopy(crop)
        q["page_set"] = mapping[old]
        q["rgb"] = _rel(crop["rgb"], folder, out_root)
        q["rgb_sha256"] = _sha(q["rgb"], folder, out_root)
        if crop.get("indexed"):
            q["indexed"] = _rel(crop["indexed"], folder, out_root)
            q["indexed_sha256"] = _sha(q["indexed"], folder, out_root)
        out.append(q)
    return out


def _map_materials(materials, mapping):
    return [dict(m, set=mapping[int(m["set"])]) for m in materials]


def _slots(data: dict, folder: str, out_root: str, mapping: dict[int, int],
           target_sets: list[int]) -> list[dict]:
    out = []
    if data.get("slots"):
        for s in data["slots"]:
            q = copy.deepcopy(s)
            q["page_sets"] = [mapping[int(x)] for x in s.get("page_sets", [])]
            for key in ("obj_raw", "obj_edit"):
                q[key] = _rel(s[key], folder, out_root)
                q[key + "_sha256"] = _sha(q[key], folder, out_root)
            q["materials"] = _map_materials(s.get("materials", []), mapping)
            q["crops"] = _map_crops(s.get("crops", []), folder, out_root, mapping)
            q.setdefault("offset", [0, 0, 0])
            q.setdefault("scale", [1, 1, 1])
            out.append(q)
        return out

    for part in data.get("parts", []):
        if part.get("bake_slot") is None:
            continue
        q = {
            "slot": int(part["bake_slot"]),
            "ped": data.get("ped", "imported"),
            "part": part["name"],
            "page_sets": [mapping[int(x)] for x in part.get("page_sets", [])],
            "obj_raw": _rel(part["obj_raw"], folder, out_root),
            "obj_edit": _rel(part["obj_edit"], folder, out_root),
            "materials": _map_materials(part.get("materials", []), mapping),
            "crops": _map_crops(part.get("crops", []), folder, out_root, mapping),
            "offset": part.get("offset", [0, 0, 0]),
            "scale": part.get("scale", [1, 1, 1]),
        }
        q["obj_raw_sha256"] = _sha(q["obj_raw"], folder, out_root)
        q["obj_edit_sha256"] = _sha(q["obj_edit"], folder, out_root)
        out.append(q)
    if len(out) != len(jc.MODEL_NAMES):
        raise jc.JMPPEDError("legacy ped.json produced %d slots; expected %d"
                             % (len(out), len(jc.MODEL_NAMES)))
    return out


def _bindpose(data: dict, folder: str, out_root: str) -> dict:
    bp = data.get("bindpose") or {}
    raw = bp.get("raw") or "obj/bindpose_raw.obj"
    edit = bp.get("edit") or "obj/bindpose_edit.obj"
    q = {
        "raw": _rel(raw, folder, out_root),
        "edit": _rel(edit, folder, out_root),
    }
    q["raw_sha256"] = _sha(q["raw"], folder, out_root)
    q["edit_sha256"] = _sha(q["edit"], folder, out_root)
    return q


def _palette_crops(data, pages, folder, out_root, mapping):
    crops = data.get("palette_crops")
    if crops:
        out = []
        for c in crops:
            q = copy.deepcopy(c)
            old = int(c["set"])
            q["set"] = mapping[old]
            q["palette_png"] = _rel(c["palette_png"], folder, out_root)
            q["palette_png_sha256"] = _sha(q["palette_png"], folder, out_root)
            out.append(q)
        return out
    return [{
        "set": p["set"],
        "palette_png": p["palette_png"],
        "palette_png_sha256": p["palette_png_sha256"],
        "cluts_hex": p.get("cluts_hex", []),
        "display_clut": p.get("display_clut", 0),
    } for p in pages]


def _obj_sets(path: str) -> set[int]:
    sets = set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("usemtl s"):
                m = re.match(r"usemtl s(\d+)_t", line.strip())
                if m:
                    sets.add(int(m.group(1)))
    return sets


def _entry(data: dict, folder: str, out_root: str, index: int,
            target_sets: list[int], fallback_name: str,
            item_dir: str = ".") -> dict:
    source_sets = _sets(data)
    current_sets = _current_sets(data)
    source_map = {source_sets[i]: target_sets[i] for i in range(3)}
    current_map = {current_sets[i]: target_sets[i] for i in range(3)}
    pages = _pages(data, folder, out_root, current_map, target_sets)
    slots = _slots(data, folder, out_root, current_map, target_sets)
    bindpose = _bindpose(data, folder, out_root)
    name = fallback_name or data.get("name") or data.get("label") or data.get("ped")
    abs_dir = (folder if item_dir in ("", ".") else
               _resolve(item_dir, folder, out_root))
    e = {
        "index": index,
        "name": name,
        "directory": je.rel(abs_dir, out_root),
        "source_ped": data.get("source_ped", data.get("ped", "imported")),
        "variant": data.get("variant", "jericho"),
        "page_sets": list(target_sets),
        "pages": pages,
        "slots": slots,
        "bindpose": bindpose,
        "palette_crops": _palette_crops(data, pages, folder, out_root, current_map),
        "page_set_map": {str(k): v for k, v in source_map.items()},
    }
    je.validate_entry({"entries": [e]}, e)
    for s in slots:
        for key in ("obj_raw", "obj_edit"):
            if not os.path.isfile(_resolve(s[key], folder, out_root)):
                raise jc.JMPPEDError("%s/%s is missing" % (name, s[key]))
        for c in s.get("crops", []):
            if not os.path.isfile(_resolve(c["rgb"], folder, out_root)):
                raise jc.JMPPEDError("%s/crop %s is missing" % (name, c["rgb"]))
    for p in pages:
        for key in ("indexed_png", "palette_png"):
            if not os.path.isfile(_resolve(p[key], folder, out_root)):
                raise jc.JMPPEDError("%s/page %s/%s is missing"
                                     % (name, p.get("set"), key))
    for key in ("raw", "edit"):
        if not os.path.isfile(_resolve(bindpose[key], folder, out_root)):
            raise jc.JMPPEDError("%s/bindpose %s is missing" % (name, key))
    source_set = set(source_sets)
    target_set = set(target_sets)
    for s in slots:
        for key in ("obj_raw", "obj_edit"):
            path = _resolve(s[key], folder, out_root)
            bad = _obj_sets(path) - source_set
            if bad:
                raise jc.JMPPEDError("%s/%s uses unmapped page set(s) %s"
                                     % (name, s["part"], sorted(bad)))
            if {source_map[x] for x in _obj_sets(path)} - target_set:
                raise jc.JMPPEDError("%s/%s maps outside %s"
                                     % (name, s["part"], target_sets))
    return e


def import_folder(folder: str, out_root: str, name: str | None = None) -> list[dict]:
    folder, data = _doc(folder)
    out_root = os.path.abspath(out_root)
    manifest = je.load_manifest(out_root)
    entries = data["entries"] if isinstance(data.get("entries"), list) else [data]
    bundle = isinstance(data.get("entries"), list)
    imported = []
    for item in entries:
        label = name or item.get("name") or item.get("label") or item.get("ped") \
            or os.path.basename(folder.rstrip("\\/"))
        item_dir = (item.get("directory") or ".") if bundle else "."
        abs_dir = (folder if item_dir in ("", ".") else
                   _resolve(item_dir, folder, out_root))
        directory = je.rel(abs_dir, out_root)
        existing = next((i for i, e in enumerate(manifest["entries"])
                         if e.get("name") == label or e.get("directory") == directory),
                        None)
        if existing is not None:
            index = existing
            target_sets = [int(s) for s in manifest["entries"][index]["page_sets"]]
        else:
            index = len(manifest["entries"])
            target_sets = list(jc.entry_page_sets(index))
        for j, other in enumerate(manifest["entries"]):
            if j != existing and set(target_sets) & set(other["page_sets"]):
                raise jc.JMPPEDError("%s: page sets %s are already owned by %r"
                                     % (label, target_sets, other.get("name")))
        entry = _entry(item, folder, out_root, index, target_sets, label,
                       item_dir=item_dir)
        if existing is None:
            manifest["entries"].append(entry)
        else:
            manifest["entries"][existing] = entry
        imported.append(entry)
    manifest["archive_version"] = jc.VERSION_V5
    jc.save_json(os.path.join(out_root, "jmpped.json"), manifest)
    return imported
