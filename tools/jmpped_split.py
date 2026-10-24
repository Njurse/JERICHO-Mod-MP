#!/usr/bin/env python3
"""jmpped_split.py - split an edited bindpose OBJ back into the ped slotted OBJs.

Edit `jericho/obj/bindpose_edit.obj` (the whole playable character, including
Tanner's limbs) in Blender, then run:

    python jmpped_split.py --out tools/out/jmppeds/v3 --and-build

The combined OBJ is in bind space: every part is at its bone's engine position.
The splitter subtracts that bind offset, resets the part to model-local space,
writes only the groups whose geometry changed to `obj/edit/<part>.obj`, and can
run the normal builder. The builder's first-class per-part path then bakes the
14 v3 slots. Unchanged groups are not rewritten.

Use --variant tanner for `tanner/obj/bindpose_edit.obj`; v3 only stores the
eight Tanner limbs, so the extra Tanner parts are written as reference and the
builder warns if you edit them.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import jmpped_common as jc
import jmpped_build
import jmpped_entries as je


HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "out", "jmppeds", "v3")


def _read_groups(path: str) -> dict[str, dict]:
    """Parse a combined OBJ into per-o groups, in raw engine coordinates."""
    verts_all: list[tuple[float, float, float]] = []
    uvs_all: list[tuple[float, float]] = []
    groups: list[dict] = []
    current = None

    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if p[0] == "o":
                current = {"name": p[1] if len(p) > 1 else "default",
                           "vstart": len(verts_all), "uvstart": len(uvs_all),
                           "faces": [], "material": None}
                groups.append(current)
            elif p[0] == "v" and len(p) >= 4:
                x, y, z = float(p[1]), float(p[2]), float(p[3])
                x, y, z = x / jc.BLENDER_SCALE, -z / jc.BLENDER_SCALE, y / jc.BLENDER_SCALE
                verts_all.append((x, y, z))
            elif p[0] == "vt" and len(p) >= 3:
                uvs_all.append((float(p[1]), float(p[2])))
            elif p[0] == "usemtl" and len(p) >= 2:
                if current is None:
                    raise jc.JMPPEDError("%s: usemtl before any o group" % path)
                current["material"] = p[1]
            elif p[0] == "f" and len(p) >= 4:
                if current is None:
                    raise jc.JMPPEDError("%s: face before any o group" % path)
                face = []
                for tok in p[1:]:
                    bits = tok.split("/")
                    vi = int(bits[0]) - 1
                    ti = int(bits[1]) - 1 if len(bits) > 1 and bits[1] else None
                    face.append((vi, ti, current["material"]))
                current["faces"].append(face)

    out = {}
    for i, g in enumerate(groups):
        if g["name"] in out:
            raise jc.JMPPEDError("%s: duplicate o group %r" % (path, g["name"]))
        vend = groups[i + 1]["vstart"] if i + 1 < len(groups) else len(verts_all)
        uvend = groups[i + 1]["uvstart"] if i + 1 < len(groups) else len(uvs_all)
        vstart, uvstart = g["vstart"], g["uvstart"]
        in_range = (g["faces"] and vstart < vend and
                    all(vstart <= c[0] < vend for face in g["faces"] for c in face))
        if in_range:
            vmap = {j: j - vstart for j in range(vstart, vend)}
            umap = {j: j - uvstart for j in range(uvstart, uvend)}
            verts = [verts_all[j] for j in range(vstart, vend)]
            uvs = [uvs_all[j] for j in range(uvstart, uvend)]
        else:
            used_v, used_uv = [], []
            vmap, umap = {}, {}
            for face in g["faces"]:
                for vi, ti, _mat in face:
                    if vi not in vmap:
                        vmap[vi] = len(used_v)
                        used_v.append(vi)
                    if ti is not None and ti not in umap:
                        umap[ti] = len(used_uv)
                        used_uv.append(ti)
            verts = [verts_all[j] for j in used_v]
            uvs = [uvs_all[j] for j in used_uv]

        faces = []
        for face in g["faces"]:
            faces.append([
                (vmap[vi],
                 uvs[umap[ti]] if ti is not None and ti in umap else None,
                 mat)
                for vi, ti, mat in face
            ])
        out[g["name"]] = {"verts": verts, "faces": faces}
    return out


def _write_part(path: str, name: str, verts, faces, *, raw: bool) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    uv_count = 0
    uv_faces = []
    for face in faces:
        ids = []
        for _vi, uv, _mat in face:
            if uv is None:
                raise jc.JMPPEDError("%s: face without a UV" % name)
            uv_count += 1
            ids.append(uv_count)
        uv_faces.append(ids)

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# split from bindpose: %s\n" % name)
        f.write("o %s\n" % name)
        for x, y, z in verts:
            if raw:
                f.write("v %d %d %d\n" % (round(x), round(y), round(z)))
            else:
                f.write("v %.6f %.6f %.6f\n" %
                        (x * jc.BLENDER_SCALE, z * jc.BLENDER_SCALE,
                         -y * jc.BLENDER_SCALE))
        uv_i = 0
        for face in faces:
            for _vi, uv, _mat in face:
                f.write("vt %.6f %.6f\n" % (uv[0], uv[1]))
                uv_i += 1
        for face, uv_ids in zip(faces, uv_faces):
            f.write("usemtl %s\n" % (face[0][2] or "default"))
            f.write("f " + " ".join("%d/%d" % (c[0] + 1, uv_ids[j])
                                    for j, c in enumerate(face)) + "\n")


def _same_geometry(base: jc.Model, verts, polys) -> bool:
    if len(verts) != len(base.vertices) or len(polys) != len(base.polys):
        return False
    for a, b in zip(verts, base.vertices):
        if (int(round(a[0])), int(round(a[1])), int(round(a[2]))) != b:
            return False
    for a, b in zip(polys, base.polys):
        # The entry's page sets are remapped (e.g. 23/25/2 -> 26/27/3); the
        # geometry is unchanged. Compare the texture id/UVs, not the set.
        if (a.kind, a.id, a.v, a.uv) != (b.kind, b.id, b.v, b.uv):
            return False
        if not a.textured and a.color != b.color:
            return False
    return True


def split(out_root: str, entry_name: str, scene: str | None, raw: bool,
          and_build: bool) -> int:
    out_root = os.path.abspath(out_root)
    manifest = je.load_manifest(out_root)
    entry = je.entry(manifest, entry_name)
    je.validate_entry(manifest, entry)
    bindpose = entry["bindpose"]
    scene_path = scene or os.path.join(out_root, bindpose["edit"])
    scene_path = os.path.abspath(scene_path)
    if not os.path.isfile(scene_path):
        raise jc.JMPPEDError("combined scene %s not found" % scene_path)
    original = bindpose.get("edit_sha256")
    new_hash = jc.sha256_file(scene_path)
    if original and new_hash == original:
        print("%s is unchanged; nothing to split" % scene_path)
        return 0

    skeleton = jc.load_json(os.path.join(out_root, manifest["skeleton"]["json"]))
    bones = {b["name"]: b for b in skeleton["bones"]}
    variant = entry.get("variant", "jericho")
    bindings = skeleton["variants"][variant]["bindings"]
    bone_by_model = {model: bone for bone, model in bindings.items()}
    parts = {s["part"]: {"bake_slot": int(s["slot"]),
                         "obj_raw": s["obj_raw"],
                         "obj_edit": s["obj_edit"]}
             for s in entry["slots"]}

    arch = jc.read_archive(os.path.join(out_root, manifest["base_archive"]))
    groups = _read_groups(scene_path)
    written = 0
    skipped = 0
    for name, group in groups.items():
        bone_name = bone_by_model.get(name)
        if bone_name is None:
            raise jc.JMPPEDError("%s: group %r is not bound in the %s skeleton"
                                 % (scene_path, name, variant))
        offset = bones[bone_name]["engine"]
        verts = [(x - offset[0], y - offset[1], z - offset[2])
                 for x, y, z in group["verts"]]
        polys = jmpped_build._polys_from_obj(group["faces"], scene_path)

        part = parts.get(name)
        if part is None:
            # The bindpose carries a few reference-only bones (HIPS). They have
            # no v3 slot, so keep them beside the entry for editing but do not
            # pretend they bake.
            path = os.path.join(out_root, entry["directory"], "obj",
                                "raw" if raw else "edit", name + ".obj")
            _write_part(path, name, verts, group["faces"], raw=raw)
            written += 1
            print("%-18s -> %s (reference; no v3 slot)"
                  % (name, os.path.relpath(path, out_root)))
            continue

        slot = int(part["bake_slot"])
        if _same_geometry(arch.models[slot], verts, polys):
            skipped += 1
            continue
        path = os.path.join(out_root, part["obj_raw" if raw else "obj_edit"])
        _write_part(path, name, verts, group["faces"], raw=raw)
        written += 1
        print("%-18s -> %s" % (name, os.path.relpath(path, out_root)))

    # The combined scene has been consumed; remember its hash so a second split
    # without a new edit is a no-op.
    bindpose["edit_sha256"] = new_hash
    jc.save_json(os.path.join(out_root, "jmpped.json"), manifest)
    ped_json = os.path.join(out_root, entry["directory"], "ped.json")
    if os.path.isfile(ped_json):
        pj = jc.load_json(ped_json)
        if "bindpose" in pj:
            pj["bindpose"]["edit_sha256"] = new_hash
            jc.save_json(ped_json, pj)

    print("split %s: %d changed, %d unchanged" % (scene_path, written, skipped))
    if and_build:
        cmd = [sys.executable, os.path.join(HERE, "jmpped_build.py"),
               "--out", out_root, "--use", "auto"]
        print("+ " + " ".join(cmd))
        return subprocess.run(cmd, cwd=HERE).returncode
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Split an edited bindpose OBJ into the ped parts")
    ap.add_argument("--out", default=DEFAULT_OUT, help="toolkit workspace")
    ap.add_argument("--entry", default="Jericho",
                    help="entry name from jmpped.json (default Jericho)")
    ap.add_argument("--scene", default=None,
                    help="combined OBJ to read (default: that ped's bindpose_edit.obj)")
    ap.add_argument("--raw", action="store_true",
                    help="write engine-space obj/raw parts instead of Blender obj/edit")
    ap.add_argument("--and-build", action="store_true",
                    help="run jmpped_build.py after splitting")
    args = ap.parse_args(argv)
    try:
        return split(args.out, args.entry, args.scene, args.raw, args.and_build)
    except jc.JMPPEDError as e:
        print("jmpped_split: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
