#!/usr/bin/env python3
"""jmpped_build.py - rebuild the v3 JMPPED archive from Tanner/Jericho edits.

The export workspace groups the Driver 2 source models by ped:

    tanner/obj/{raw,edit}/*.obj   full Tanner set (only eight limbs are baked)
    jericho/obj/{raw,edit}/*.obj  six JERI_* parts
    tanner|jericho/png/...        pages, palette strips and per-part crops
    base.LEV                      untouched v3 archive from the same level
    jmpped.json                   slot map, page storage, transforms and hashes

JMPPED v3 has 14 slots: slot 0..5 are Jericho, 6..13 are Tanner's limbs. Other
Tanner parts are exported for reference but cannot be stored in v3; editing one
prints a warning and does not change the archive.

Usage:
    python jmpped_build.py --out tools/out/jmppeds/v3
    python jmpped_build.py --out ... --copy-to DRIVER2/LEVELS/JMPPED.LEV
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import jmpped_common as jc


HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "out", "jmppeds", "v3")


def _changed(path: str, expected: str) -> bool:
    return os.path.isfile(path) and jc.sha256_file(path) != expected


def _abs(root: str, rel: str) -> str:
    return os.path.join(root, rel.replace("/", os.sep))


def _material(name: str | None) -> tuple[int, int, tuple[int, int, int]]:
    if name is None:
        raise jc.JMPPEDError("a face has no material; keep the exported usemtl lines")
    if name.startswith("s") and "_t" in name:
        try:
            set_s, id_s = name[1:].split("_t", 1)
            return int(set_s), int(id_s), (0x7F, 0x7F, 0x7F)
        except ValueError:
            pass
    if name.startswith("color_"):
        try:
            _, r, g, b = name.split("_")
            return 0, 0, (int(r) & 0xFF, int(g) & 0xFF, int(b) & 0xFF)
        except ValueError:
            pass
    raise jc.JMPPEDError("unrecognised material %r; expected sSET_tID or color_R_G_B"
                         % name)


def _polys_from_obj(faces, path: str) -> list[jc.Poly]:
    out = []
    for fi, face in enumerate(faces):
        if len(face) not in (3, 4):
            raise jc.JMPPEDError("%s: face %d has %d corners; only triangles and "
                                 "quads are supported" % (path, fi + 1, len(face)))
        vs = [c[0] for c in face]
        if any(v < 0 for v in vs):
            raise jc.JMPPEDError("%s: face %d has an invalid vertex index"
                                 % (path, fi + 1))
        setid, texid, color = _material(face[0][2])
        uvs = []
        for vi, uv, _mat in face:
            if uv is None:
                raise jc.JMPPEDError("%s: face %d has a corner without a UV"
                                     % (path, fi + 1))
            u = int(round(uv[0] * 255.0))
            v = int(round((1.0 - uv[1]) * 255.0))
            uvs.append((max(0, min(255, u)), max(0, min(255, v))))
        out.append(jc.Poly(kind=jc.POLY_FT3 if len(vs) == 3 else jc.POLY_FT4,
                           set=setid, id=texid, v=vs, uv=uvs, color=color))
    if not out:
        raise jc.JMPPEDError("%s has no faces" % path)
    return out


def _load_model(path: str, blender: bool, model: jc.Model,
                page_sets: set[int], slot_name: str) -> bytes:
    if model.normals or model.point_normals:
        raise jc.JMPPEDError(
            "%s (%s): this model has normal tables; the OBJ path only rebuilds "
            "FT3/FT4 ped models. Leave the slot untouched or use a model-aware "
            "tool." % (slot_name, path))
    if any(not p.supported_for_rebuild for p in model.polys):
        raise jc.JMPPEDError(
            "%s (%s): the original model uses a poly kind this toolkit cannot "
            "rebuild; leave the slot untouched" % (slot_name, path))
    verts, faces = jc.read_obj(path, blender=blender)
    ints = [(int(round(x)), int(round(y)), int(round(z))) for x, y, z in verts]
    try:
        polys = _polys_from_obj(faces, path)
    except jc.JMPPEDError as e:
        raise jc.JMPPEDError("%s: %s" % (slot_name, e)) from e
    for p in polys:
        if p.textured and p.set not in page_sets:
            raise jc.JMPPEDError(
                "%s: a poly references texture page %d, which this archive does "
                "not carry (pages: %s)"
                % (slot_name, p.set, ", ".join(str(s) for s in sorted(page_sets))))
    return jc.build_model_blob(model, ints, polys)


def _check_page_rect(rect, path: str) -> tuple[int, int, int, int]:
    if len(rect) != 4:
        raise jc.JMPPEDError("%s: crop rect must be [x,y,w,h]" % path)
    x, y, w, h = (int(v) for v in rect)
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > jc.PAGE_W or y + h > jc.PAGE_H:
        raise jc.JMPPEDError("%s: crop rect %r is outside the %dx%d page"
                             % (path, rect, jc.PAGE_W, jc.PAGE_H))
    return x, y, w, h


def _load_page_rgb(path: str, cluts: list[bytes], display_clut: int) -> bytes:
    w, h, pixels = jc.load_png_rgb(path)
    if w != jc.PAGE_W or h != jc.PAGE_H:
        raise jc.JMPPEDError("%s is %dx%d, expected %dx%d"
                             % (path, w, h, jc.PAGE_W, jc.PAGE_H))
    clut = jc.clut_rgb(cluts[display_clut]) if cluts else [(0, 0, 0)] * 16
    return bytes(jc.nearest_clut_index(c, clut) for c in pixels)


def _load_indexed(path: str, width: int, height: int) -> bytes:
    w, h, indices = jc.load_png_indices(path)
    if w != width or h != height:
        raise jc.JMPPEDError("%s is %dx%d, expected %dx%d"
                             % (path, w, h, width, height))
    return indices


def _load_clut_strip(path: str, count: int) -> list[bytes]:
    w, h, pixels = jc.load_png_rgb(path)
    if w != jc.CLUT_ROW_ENTRIES or h != count:
        raise jc.JMPPEDError("%s is %dx%d, expected %dx%d (one row per CLUT)"
                             % (path, w, h, jc.CLUT_ROW_ENTRIES, count))
    rows = []
    for y in range(h):
        rows.append(jc.rgb_to_clut(pixels[y * w:(y + 1) * w]))
    return rows


def _page_state(arch: jc.Archive, entry: dict, root: str) -> dict:
    page_set = int(entry["set"])
    index = next((i for i, p in enumerate(arch.pages) if p.set == page_set), None)
    if index is None:
        raise jc.JMPPEDError("manifest page set %d is not in the base archive"
                             % page_set)
    page = arch.pages[index]
    indices = jc.page_indices(page)
    cluts = jc.page_cluts(page)
    display = int(entry.get("display_clut", 0))
    if display < 0 or display >= len(cluts):
        display = 0

    png = _abs(root, entry["png"])
    idx = _abs(root, entry["indexed_png"])
    pal = _abs(root, entry["palette_png"])
    page_changed = _changed(png, entry["png_sha256"])
    idx_changed = _changed(idx, entry["indexed_png_sha256"])
    pal_changed = _changed(pal, entry["palette_png_sha256"])
    if page_changed and idx_changed:
        raise jc.JMPPEDError(
            "%s: both the RGB and indexed page changed; edit one of them"
            % os.path.dirname(png))

    if pal_changed:
        cluts = _load_clut_strip(pal, len(cluts))
    if idx_changed:
        indices = _load_indexed(idx, jc.PAGE_W, jc.PAGE_H)
    elif page_changed:
        indices = _load_page_rgb(png, cluts, display)

    return {
        "set": page_set,
        "index": index,
        "page": page,
        "indices": indices,
        "cluts": cluts,
        "display": display,
        "touched": bool(page_changed or idx_changed or pal_changed),
        "full_page_touched": bool(page_changed or idx_changed),
        "path": png,
    }


def _apply_crop(state: dict, crop: dict, root: str) -> None:
    x, y, w, h = _check_page_rect(crop["rect"], crop.get("rgb", "crop"))
    rgb_path = _abs(root, crop["rgb"])
    idx_path = _abs(root, crop["indexed"])
    rgb_changed = _changed(rgb_path, crop["rgb_sha256"])
    idx_changed = _changed(idx_path, crop["indexed_sha256"])
    if rgb_changed and idx_changed:
        raise jc.JMPPEDError("%s: both the RGB and indexed crop changed; edit one"
                             % os.path.dirname(rgb_path))
    if state["full_page_touched"]:
        if rgb_changed or idx_changed:
            raise jc.JMPPEDError(
                "%s: the full page and a per-part crop both changed; edit one"
                % state["path"])
        return
    if not (rgb_changed or idx_changed):
        return

    if idx_changed:
        patch = _load_indexed(idx_path, w, h)
    else:
        _, _, pixels = jc.load_png_rgb(rgb_path)
        if len(pixels) != w * h:
            raise jc.JMPPEDError("%s is %dx%d, expected %dx%d"
                                 % (rgb_path, len(pixels), 1, w, h))
        clut = (jc.clut_rgb(state["cluts"][state["display"]])
                if state["cluts"] else [(0, 0, 0)] * 16)
        patch = bytes(jc.nearest_clut_index(c, clut) for c in pixels)

    indices = bytearray(state["indices"])
    for row in range(h):
        dst = (y + row) * jc.PAGE_W + x
        src = row * w
        indices[dst:dst + w] = patch[src:src + w]
    state["indices"] = bytes(indices)
    state["touched"] = True


def build(out_root: str, dest: str | None, copy_to: str | None,
          dry_run: bool, use: str) -> int:
    manifest_path = os.path.join(out_root, "jmpped.json")
    if not os.path.isfile(manifest_path):
        raise jc.JMPPEDError("%s not found; run jmpped_export.py first"
                             % manifest_path)
    manifest = jc.load_json(manifest_path)
    if manifest.get("manifest_version") != 2:
        raise jc.JMPPEDError("unsupported manifest version %r; re-export the "
                             "Tanner/Jericho workspace"
                             % manifest.get("manifest_version"))
    if manifest.get("archive_version") != jc.VERSION_V3:
        raise jc.JMPPEDError("this builder only writes v3 JMPPED archives")

    base_path = _abs(out_root, manifest["base_archive"])
    if not os.path.isfile(base_path):
        raise jc.JMPPEDError("base archive %s is missing" % base_path)
    if jc.sha256_file(base_path) != manifest["base_archive_sha256"]:
        raise jc.JMPPEDError("base archive hash does not match the manifest")
    arch = jc.read_archive(base_path)
    if arch.version != jc.VERSION_V3 or len(arch.models) != 14:
        raise jc.JMPPEDError("base archive is v%d with %d models; expected v3/14"
                             % (arch.version, len(arch.models)))

    page_sets = {p.set for p in arch.pages}
    used_slots: set[int] = set()
    changed_models = 0
    crop_parts: list[tuple[str, dict]] = []

    for ped_name, ped in manifest["peds"].items():
        for entry in ped["parts"]:
            crop_parts.append((ped_name, entry))
            slot = entry.get("bake_slot")
            edit = _abs(out_root, entry["obj_edit"])
            raw = _abs(out_root, entry["obj_raw"])
            edit_changed = _changed(edit, entry["obj_edit_sha256"])
            raw_changed = _changed(raw, entry["obj_raw_sha256"])

            if slot is None:
                if edit_changed or raw_changed:
                    print("%s/%s is exported for reference but is not part of "
                          "the 14-slot v3 archive; edit ignored"
                          % (ped_name, entry["name"]))
                continue

            slot = int(slot)
            if slot < 0 or slot >= len(arch.models):
                raise jc.JMPPEDError("%s/%s maps to invalid slot %d"
                                     % (ped_name, entry["name"], slot))
            if slot in used_slots:
                raise jc.JMPPEDError("slot %d is claimed more than once" % slot)
            if entry["name"] != arch.model_names[slot]:
                raise jc.JMPPEDError("%s/%s maps to slot %d, whose base model is %s"
                                     % (ped_name, entry["name"], slot,
                                        arch.model_names[slot]))
            used_slots.add(slot)

            if use == "edit":
                edit_changed = True
            elif use == "raw":
                raw_changed = True
            if edit_changed and raw_changed and use == "auto":
                raise jc.JMPPEDError(
                    "%s/%s: both obj/edit and obj/raw changed; edit one of them"
                    % (ped_name, entry["name"]))
            if not (edit_changed or raw_changed):
                continue
            path = edit if edit_changed else raw
            blob = _load_model(path, blender=edit_changed, model=arch.models[slot],
                               page_sets=page_sets,
                               slot_name="%s/%s" % (ped_name, entry["name"]))
            arch.models[slot] = jc.parse_model(blob)
            arch.raw_models[slot] = blob
            changed_models += 1
            print("model %-20s rebuilt from %s/%s" %
                  (arch.model_names[slot], ped_name,
                   os.path.basename(path)))

    if used_slots != set(range(14)):
        missing = sorted(set(range(14)) - used_slots)
        raise jc.JMPPEDError("the manifest does not cover v3 slots %s" % missing)

    # --- pages and crops ------------------------------------------------
    states: dict[int, dict] = {}
    for entry in manifest["pages"]:
        st = _page_state(arch, entry, out_root)
        if st["set"] in states:
            raise jc.JMPPEDError("page set %d appears twice in the manifest"
                                 % st["set"])
        states[st["set"]] = st

    changed_crops = 0
    for _ped_name, entry in crop_parts:
        for crop in entry.get("crops", []):
            page_set = int(crop["page_set"])
            if page_set not in states:
                raise jc.JMPPEDError("%s: crop references page set %d, which the "
                                     "manifest does not carry"
                                     % (entry["name"], page_set))
            before = states[page_set]["touched"]
            _apply_crop(states[page_set], crop, out_root)
            if states[page_set]["touched"] and not before:
                changed_crops += 1

    changed_pages = 0
    for st in states.values():
        if not st["touched"]:
            continue
        arch.pages[st["index"]] = jc.make_raw_page(st["set"], st["indices"], st["cluts"])
        arch.raw_pages[st["index"]] = b""
        changed_pages += 1
        print("page set %3d rebuilt as a raw page" % st["set"])

    # --- write ----------------------------------------------------------
    data = jc.archive_bytes(arch)
    original = open(base_path, "rb").read()
    if dry_run:
        print("dry run: %d model(s), %d page(s); %d bytes" %
              (changed_models, changed_pages, len(data)))
        return 0

    dest = dest or os.path.join(out_root, "JMPPED.LEV")
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(data)
    unchanged = data == original
    print("wrote %s (%d bytes, %s)" %
          (dest, len(data), "byte-identical to base" if unchanged else
           "%d model(s), %d page(s) changed" % (changed_models, changed_pages)))

    check = jc.read_archive(dest)
    if check.version != jc.VERSION_V3 or len(check.models) != 14:
        raise jc.JMPPEDError("self-check failed: reread archive is v%d/%d models"
                             % (check.version, len(check.models)))
    if jc.archive_bytes(check) != data:
        raise jc.JMPPEDError("self-check failed: reread archive does not re-serialize")
    if copy_to:
        shutil.copy2(dest, copy_to)
        print("copied to %s" % copy_to)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Rebuild v3 JMPPED from Tanner/Jericho edits")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="export workspace containing jmpped.json")
    ap.add_argument("--dest", default=None,
                    help="output archive (default: <out>/JMPPED.LEV)")
    ap.add_argument("--copy-to", default=None,
                    help="copy the v3 archive to this path")
    ap.add_argument("--use", choices=("auto", "edit", "raw"), default="auto",
                    help="which OBJ copy to read (auto: whichever changed)")
    ap.add_argument("--dry-run", action="store_true", help="report without writing")
    args = ap.parse_args(argv)
    try:
        return build(args.out, args.dest, args.copy_to, args.dry_run, args.use)
    except jc.JMPPEDError as e:
        print("jmpped_build: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
