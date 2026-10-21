#!/usr/bin/env python3
"""Bake Jericho's pedestrian models and pages into a self-contained JMPPED.LEV.

The six JERI_* models live in Havana's LUMP_MODELS. Their two pages (texture
sets 23 and 25) are streamed area pages, not permanent/special pages: each is
17 sectors, one CLUT sector followed by a 64x256 raw image. This tool writes a
small file the runtime loader can consume on any city:

    "JMPPED\0\0" | u32 version | u32 model_count
    for each model: u32 size, raw MODEL blob (MODEL-relative offsets)
    u32 page_count
    for each page: u32 set, u32 npalettes, u32 clut_bytes, clut,
                   u32 image_bytes, raw 64x256 image

Usage:
    python bake_jmpped.py [HAVANA.LEV] [JMPPED.LEV]
"""

import os
import struct
import sys

MAGIC = b"JMPPED\0\0"
VERSION = 2
MODEL_NAMES = [
    "JERI_TORSO",
    "JERI_HEAD",
    "JERI_U_ARM_LEFT",
    "JERI_L_ARM_LEFT",
    "JERI_U_ARM_RIGHT",
    "JERI_L_ARM_RIGHT",
]
LEG_NAMES = [
    "THIGH_LEFT",
    "CALF_LEFT",
    "FOOT_LEFT",
    "THIGH_RIGHT",
    "CALF_RIGHT",
    "FOOT_RIGHT",
]
ALL_NAMES = MODEL_NAMES + LEG_NAMES
POLY_SIZES = [8, 12, 16, 24, 20, 20, 28, 32, 8, 12, 16, 16,
              0, 0, 0, 0, 12, 12, 12, 16, 20, 20, 24, 24,
              0, 0, 0, 0, 0, 0, 0, 0, 8, 12, 16, 24,
              20, 24, 28, 32, 0, 0, 0, 0, 0, 0, 0, 0,
              12, 12, 12, 16, 20, 20, 24, 24]
TEXTURED = {4, 5, 6, 7, 20, 21, 22, 23}


def first_texture(blob):
    poly = struct.unpack_from("<i", blob, 20)[0]
    count = struct.unpack_from("<H", blob, 14)[0]
    for _ in range(count):
        kind = blob[poly] & 0x1f
        if kind in TEXTURED:
            return blob[poly + 1], blob[poly + 2]
        step = POLY_SIZES[kind] if kind < len(POLY_SIZES) else 0
        if step <= 0:
            break
        poly += step
    return 0, 0


def retexture(blob, texture_set, texture_id):
    poly = struct.unpack_from("<i", blob, 20)[0]
    count = struct.unpack_from("<H", blob, 14)[0]
    for _ in range(count):
        kind = blob[poly] & 0x1f
        if kind in TEXTURED:
            blob[poly + 1] = texture_set
            blob[poly + 2] = texture_id
        step = POLY_SIZES[kind] if kind < len(POLY_SIZES) else 0
        if step <= 0:
            break
        poly += step
PAGE_SETS = (23, 25)
SECTOR = 2048


def find_lump(body, want):
    off = 0
    while off + 8 <= len(body):
        kind, size = struct.unpack_from("<ii", body, off)
        if size < 0 or off + 8 + size > len(body):
            return None
        if kind == want:
            return body[off + 8:off + 8 + size]
        off += 8 + ((size + 3) & ~3)
    return None


def find_model_blob(models, index):
    """LUMP_MODELS is a count followed by size-prefixed blobs. Empty slots use
    a zero size and consume only their 4-byte length."""
    if models is None or len(models) < 4:
        raise RuntimeError("no LUMP_MODELS")
    count = struct.unpack_from("<i", models, 0)[0]
    cur = 4
    for i in range(count):
        if cur + 4 > len(models):
            break
        size = struct.unpack_from("<i", models, cur)[0]
        if i == index:
            if size <= 0:
                raise RuntimeError("model %d has no data" % index)
            return models[cur + 4:cur + 4 + size]
        cur += 4 + max(size, 0)
    raise RuntimeError("model %d is past the model table" % index)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", "..", "..", ".."))
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "src_rebuild", "bin", "Release", "DRIVER2", "LEVELS", "HAVANA.LEV")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "JMPPED.LEV")

    data = open(src, "rb").read()
    table = struct.unpack_from("<8I", data, 8)
    data1 = data[table[0]:table[0] + table[1]]
    data2 = data[table[4]:table[4] + table[5]]
    spool_base = table[6]

    names = find_lump(data1[8:], 12)
    if names is None:
        raise RuntimeError("no LUMP_MODELNAMES")
    name_list = names.split(b"\0")

    models = find_lump(data2[8:], 1)
    blobs = []
    source_indices = []
    for name in ALL_NAMES:
        try:
            index = name_list.index(name.encode("ascii"))
        except ValueError:
            raise RuntimeError("missing model name %s" % name)
        blob = find_model_blob(models, index)
        blobs.append(bytearray(blob))
        source_indices.append(index)
        print("model %-18s index %4d %6d bytes" % (name, index, len(blob)))

    # A DAM instance names its parent by the source model index. Rewrite that
    # to the local 0..5 index so the runtime copy can resolve it without the
    # Havana model table.
    for i, blob in enumerate(blobs):
        instance = struct.unpack_from("<h", blob, 4)[0]
        if instance >= 0:
            if instance not in source_indices:
                raise RuntimeError("%s is an instance of model %d, not one of the baked models"
                                   % (ALL_NAMES[i], instance))
            local = source_indices.index(instance)
            struct.pack_into("<h", blob, 4, local)
            print("  %s instance -> local %d" % (ALL_NAMES[i], local))

    suit_set, suit_id = first_texture(blobs[0])
    print("suit texture (torso): set %d id %d" % (suit_set, suit_id))
    # Keep the legs' own Tanner texture set/id. The palette system tints those
    # resident Tanner pages to the player's suit colour; rewriting them to the
    # torso page makes the legs sample the wrong part of the suit atlas.

    spool = find_lump(data2[8:], 26)
    if spool is None:
        raise RuntimeError("no LUMP_SPOOLINFO")

    ptr = 4
    music_size = struct.unpack_from("<i", spool, ptr)[0]
    ptr += 4 + music_size
    nareas = struct.unpack_from("<i", spool, ptr)[0]
    ptr += 4
    areas = ptr
    pages_at = areas + nareas * 16

    pages = {}
    print("spool areas", nareas)
    for a in range(nareas):
        gfx = struct.unpack_from("<H", spool, areas + a * 16)[0]
        ntpages = spool[areas + a * 16 + 10]
        page_list = spool[pages_at + a * 16: pages_at + a * 16 + ntpages]
        print("  area", a, "gfx", gfx, "pages", list(page_list))
        for i, page_set in enumerate(page_list):
            if page_set in PAGE_SETS:
                pages[page_set] = (gfx + i * 17, a, i)
    if len(pages) != len(PAGE_SETS):
        raise RuntimeError("area page sets not found: %r" % (pages,))

    baked_pages = []
    for page_set in PAGE_SETS:
        sector, area, index = pages[page_set]
        off = spool_base + sector * SECTOR
        buf = data[off:off + 17 * SECTOR]
        if len(buf) != 17 * SECTOR:
            raise RuntimeError("short page for set %d" % page_set)
        npalettes = struct.unpack_from("<i", buf, 0)[0]
        if npalettes <= 0 or npalettes > 256:
            raise RuntimeError("bad palette count %d for set %d" % (npalettes, page_set))
        clut_rows = npalettes // 4 + 1
        clut = buf[4:4 + clut_rows * 128]
        image = buf[SECTOR:SECTOR + 64 * 256 * 2]
        baked_pages.append((page_set, npalettes, clut, image))
        print("page  set %2d area %d page %d sector %d: %d palettes, %d clut bytes, %d image bytes"
              % (page_set, area, index, sector, npalettes, len(clut), len(image)))

    out = bytearray(MAGIC)
    out += struct.pack("<II", VERSION, len(blobs))
    for blob in blobs:
        out += struct.pack("<I", len(blob))
        out += blob
    out += struct.pack("<I", len(baked_pages))
    for page_set, npalettes, clut, image in baked_pages:
        out += struct.pack("<IIII", page_set, npalettes, len(clut), len(image))
        out += clut
        out += image

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    open(dst, "wb").write(out)
    print("wrote %s (%d bytes)" % (dst, len(out)))

    release = os.path.join(root, "src_rebuild", "bin", "Release", "DRIVER2", "LEVELS", "JMPPED.LEV")
    if os.path.abspath(release) != os.path.abspath(dst):
        os.makedirs(os.path.dirname(release), exist_ok=True)
        open(release, "wb").write(out)
        print("copied to %s" % release)


if __name__ == "__main__":
    main()
