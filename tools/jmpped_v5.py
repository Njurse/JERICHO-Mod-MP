#!/usr/bin/env python3
"""jmpped_v5.py - build a multi-entry v5 JMPPED archive from entries[].

Entry 0 can reuse the v3 base blobs when untouched. A later entry is always
rebuilt from its own OBJ/PNG files because its materials point at its own page
sets. Pages are built once per entry; entries own disjoint page sets.
"""

from __future__ import annotations

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import jmpped_common as jc
import jmpped_entries as je
import jmpped_build as jb


def _entry_page_state(entry: dict, arch: jc.Archive, root: str) -> dict:
    setid = int(entry["set"])
    base_index = next((i for i, p in enumerate(arch.pages) if p.set == setid), None)
    base_page = arch.pages[base_index] if base_index is not None else None
    base_raw = arch.raw_pages[base_index] if base_index is not None else None

    if entry.get("cluts_hex"):
        cluts = [bytes.fromhex(h) for h in entry["cluts_hex"]]
    elif base_page is not None:
        cluts = jc.page_cluts(base_page)
    else:
        cluts = []

    indices = jc.page_indices(base_page) if base_page is not None else None
    display = int(entry.get("display_clut", 0))
    if display < 0 or display >= len(cluts):
        display = 0

    png = jb._abs(root, entry["png"]) if entry.get("png") else None
    idx = jb._abs(root, entry["indexed_png"])
    pal = jb._abs(root, entry["palette_png"])
    page_changed = jb._changed(png, entry.get("png_sha256", "")) if png else False
    idx_changed = jb._changed(idx, entry["indexed_png_sha256"])
    pal_changed = jb._changed(pal, entry["palette_png_sha256"])
    if page_changed and idx_changed:
        raise jc.JMPPEDError(
            "%s: both the RGB and indexed page changed; edit one of them"
            % os.path.dirname(png or idx))
    if not os.path.isfile(idx):
        raise jc.JMPPEDError("%s: indexed page is missing" % idx)

    if pal_changed:
        cluts = jb._load_clut_strip(pal, len(cluts))
    if idx_changed:
        indices = jb._load_indexed(idx, jc.PAGE_W, jc.PAGE_H)
    elif page_changed:
        indices = jb._load_page_rgb(png, cluts, display)
    elif indices is None:
        indices = jb._load_indexed(idx, jc.PAGE_W, jc.PAGE_H)

    touched = bool(page_changed or idx_changed or pal_changed or base_index is None)
    return {
        "set": setid,
        "index": base_index,
        "page": base_page,
        "raw": base_raw,
        "indices": indices,
        "cluts": cluts,
        "display": display,
        "touched": touched,
        "full_page_touched": bool(page_changed or idx_changed),
        "path": png,
    }


def build_v5(out_root: str, dest: str | None, copy_to: str | None,
             dry_run: bool, use: str, entry: str | None = None) -> int:
    out_root = os.path.abspath(out_root)
    manifest = je.load_manifest(out_root)
    if manifest.get("archive_version") not in (jc.VERSION_V3, jc.VERSION_V5):
        raise jc.JMPPEDError("unsupported archive_version %r"
                             % manifest.get("archive_version"))
    if entry:
        je.entry(manifest, entry)

    base_path = jb._abs(out_root, manifest["base_archive"])
    if not os.path.isfile(base_path):
        raise jc.JMPPEDError("base archive %s is missing" % base_path)
    if jc.sha256_file(base_path) != manifest["base_archive_sha256"]:
        raise jc.JMPPEDError("base archive hash does not match the manifest")
    arch = jc.read_archive(base_path)
    if arch.version != jc.VERSION_V3 or len(arch.models) != 14:
        raise jc.JMPPEDError("base archive is v%d with %d models; expected v3/14"
                             % (arch.version, len(arch.models)))

    entries = manifest["entries"]
    if not entries:
        raise jc.JMPPEDError("manifest has no entries[]")

    models_by_entry: list[tuple[list[jc.Model], list[bytes]]] = []
    states: dict[int, dict] = {}
    changed_models = 0

    for entry_index, e in enumerate(entries):
        je.validate_entry(manifest, e)
        page_sets = {int(s) for s in e["page_sets"]}
        used_slots: set[int] = set()
        models: list[jc.Model] = []
        raw_models: list[bytes] = []

        for slot_desc in e["slots"]:
            slot = int(slot_desc["slot"])
            if slot < 0 or slot >= len(arch.models):
                raise jc.JMPPEDError("%s/%s maps to invalid slot %d"
                                     % (e["name"], slot_desc["part"], slot))
            if slot in used_slots:
                raise jc.JMPPEDError("%s: slot %d is claimed more than once"
                                     % (e["name"], slot))
            used_slots.add(slot)
            if slot_desc["part"] != arch.model_names[slot]:
                raise jc.JMPPEDError("%s/%s maps to slot %d, whose base model is %s"
                                     % (e["name"], slot_desc["part"], slot,
                                        arch.model_names[slot]))

            edit = jb._abs(out_root, slot_desc["obj_edit"])
            raw = jb._abs(out_root, slot_desc["obj_raw"])
            edit_changed = jb._changed(edit, slot_desc["obj_edit_sha256"])
            raw_changed = jb._changed(raw, slot_desc["obj_raw_sha256"])
            transform = jb._part_transform(
                slot_desc, "%s/%s" % (e["name"], slot_desc["part"]))
            transform_changed = not jb._transform_is_default(transform)
            if use == "edit":
                edit_changed = True
            elif use == "raw":
                raw_changed = True
            if edit_changed and raw_changed and use == "auto":
                raise jc.JMPPEDError(
                    "%s/%s: both obj/edit and obj/raw changed; edit one of them"
                    % (e["name"], slot_desc["part"]))

            # Entry 0 can copy the base blob when untouched. Every later entry
            # must be rebuilt even when cloned unchanged: its materials name its
            # own page sets, not entry 0's.
            force = entry_index > 0
            if force:
                path = raw if (use == "raw" or (raw_changed and not edit_changed)) else edit
                blender = path == edit
                blob = jb._load_model(path, blender=blender, model=arch.models[slot],
                                      page_sets=page_sets,
                                      slot_name="%s/%s" % (e["name"], slot_desc["part"]),
                                      offset=transform[0], scale=transform[1])
                model = jc.parse_model(blob)
                changed_models += 1
            elif edit_changed or raw_changed or transform_changed:
                if edit_changed:
                    path = edit
                    blender = True
                elif raw_changed:
                    path = raw
                    blender = False
                else:
                    path = edit
                    blender = True
                blob = jb._load_model(path, blender=blender,
                                      model=arch.models[slot], page_sets=page_sets,
                                      slot_name="%s/%s" % (e["name"], slot_desc["part"]),
                                      offset=transform[0], scale=transform[1])
                model = jc.parse_model(blob)
                changed_models += 1
                print("model %-20s rebuilt from %s/%s" %
                      (arch.model_names[slot], e["name"],
                       os.path.basename(path)))
            else:
                blob = arch.raw_models[slot]
                model = arch.models[slot]
            models.append(model)
            raw_models.append(blob)

        if used_slots != set(range(14)):
            missing = sorted(set(range(14)) - used_slots)
            raise jc.JMPPEDError("%s: the manifest does not cover slots %s"
                                 % (e["name"], missing))
        models_by_entry.append((models, raw_models))

        for page_desc in e["pages"]:
            st = _entry_page_state(page_desc, arch, out_root)
            if st["set"] in states:
                raise jc.JMPPEDError("page set %d appears in more than one entry"
                                     % st["set"])
            states[st["set"]] = st

    changed_crops = 0
    for e in entries:
        for slot_desc in e["slots"]:
            for crop in slot_desc.get("crops", []):
                page_set = int(crop["page_set"])
                if page_set not in states:
                    raise jc.JMPPEDError(
                        "%s/%s: crop references page set %d, which the entry does "
                        "not carry" % (e["name"], slot_desc["part"], page_set))
                before = states[page_set]["touched"]
                texture_ids = sorted({int(m["id"])
                                      for m in slot_desc.get("materials", [])
                                      if int(m["set"]) == page_set})
                jb._apply_crop(states[page_set], crop, out_root, texture_ids)
                if states[page_set]["touched"] and not before:
                    changed_crops += 1

    changed_pages = 0
    out_entries: list[jc.Entry] = []
    for e, (models, raw_models) in zip(entries, models_by_entry):
        pages: list[jc.Page] = []
        raw_pages: list[bytes] = []
        for page_desc in e["pages"]:
            st = states[int(page_desc["set"])]
            if st["touched"] or st["index"] is None:
                pages.append(jc.make_raw_page(st["set"], st["indices"], st["cluts"]))
                raw_pages.append(b"")
                changed_pages += 1
            else:
                pages.append(st["page"])
                raw_pages.append(st["raw"] or b"")
        out_entries.append(jc.Entry(e["name"], models, pages, raw_models, raw_pages))

    arch5 = jc.Archive(version=jc.VERSION_V5, model_names=jc.MODEL_NAMES,
                       models=out_entries[0].models, pages=out_entries[0].pages,
                       raw_models=out_entries[0].raw_models,
                       raw_pages=out_entries[0].raw_pages,
                       entries=out_entries)
    data = jc.archive_bytes(arch5)

    expected = None
    base_v5 = manifest.get("base_v5_archive")
    if base_v5 and len(out_entries) == 1:
        p = jb._abs(out_root, base_v5)
        if os.path.isfile(p):
            expected = open(p, "rb").read()
    unchanged = expected is not None and data == expected
    if dry_run:
        print("dry run: %d entries, %d model(s), %d page(s); %d bytes" %
              (len(out_entries), changed_models, changed_pages, len(data)))
        return 0

    dest = dest or os.path.join(out_root, "JMPPED.LEV")
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes, %d entries, %s)" %
          (dest, len(data), len(out_entries),
           "byte-identical to base" if unchanged else
           "%d model(s), %d page(s), %d crop(s) changed" %
           (changed_models, changed_pages, changed_crops)))

    check = jc.read_archive(dest)
    if check.version != jc.VERSION_V5 or len(check.entries) != len(out_entries):
        raise jc.JMPPEDError("self-check failed: reread archive is v%d/%d entries"
                             % (check.version, len(check.entries)))
    if jc.archive_bytes(check) != data:
        raise jc.JMPPEDError("self-check failed: reread archive does not re-serialize")
    if copy_to:
        shutil.copy2(dest, copy_to)
        print("copied to %s" % copy_to)
    return 0
