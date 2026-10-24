#!/usr/bin/env python3
"""jmpped_common.py - shared reader/writer for the JMPPED v3/v4 archive.

The archive is the self-contained Jericho pedestrian bake the engine loads from
DRIVER2/LEVELS/JMPPED.LEV (see src_rebuild/Game/C/models.c, JerPedImportBaked):

    "JMPPED\\0\\0" | u32 version | u32 model_count
    model_count x (u32 size + MODEL blob)
    u32 page_count
    page_count x (u32 set, u32 npalettes, u32 clut_bytes, u32 image_bytes,
                   u32 flags, CLUT bytes, image bytes)

v4 appends the civilian/cop pack:

    u32 ped_model_index
    u32 ped_page_count + the same page records
    u32 ped_tex_count + (u32 texnum, x, y, w, h) records
    u32 ped_palette_count + (u32 texnum, u32 slot, 32-byte CLUT row) records

A MODEL is 36 bytes followed by vertices, optional normal tables and the poly
stream. Ped models use FT3/FT4 records (textured, no per-poly normals), so this
module preserves those records rather than re-encoding them as GT3/GT4.

The archive bytes are kept verbatim for untouched models and pages, so a
no-edit rebuild is byte-identical. The exporter's manifest records the hashes
and the editable views; the builder uses them to decide what changed.
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import zlib
from dataclasses import dataclass, field
from typing import Iterable, Sequence

MAGIC = b"JMPPED\x00\x00"
VERSION_V3 = 3
VERSION_V4 = 4
VERSION_V5 = 5

# v5 layout:
#   MAGIC                                     8
#   u32 version = 5                           4
#   u32 entry_count                           4
#   per entry:
#     u32 name_len, name bytes
#     u32 model_count (14)
#     model_count * (u32 size, MODEL blob)
#     u32 page_count
#     page_count * (u32 set, u32 npalettes, u32 clut_len, u32 image_len,
#                   u32 flags, clut, image)
# Entry 0 is the existing Jericho set (sets 23/25/2). Custom entries use their
# own page sets so pages and CLUTs cannot collide. The v3 reader stays for
# existing archives and the Havana base bake.
JMPPED_MAX_ENTRIES = 8
ENTRY_NAME_MAX = 63

# Fixed page-set allocation per entry. Entry 0 is the existing Jericho page set;
# every later entry owns three fresh sets, so their pages/CLUTs can never alias.
# The three values are (torso+arms, head, legs/hands).
ENTRY_PAGE_SETS = (
    (23, 25, 2),
    (26, 27, 3),
    (28, 29, 4),
    (30, 31, 5),
    (32, 33, 6),
    (34, 35, 7),
    (36, 37, 8),
    (38, 39, 9),
)


def entry_page_sets(index: int) -> tuple[int, int, int]:
    if index < 0 or index >= len(ENTRY_PAGE_SETS):
        raise JMPPEDError("entry %d has no page-set allocation" % index)
    return ENTRY_PAGE_SETS[index]


MODEL_HEADER_SIZE = 36
MODEL_HEADER_FMT = "<HHhBBhHHHiiiii"
PAGE_W = 256
PAGE_H = 256
PAGE_INDEX_BYTES = PAGE_W * PAGE_H // 2       # 4bpp page, two indices per byte
PAGE_BYTES = PAGE_W * PAGE_H                   # decoded index count
CLUT_ROW_BYTES = 32
CLUT_ROW_ENTRIES = 16
CLUT_GROUP_BYTES = 128                        # four CLUT rows
PAGE_FLAG_RAW = 0
PAGE_FLAG_COMPRESSED = 1
SECTOR = 2048

MODEL_NAMES = [
    "JERI_TORSO", "JERI_HEAD",
    "JERI_U_ARM_LEFT", "JERI_L_ARM_LEFT",
    "JERI_U_ARM_RIGHT", "JERI_L_ARM_RIGHT",
    "THIGH_LEFT", "CALF_LEFT", "FOOT_LEFT",
    "THIGH_RIGHT", "CALF_RIGHT", "FOOT_RIGHT",
    "HAND_LEFT", "HAND_RIGHT",
]
PED_HEAD_MODEL = "PEDHEAD1"
V4_MODEL_NAMES = MODEL_NAMES + [PED_HEAD_MODEL]
PED_TEXTURE_NAMES = ["CHEST1", "ARM1", "FOREARM1", "JEANS1", "LOWHEAD", "HEAD1"]
POLY_SIZES = [8, 12, 16, 24, 20, 20, 28, 32, 8, 12, 16, 16,
              0, 0, 0, 0, 12, 12, 12, 16, 20, 20, 24, 24,
              0, 0, 0, 0, 0, 0, 0, 0, 8, 12, 16, 24,
              20, 24, 28, 32, 0, 0, 0, 0, 0, 0, 0, 0,
              12, 12, 12, 16, 20, 20, 24, 24]
POLY_FT3 = 20
POLY_FT4 = 21
POLY_GT3 = 22
POLY_GT4 = 23
TEXTURED_KINDS = {4, 5, 6, 7, 20, 21, 22, 23}
POLY_NAMES = {POLY_FT3: "FT3", POLY_FT4: "FT4", POLY_GT3: "GT3", POLY_GT4: "GT4"}

# Blender's Z-up view. The raw OBJ is authoritative; this transform only exists
# for the editing copy.
BLENDER_SCALE = 1.0 / 1024.0


class JMPPEDError(Exception):
    """A malformed archive or an edit this toolkit cannot represent."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _u32(data: bytes, off: int) -> int:
    if off + 4 > len(data):
        raise JMPPEDError("truncated archive at offset %d" % off)
    return struct.unpack_from("<I", data, off)[0]


@dataclass
class Poly:
    kind: int
    set: int
    id: int
    v: list[int]
    uv: list[tuple[int, int]]
    n: list[int] = field(default_factory=list)
    color: tuple[int, int, int] = (0x7F, 0x7F, 0x7F)
    raw: bytes = b""

    @property
    def name(self) -> str:
        return POLY_NAMES.get(self.kind, "K%d" % self.kind)

    @property
    def textured(self) -> bool:
        return self.kind in TEXTURED_KINDS

    @property
    def supported_for_rebuild(self) -> bool:
        return self.kind in (POLY_FT3, POLY_FT4)


@dataclass
class Model:
    blob: bytes
    shape_flags: int
    flags2: int
    instance_number: int
    tri_verts: int
    z_bias: int
    bounding_sphere: int
    num_point_normals: int
    num_vertices: int
    num_polys: int
    vertices_offset: int
    poly_offset: int
    normals_offset: int
    point_normals_offset: int
    collision_offset: int
    vertices: list[tuple[int, int, int]] = field(default_factory=list)
    normals: list[tuple[int, int, int]] = field(default_factory=list)
    point_normals: list[tuple[int, int, int]] = field(default_factory=list)
    polys: list[Poly] = field(default_factory=list)
    collision: bytes = b""
    unsupported_polys: int = 0

    @property
    def slot_count(self) -> int:
        return len(self.polys)


def parse_model(blob: bytes) -> Model:
    if len(blob) < MODEL_HEADER_SIZE:
        raise JMPPEDError("MODEL blob is only %d bytes" % len(blob))
    h = struct.unpack_from(MODEL_HEADER_FMT, blob, 0)
    (shape_flags, flags2, instance_number, tri_verts, z_bias, bounding_sphere,
     num_point_normals, num_vertices, num_polys, vertices_offset, poly_offset,
     normals_offset, point_normals_offset, collision_offset) = h

    m = Model(blob=blob, shape_flags=shape_flags, flags2=flags2,
              instance_number=instance_number, tri_verts=tri_verts,
              z_bias=z_bias, bounding_sphere=bounding_sphere,
              num_point_normals=num_point_normals, num_vertices=num_vertices,
              num_polys=num_polys, vertices_offset=vertices_offset,
              poly_offset=poly_offset, normals_offset=normals_offset,
              point_normals_offset=point_normals_offset,
              collision_offset=collision_offset)

    if 0 <= vertices_offset and vertices_offset + num_vertices * 8 <= len(blob):
        for i in range(num_vertices):
            x, y, z, _pad = struct.unpack_from("<hhhh", blob, vertices_offset + i * 8)
            m.vertices.append((x, y, z))

    # A model with no normal table points both offsets at the poly block. Only
    # read a table when it is distinct and complete.
    normal_offset = normals_offset
    if num_polys and normal_offset != poly_offset and normal_offset >= 0:
        if normal_offset + num_polys * 8 <= len(blob):
            for i in range(num_polys):
                x, y, z, _pad = struct.unpack_from("<hhhh", blob, normal_offset + i * 8)
                m.normals.append((x, y, z))

    if num_point_normals and point_normals_offset != poly_offset and point_normals_offset >= 0:
        if point_normals_offset + num_point_normals * 8 <= len(blob):
            for i in range(num_point_normals):
                x, y, z, _pad = struct.unpack_from("<hhhh", blob, point_normals_offset + i * 8)
                m.point_normals.append((x, y, z))

    if 0 <= poly_offset < len(blob):
        p = poly_offset
        for _ in range(num_polys):
            if p >= len(blob):
                break
            kind = blob[p] & 0x1F
            step = POLY_SIZES[kind] if kind < len(POLY_SIZES) else 0
            if step <= 0 or p + step > len(blob):
                break
            rec = blob[p:p + step]
            poly = _parse_poly(rec, kind)
            if poly is None:
                m.unsupported_polys += 1
            else:
                m.polys.append(poly)
            p += step

    if collision_offset > 0 and collision_offset < len(blob):
        m.collision = blob[collision_offset:]

    return m


def _parse_poly(rec: bytes, kind: int) -> Poly | None:
    setid, texid = rec[1], rec[2]
    if kind == POLY_FT3:
        return Poly(kind=kind, set=setid, id=texid,
                    v=[rec[4], rec[5], rec[6]],
                    uv=[(rec[8], rec[9]), (rec[10], rec[11]), (rec[12], rec[13])],
                    color=tuple(rec[16:19]), raw=rec)
    if kind == POLY_FT4:
        return Poly(kind=kind, set=setid, id=texid,
                    v=[rec[4], rec[5], rec[6], rec[7]],
                    uv=[(rec[8], rec[9]), (rec[10], rec[11]),
                        (rec[12], rec[13]), (rec[14], rec[15])],
                    color=tuple(rec[16:19]), raw=rec)
    if kind == POLY_GT3:
        return Poly(kind=kind, set=setid, id=texid,
                    v=[rec[4], rec[5], rec[6]],
                    n=[rec[8], rec[9], rec[10]],
                    uv=[(rec[12], rec[13]), (rec[14], rec[15]), (rec[16], rec[17])],
                    color=tuple(rec[20:23]), raw=rec)
    if kind == POLY_GT4:
        return Poly(kind=kind, set=setid, id=texid,
                    v=[rec[4], rec[5], rec[6], rec[7]],
                    n=[rec[8], rec[9], rec[10], rec[11]],
                    uv=[(rec[12], rec[13]), (rec[14], rec[15]),
                        (rec[16], rec[17]), (rec[18], rec[19])],
                    color=tuple(rec[20:23]), raw=rec)
    return None


def _encode_poly(poly: Poly) -> bytes:
    if poly.kind not in (POLY_FT3, POLY_FT4):
        raise JMPPEDError(
            "slot poly kind %s cannot be rebuilt from OBJ; only FT3/FT4 are "
            "editable (leave this model untouched to copy it verbatim)"
            % poly.name)
    n = 3 if poly.kind == POLY_FT3 else 4
    rec = bytearray(20)
    rec[0] = poly.kind
    rec[1] = poly.set & 0xFF
    rec[2] = poly.id & 0xFF
    rec[4:4 + n] = bytes((v & 0xFF) for v in poly.v[:n])
    for i in range(n):
        u, v = poly.uv[i] if i < len(poly.uv) else (0, 0)
        rec[8 + i * 2] = u & 0xFF
        rec[9 + i * 2] = v & 0xFF
    rec[16:19] = bytes((c & 0xFF) for c in poly.color[:3])
    return bytes(rec)


def build_model_blob(model: Model,
                     vertices: Sequence[tuple[int, int, int]] | None = None,
                     polys: Sequence[Poly] | None = None) -> bytes:
    """Rebuild a MODEL from vertices/polys while preserving its non-geometry fields.

    The ped archive's models are FT3/FT4 only. A model with any other poly kind
    is refused here; the builder copies untouched models verbatim instead.
    """
    verts = list(vertices if vertices is not None else model.vertices)
    plist = list(polys if polys is not None else model.polys)
    if not plist:
        raise JMPPEDError("model has no polygons")
    for p in plist:
        if not p.supported_for_rebuild:
            raise JMPPEDError(
                "model uses unsupported poly kind %s; only FT3/FT4 are editable"
                % p.name)
        if any(i < 0 or i >= len(verts) for i in p.v):
            raise JMPPEDError("poly references a vertex outside the model")

    stream = b"".join(_encode_poly(p) for p in plist)
    nv = len(verts)
    npol = len(plist)
    vofs = MODEL_HEADER_SIZE
    nofs = vofs + nv * 8
    pnoffs = nofs
    poly_block = pnoffs
    bound = 0
    if verts:
        bound = max(max(abs(x), abs(y), abs(z)) for x, y, z in verts) + 1

    hdr = struct.pack(
        MODEL_HEADER_FMT,
        model.shape_flags, model.flags2, model.instance_number,
        model.tri_verts, model.z_bias, bound,
        0, nv, npol,
        vofs, poly_block, nofs, pnoffs,
        0)
    out = bytearray(hdr)
    for x, y, z in verts:
        out += struct.pack("<hhhh", x, y, z, 0)
    out += stream
    if model.collision:
        collision_offset = len(out)
        out += model.collision
        struct.pack_into("<i", out, 0 + 32, collision_offset)
    return bytes(out)


@dataclass
class Page:
    set: int
    npalettes: int
    clut: bytes
    image: bytes
    flags: int
    raw_blob: bytes = b""

    @property
    def compressed(self) -> bool:
        return bool(self.flags & 1)


@dataclass
class PedTexture:
    texnum: int
    x: int
    y: int
    w: int
    h: int


@dataclass
class PedPalette:
    texnum: int
    slot: int
    row: bytes


@dataclass
class Entry:
    name: str
    models: list[Model]
    pages: list[Page]
    raw_models: list[bytes] = field(default_factory=list)
    raw_pages: list[bytes] = field(default_factory=list)


@dataclass
class Archive:
    version: int
    model_names: list[str]
    models: list[Model]
    pages: list[Page]
    ped_model_index: int | None = None
    ped_pages: list[Page] = field(default_factory=list)
    ped_textures: list[PedTexture] = field(default_factory=list)
    ped_palettes: list[PedPalette] = field(default_factory=list)
    source_hash: str = ""
    raw_models: list[bytes] = field(default_factory=list)
    raw_pages: list[bytes] = field(default_factory=list)
    entries: list[Entry] = field(default_factory=list)

    @property
    def is_v4(self) -> bool:
        return self.version == VERSION_V4

    @property
    def is_v5(self) -> bool:
        return self.version == VERSION_V5


def read_archive(path: str) -> Archive:
    data = open(path, "rb").read()
    return parse_archive(data)


def parse_archive(data: bytes) -> Archive:
    if len(data) < 16 or data[:8] != MAGIC:
        raise JMPPEDError("not a JMPPED archive")
    version, count = struct.unpack_from("<II", data, 8)
    if version == VERSION_V5:
        return _parse_v5(data)
    if version not in (VERSION_V3, VERSION_V4):
        raise JMPPEDError("unsupported JMPPED version %d" % version)
    expected = 14 if version == VERSION_V3 else 15
    names = MODEL_NAMES if version == VERSION_V3 else V4_MODEL_NAMES
    if count != expected:
        raise JMPPEDError("version %d expects %d models, archive has %d"
                         % (version, expected, count))

    off = 16
    raw_models: list[bytes] = []
    models: list[Model] = []
    for _ in range(count):
        size = _u32(data, off)
        off += 4
        if off + size > len(data):
            raise JMPPEDError("truncated model blob at offset %d" % off)
        blob = data[off:off + size]
        off += size
        raw_models.append(blob)
        models.append(parse_model(blob))

    page_count = _u32(data, off)
    off += 4
    pages: list[Page] = []
    raw_pages: list[bytes] = []
    for _ in range(page_count):
        start = off
        setid, npal, clut_len, image_len, flags = struct.unpack_from("<IIIII", data, off)
        off += 20
        if off + clut_len + image_len > len(data):
            raise JMPPEDError("truncated page record at offset %d" % off)
        clut = data[off:off + clut_len]
        off += clut_len
        image = data[off:off + image_len]
        off += image_len
        pages.append(Page(setid, npal, clut, image, flags,
                          raw_blob=data[start:off]))
        raw_pages.append(data[start:off])

    arch = Archive(version=version, model_names=names, models=models,
                   pages=pages, raw_models=raw_models, raw_pages=raw_pages,
                   source_hash=sha256_bytes(data))
    arch.entries = [Entry("Jericho", models, pages, raw_models, raw_pages)]

    if version == VERSION_V4:
        arch.ped_model_index = _u32(data, off)
        off += 4
        ped_page_count = _u32(data, off)
        off += 4
        for _ in range(ped_page_count):
            setid, npal, clut_len, image_len, flags = struct.unpack_from("<IIIII", data, off)
            off += 20
            if off + clut_len + image_len > len(data):
                raise JMPPEDError("truncated ped page record at offset %d" % off)
            clut = data[off:off + clut_len]
            off += clut_len
            image = data[off:off + image_len]
            off += image_len
            arch.ped_pages.append(Page(setid, npal, clut, image, flags))
        tex_count = _u32(data, off)
        off += 4
        for _ in range(tex_count):
            texnum, x, y, w, h = struct.unpack_from("<IIIII", data, off)
            off += 20
            arch.ped_textures.append(PedTexture(texnum, x, y, w, h))
        pal_count = _u32(data, off)
        off += 4
        for _ in range(pal_count):
            texnum, slot = struct.unpack_from("<II", data, off)
            off += 8
            row = data[off:off + CLUT_ROW_BYTES]
            if len(row) != CLUT_ROW_BYTES:
                raise JMPPEDError("truncated ped palette row at offset %d" % off)
            off += CLUT_ROW_BYTES
            arch.ped_palettes.append(PedPalette(texnum, slot, row))

    if off != len(data):
        raise JMPPEDError("%d trailing byte(s) after the archive" % (len(data) - off))
    return arch


def _parse_v5(data: bytes) -> Archive:
    version, entry_count = struct.unpack_from("<II", data, 8)

    if version != VERSION_V5:
        raise JMPPEDError("not a v5 archive")
    if entry_count < 1 or entry_count > JMPPED_MAX_ENTRIES:
        raise JMPPEDError("v5 archive has %d entries (max %d)"
                          % (entry_count, JMPPED_MAX_ENTRIES))

    off = 16
    entries: list[Entry] = []
    seen_sets: dict[int, str] = {}

    for _ in range(entry_count):
        if off + 4 > len(data):
            raise JMPPEDError("truncated v5 entry header")
        name_len = _u32(data, off)
        off += 4
        if name_len == 0 or name_len > ENTRY_NAME_MAX or off + name_len > len(data):
            raise JMPPEDError("bad v5 entry name length %d" % name_len)
        name = data[off:off + name_len].decode("utf-8")
        off += name_len
        if any(e.name == name for e in entries):
            raise JMPPEDError("duplicate v5 entry name %r" % name)

        model_count = _u32(data, off)
        off += 4
        if model_count != len(MODEL_NAMES):
            raise JMPPEDError("entry %r has %d models; expected %d"
                              % (name, model_count, len(MODEL_NAMES)))

        raw_models: list[bytes] = []
        models: list[Model] = []
        for _slot in range(model_count):
            if off + 4 > len(data):
                raise JMPPEDError("truncated model size in entry %r" % name)
            size = _u32(data, off)
            off += 4
            if size < MODEL_HEADER_SIZE or off + size > len(data):
                raise JMPPEDError("entry %r has a short model blob" % name)
            blob = data[off:off + size]
            off += size
            raw_models.append(blob)
            models.append(parse_model(blob))

        page_count = _u32(data, off)
        off += 4
        if page_count < 1 or page_count > 64:
            raise JMPPEDError("entry %r has %d pages" % (name, page_count))

        pages: list[Page] = []
        raw_pages: list[bytes] = []
        for _page in range(page_count):
            start = off
            if off + 20 > len(data):
                raise JMPPEDError("truncated page header in entry %r" % name)
            setid, npal, clut_len, image_len, flags = struct.unpack_from("<IIIII", data, off)
            off += 20
            if off + clut_len + image_len > len(data):
                raise JMPPEDError("truncated page record in entry %r" % name)
            clut = data[off:off + clut_len]
            off += clut_len
            image = data[off:off + image_len]
            off += image_len
            if setid in seen_sets:
                raise JMPPEDError("page set %d is used by both %r and %r"
                                  % (setid, seen_sets[setid], name))
            seen_sets[setid] = name
            pages.append(Page(setid, npal, clut, image, flags,
                              raw_blob=data[start:off]))
            raw_pages.append(data[start:off])

        entries.append(Entry(name, models, pages, raw_models, raw_pages))

    if off != len(data):
        raise JMPPEDError("%d trailing byte(s) after the v5 archive" % (len(data) - off))

    first = entries[0]
    return Archive(version=VERSION_V5, model_names=MODEL_NAMES,
                   models=first.models, pages=first.pages,
                   raw_models=first.raw_models, raw_pages=first.raw_pages,
                   entries=entries, source_hash=sha256_bytes(data))


def archive_bytes(arch: Archive) -> bytes:
    """Serialize an archive. Untouched model/page blobs are emitted verbatim."""
    if arch.version == VERSION_V5:
        entries = arch.entries
        if not entries:
            entries = [Entry("Jericho", arch.models, arch.pages,
                             arch.raw_models, arch.raw_pages)]
        return _archive_v5_bytes(entries)

    out = bytearray(MAGIC)
    out += struct.pack("<II", arch.version, len(arch.models))
    for i, m in enumerate(arch.models):
        raw = arch.raw_models[i] if i < len(arch.raw_models) else m.blob
        out += struct.pack("<I", len(raw))
        out += raw
    out += struct.pack("<I", len(arch.pages))
    for i, p in enumerate(arch.pages):
        raw = arch.raw_pages[i] if i < len(arch.raw_pages) else None
        if raw:
            out += raw
        else:
            out += _page_record(p)
    if arch.version == VERSION_V4:
        out += struct.pack("<I", arch.ped_model_index if arch.ped_model_index is not None else 14)
        out += struct.pack("<I", len(arch.ped_pages))
        for p in arch.ped_pages:
            out += _page_record(p)
        out += struct.pack("<I", len(arch.ped_textures))
        for t in arch.ped_textures:
            out += struct.pack("<IIIII", t.texnum, t.x, t.y, t.w, t.h)
        out += struct.pack("<I", len(arch.ped_palettes))
        for p in arch.ped_palettes:
            out += struct.pack("<II", p.texnum, p.slot)
            out += p.row
    return bytes(out)


def _archive_v5_bytes(entries: list[Entry]) -> bytes:
    if len(entries) < 1 or len(entries) > JMPPED_MAX_ENTRIES:
        raise JMPPEDError("v5 archive has %d entries (max %d)"
                          % (len(entries), JMPPED_MAX_ENTRIES))

    names: set[str] = set()
    page_sets: set[int] = set()
    out = bytearray(MAGIC)
    out += struct.pack("<II", VERSION_V5, len(entries))

    for entry in entries:
        name = entry.name.encode("utf-8")
        if not name or len(name) > ENTRY_NAME_MAX:
            raise JMPPEDError("entry name %r is not 1..%d UTF-8 bytes"
                              % (entry.name, ENTRY_NAME_MAX))
        if entry.name in names:
            raise JMPPEDError("duplicate entry name %r" % entry.name)
        names.add(entry.name)

        if len(entry.models) != len(MODEL_NAMES):
            raise JMPPEDError("entry %r has %d models; expected %d"
                              % (entry.name, len(entry.models), len(MODEL_NAMES)))

        out += struct.pack("<I", len(name))
        out += name
        out += struct.pack("<I", len(entry.models))
        for i, m in enumerate(entry.models):
            raw = entry.raw_models[i] if i < len(entry.raw_models) else m.blob
            out += struct.pack("<I", len(raw))
            out += raw

        out += struct.pack("<I", len(entry.pages))
        for i, p in enumerate(entry.pages):
            if p.set in page_sets:
                raise JMPPEDError("duplicate page set %d in entry %r"
                                  % (p.set, entry.name))
            page_sets.add(p.set)
            raw = entry.raw_pages[i] if i < len(entry.raw_pages) else None
            out += raw if raw else _page_record(p)

    return bytes(out)


def _page_record(page: Page) -> bytes:
    return (struct.pack("<IIIII", page.set, page.npalettes, len(page.clut),
                        len(page.image), page.flags) + page.clut + page.image)


def write_archive(path: str, arch: Archive) -> bytes:
    data = archive_bytes(arch)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return data


# --------------------------------------------------------------------------
# Pages, CLUTs and the 4bpp index map
# --------------------------------------------------------------------------

def cluts_from_bytes(clut: bytes, npalettes: int) -> list[bytes]:
    if npalettes < 0 or npalettes > 128:
        raise JMPPEDError("bad CLUT count %d" % npalettes)
    rows = []
    for i in range(npalettes):
        o = (i // 4) * CLUT_GROUP_BYTES + (i % 4) * CLUT_ROW_BYTES
        row = clut[o:o + CLUT_ROW_BYTES]
        rows.append(row + b"\x00" * (CLUT_ROW_BYTES - len(row)))
    return rows


def cluts_to_bytes(rows: Sequence[bytes]) -> bytes:
    if len(rows) > 128:
        raise JMPPEDError("more than 128 CLUT rows")
    groups = (len(rows) + 3) // 4
    out = bytearray(groups * CLUT_GROUP_BYTES)
    for i, row in enumerate(rows):
        if len(row) != CLUT_ROW_BYTES:
            raise JMPPEDError("CLUT row %d is %d bytes, expected %d"
                              % (i, len(row), CLUT_ROW_BYTES))
        o = (i // 4) * CLUT_GROUP_BYTES + (i % 4) * CLUT_ROW_BYTES
        out[o:o + CLUT_ROW_BYTES] = row
    return bytes(out)


def clut_rgb(row: bytes) -> list[tuple[int, int, int]]:
    if len(row) < CLUT_ROW_BYTES:
        raise JMPPEDError("short CLUT row")
    out = []
    for i in range(CLUT_ROW_ENTRIES):
        v = struct.unpack_from("<H", row, i * 2)[0]
        out.append((((v >> 0) & 0x1F) << 3,
                    ((v >> 5) & 0x1F) << 3,
                    ((v >> 10) & 0x1F) << 3))
    return out


def rgb_to_clut(colors: Sequence[tuple[int, int, int]]) -> bytes:
    out = bytearray(CLUT_ROW_BYTES)
    for i, (r, g, b) in enumerate(colors[:CLUT_ROW_ENTRIES]):
        v = ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10)
        struct.pack_into("<H", out, i * 2, v)
    return bytes(out)


def decomp_asm(src: bytes, out_bytes: int = PAGE_INDEX_BYTES) -> bytes:
    """Mirror of the engine's decomp_asm (first decoded byte at the end)."""
    dest = bytearray(out_bytes)
    ptr = out_bytes - 1
    i = 0
    n = len(src)
    while ptr >= 0 and i < n:
        pix = src[i]
        i += 1
        if pix & 0x80:
            if i >= n:
                break
            p = src[i]
            i += 1
            for _ in range(258 - pix):
                if ptr < 0:
                    break
                dest[ptr] = p
                ptr -= 1
        else:
            for _ in range(pix + 1):
                if ptr < 0 or i >= n:
                    break
                dest[ptr] = src[i]
                i += 1
                ptr -= 1
    return bytes(dest)


def encode_literal(data: bytes) -> bytes:
    """Literal-only RLE stream; always valid input for decomp_asm."""
    out = bytearray()
    for o in range(0, len(data), 128):
        chunk = data[o:o + 128]
        out.append(len(chunk) - 1)
        out += chunk
    return bytes(out)


def page_indices(page: Page) -> bytes:
    """Decode a page to 256x256 4-bit indices (one byte per texel, 0..15)."""
    if page.flags & PAGE_FLAG_COMPRESSED:
        if len(page.image) < 4:
            raise JMPPEDError("short compressed page image")
        ncluts = struct.unpack_from("<i", page.image, 0)[0]
        if ncluts < 0 or ncluts > 128:
            raise JMPPEDError("compressed page has %d CLUTs" % ncluts)
        start = 4 + ncluts * CLUT_ROW_BYTES
        raw = decomp_asm(page.image[start:])
    else:
        raw = page.image
    if len(raw) < PAGE_INDEX_BYTES:
        raise JMPPEDError("page image is %d bytes, expected %d"
                          % (len(raw), PAGE_INDEX_BYTES))
    idx = bytearray(PAGE_BYTES)
    for y in range(PAGE_H):
        row = raw[y * (PAGE_W // 2):(y + 1) * (PAGE_W // 2)]
        for xb, b in enumerate(row):
            idx[y * PAGE_W + xb * 2] = b & 0x0F
            idx[y * PAGE_W + xb * 2 + 1] = (b >> 4) & 0x0F
    return bytes(idx)


def encode_page_indices(indices: bytes) -> bytes:
    if len(indices) != PAGE_BYTES:
        raise JMPPEDError("index map is %d bytes, expected %d"
                          % (len(indices), PAGE_BYTES))
    raw = bytearray(PAGE_INDEX_BYTES)
    for y in range(PAGE_H):
        for xb in range(PAGE_W // 2):
            lo = indices[y * PAGE_W + xb * 2] & 0x0F
            hi = indices[y * PAGE_W + xb * 2 + 1] & 0x0F
            raw[y * (PAGE_W // 2) + xb] = lo | (hi << 4)
    return bytes(raw)


def page_cluts(page: Page) -> list[bytes]:
    if page.flags & PAGE_FLAG_COMPRESSED:
        if len(page.image) < 4:
            raise JMPPEDError("short compressed page image")
        ncluts = struct.unpack_from("<i", page.image, 0)[0]
        return [page.image[4 + i * CLUT_ROW_BYTES:4 + (i + 1) * CLUT_ROW_BYTES]
                for i in range(ncluts)]
    return cluts_from_bytes(page.clut, page.npalettes)


def make_raw_page(page_set: int, indices: bytes, cluts: Sequence[bytes]) -> Page:
    return Page(set=page_set, npalettes=len(cluts), clut=cluts_to_bytes(cluts),
                image=encode_page_indices(indices), flags=PAGE_FLAG_RAW)


def page_rgb(indices: bytes, clut: Sequence[tuple[int, int, int]]) -> list[tuple[int, int, int]]:
    return [clut[i & 0x0F] if i < len(clut) else (0, 0, 0) for i in indices]


def nearest_clut_index(rgb: tuple[int, int, int],
                       clut: Sequence[tuple[int, int, int]]) -> int:
    """Nearest 4-bit CLUT entry, exact matches first.

    A page is 4bpp: every edited RGB pixel must become one index 0..15. Using a
    nearest-colour match keeps a painted PNG usable without requiring the editor
    to preserve an indexed palette.
    """
    best = 0
    bestd = None
    r, g, b = rgb
    for i, (cr, cg, cb) in enumerate(clut[:CLUT_ROW_ENTRIES]):
        d = (r - cr) ** 2 + (g - cg) ** 2 + (b - cb) ** 2
        if bestd is None or d < bestd:
            best, bestd = i, d
            if d == 0:
                break
    return best


def save_rgb_png(path: str, width: int, height: int,
                 pixels: Sequence[tuple[int, int, int]]) -> None:
    from PIL import Image
    if len(pixels) != width * height:
        raise JMPPEDError("PNG %s has %d pixels, expected %d"
                          % (path, len(pixels), width * height))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    Image.frombytes("RGB", (width, height),
                    bytes(c for rgb in pixels for c in rgb)).save(path)


def save_indexed_png(path: str, width: int, height: int,
                     indices: Sequence[int],
                     clut: Sequence[tuple[int, int, int]]) -> None:
    from PIL import Image
    if len(indices) != width * height:
        raise JMPPEDError("indexed PNG %s has %d indices, expected %d"
                          % (path, len(indices), width * height))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    img = Image.frombytes("P", (width, height), bytes(i & 0x0F for i in indices))
    palette = []
    for i in range(CLUT_ROW_ENTRIES):
        palette.extend(clut[i] if i < len(clut) else (0, 0, 0))
    palette.extend([0, 0, 0] * (256 - CLUT_ROW_ENTRIES))
    img.putpalette(palette)
    img.save(path)


def load_png_rgb(path: str) -> tuple[int, int, list[tuple[int, int, int]]]:
    from PIL import Image
    img = Image.open(path).convert("RGB")
    return img.width, img.height, list(img.getdata())


def load_png_indices(path: str) -> tuple[int, int, bytes]:
    from PIL import Image
    img = Image.open(path)
    img.load()
    if img.mode not in ("P", "L"):
        raise JMPPEDError("%s is %s, not an indexed PNG; export the indexed "
                          "page or paint an RGB page" % (path, img.mode))
    return img.width, img.height, bytes(img.getdata())



# --------------------------------------------------------------------------
# Wavefront OBJ helpers
# --------------------------------------------------------------------------

def write_obj(path: str, model: Model, *,
              blender: bool = False, name: str | None = None,
              offset: tuple[float, float, float] = (0.0, 0.0, 0.0)) -> None:
    """Write one part (or the assembled scene) as a Wavefront OBJ.

    Raw mode writes engine units unchanged. Blender mode converts Y-up engine
    space to Blender Z-up, scales by BLENDER_SCALE, and keeps the same handedness.
    """
    name = name or os.path.splitext(os.path.basename(path))[0]
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# JMPPED part: %s\n" % name)
        f.write("o %s\n" % name)
        for x, y, z in model.vertices:
            px, py, pz = x + offset[0], y + offset[1], z + offset[2]
            if blender:
                f.write("v %.6f %.6f %.6f\n" % (px * BLENDER_SCALE, pz * BLENDER_SCALE, -py * BLENDER_SCALE))
            else:
                f.write("v %d %d %d\n" % (round(px), round(py), round(pz)))
        # One vt per face corner keeps every original UV exact.
        uv_index = []
        for p in model.polys:
            ki = []
            for u, v in p.uv:
                ki.append(len(uv_index) + 1)
                uv_index.append((u, v))
            p._obj_uv = ki
        for i, (u, v) in enumerate(uv_index, 1):
            f.write("vt %.6f %.6f\n" % (u / 255.0, 1.0 - v / 255.0))
        for p in model.polys:
            if p.textured:
                f.write("usemtl s%d_t%d\n" % (p.set, p.id))
            else:
                f.write("usemtl color_%d_%d_%d\n" % p.color)
            # face vertices are 1-based; vt indices follow this face's corners
            f.write("f " + " ".join("%d/%d" % (v + 1, p._obj_uv[j])
                                    for j, v in enumerate(p.v)) + "\n")


def write_assembled_obj(path: str, models: Sequence[Model], names: Sequence[str],
                        offsets: dict[int, tuple[float, float, float]],
                        *, blender: bool = False) -> None:
    """Write all posed parts into one scene, each as its own OBJ object.

    The assembled scene is for looking and posing. The per-slot OBJs are the
    edit input; a face from this file is not split back into a slot by the
    builder.
    """
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    vert_base = 0
    uv_base = 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# JMPPED assembled pose (visual/edit reference)\n")
        f.write("# Per-slot OBJs, not this scene, are the importer's input.\n")
        for i, m in enumerate(models):
            if i not in offsets:
                continue
            f.write("o %s\n" % (names[i] if i < len(names) else "part_%d" % i))
            ox, oy, oz = offsets[i]
            for x, y, z in m.vertices:
                px, py, pz = x + ox, y + oy, z + oz
                if blender:
                    f.write("v %.6f %.6f %.6f\n" %
                            (px * BLENDER_SCALE, pz * BLENDER_SCALE, -py * BLENDER_SCALE))
                else:
                    f.write("v %d %d %d\n" % (round(px), round(py), round(pz)))
            uvs = []
            for p in m.polys:
                uvs.extend(p.uv)
            for u, v in uvs:
                f.write("vt %.6f %.6f\n" % (u / 255.0, 1.0 - v / 255.0))
            k = 0
            for p in m.polys:
                if p.textured:
                    f.write("usemtl s%d_t%d\n" % (p.set, p.id))
                else:
                    f.write("usemtl color_%d_%d_%d\n" % p.color)
                f.write("f " + " ".join("%d/%d" % (vert_base + v + 1, uv_base + k + j + 1)
                                        for j, v in enumerate(p.v)) + "\n")
                k += len(p.uv)
            vert_base += len(m.vertices)
            uv_base += len(uvs)



def read_obj(path: str, *, blender: bool = False) -> tuple[list[tuple[float, float, float]], list[list[tuple[int, tuple[int, int] | None, int | None]]]]:
    """Return vertices and faces: (v, vt, material) per corner.

    The importer resolves material names back to (texture_set, texture_id) or a
    flat colour. It is deliberately independent of the assembled scene: the
    per-slot OBJs are the edit input.
    """
    verts: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    faces: list[list[tuple[int, tuple[int, int] | None, int | None]]] = []
    material = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if parts[0] == "v" and len(parts) >= 4:
                x, y, z = (float(parts[1]), float(parts[2]), float(parts[3]))
                if blender:
                    x, y, z = x / BLENDER_SCALE, -z / BLENDER_SCALE, y / BLENDER_SCALE
                verts.append((x, y, z))
            elif parts[0] == "vt" and len(parts) >= 3:
                uvs.append((float(parts[1]), float(parts[2])))
            elif parts[0] == "usemtl" and len(parts) >= 2:
                material = parts[1]
            elif parts[0] == "f" and len(parts) >= 4:
                face = []
                for tok in parts[1:]:
                    bits = tok.split("/")
                    vi = int(bits[0]) - 1
                    ti = int(bits[1]) - 1 if len(bits) > 1 and bits[1] else None
                    face.append((vi, uvs[ti] if ti is not None and 0 <= ti < len(uvs) else None,
                                 material))
                faces.append(face)
    return verts, faces


# --------------------------------------------------------------------------
# Assembled-scene part placement (mirrors preview_jmpped.py's rough pose)
# --------------------------------------------------------------------------

def _size(models: Sequence[Model], i: int) -> tuple[float, float, float]:
    if i >= len(models) or not models[i].vertices:
        return (1.0, 1.0, 1.0)
    vs = models[i].vertices
    return (max(v[0] for v in vs) - min(v[0] for v in vs),
            max(v[1] for v in vs) - min(v[1] for v in vs),
            max(v[2] for v in vs) - min(v[2] for v in vs))


def part_offsets(models: Sequence[Model]) -> dict[int, tuple[float, float, float]]:
    torso = _size(models, 0)
    head = _size(models, 1)
    uw, uh, _ = _size(models, 2)
    lw, lh, _ = _size(models, 3)
    tw, th, _ = _size(models, 6)
    cw, ch, _ = _size(models, 7)
    fw, fh, _ = _size(models, 8)
    _hw, hh, _ = _size(models, 12)
    _h2w, h2h, _ = _size(models, 13)
    x_arm = torso[0] * 0.55 + uw * 0.5
    # Engine +Y is DOWN. A positive offset moves a part down the body; the head
    # is above the torso centre and the legs/hands hang below it.
    y_arm = torso[1] * 0.18
    x_leg = torso[0] * 0.22
    lower_y = y_arm + uh * 0.5 + lh * 0.5
    return {
        0: (0.0, 0.0, 0.0),
        1: (0.0, -(torso[1] * 0.5 + head[1] * 0.5), 0.0),
        2: (-x_arm, y_arm, 0.0),
        3: (-x_arm, lower_y, 0.0),
        4: (x_arm, y_arm, 0.0),
        5: (x_arm, lower_y, 0.0),
        6: (-x_leg, torso[1] * 0.5 + th * 0.5, 0.0),
        7: (-x_leg, torso[1] * 0.5 + th + ch * 0.5, 0.0),
        8: (-x_leg, torso[1] * 0.5 + th + ch + fh * 0.5, 0.0),
        9: (x_leg, torso[1] * 0.5 + th * 0.5, 0.0),
        10: (x_leg, torso[1] * 0.5 + th + cw * 0.5, 0.0),
        11: (x_leg, torso[1] * 0.5 + th + cw + fh * 0.5, 0.0),
        12: (-x_arm, lower_y + lh * 0.5 + hh * 0.5, 0.0),
        13: (x_arm, lower_y + lh * 0.5 + h2h * 0.5, 0.0),
    }


# --------------------------------------------------------------------------
# Manifest helpers
# --------------------------------------------------------------------------

MANIFEST_VERSION = 1


def sha256_bytes_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_json(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save_json(path: str, data: dict) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1)
        f.write("\n")


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def _selftest() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "JMPPED.LEV")
    if not os.path.isfile(path):
        print("selftest: %s not found" % path)
        return 2
    arch = read_archive(path)
    print("selftest: v%d, %d models, %d pages" %
          (arch.version, len(arch.models), len(arch.pages)))
    rebuilt = archive_bytes(arch)
    original = open(path, "rb").read()
    if rebuilt != original:
        print("selftest: FAIL archive re-serialize is not byte-identical")
        return 1
    for i, m in enumerate(arch.models):
        if m.unsupported_polys:
            print("selftest: model %d has %d unsupported poly(s)" %
                  (i, m.unsupported_polys))
    for p in arch.pages:
        idx = page_indices(p)
        cluts = page_cluts(p)
        print("selftest: page set %d %s %d CLUTs, %d indices" %
              (p.set, "compressed" if p.compressed else "raw", len(cluts), len(idx)))
    print("selftest: OK (byte-identical archive; models/pages parsed)")
    return 0


def _inspect(path: str) -> int:
    arch = read_archive(path)
    print("%s: v%d, %d models, %d pages%s" %
          (path, arch.version, len(arch.models), len(arch.pages),
           ", v4 ped pack" if arch.is_v4 else ""))
    for i, m in enumerate(arch.models):
        print("  %2d %-18s %4d verts %4d polys %s" %
              (i, arch.model_names[i], len(m.vertices), len(m.polys),
               "instance %d" % m.instance_number if m.instance_number >= 0 else ""))
    for p in arch.pages:
        print("  page set %3d %-10s %3d CLUTs %6d image bytes" %
              (p.set, "compressed" if p.compressed else "raw",
               len(page_cluts(p)), len(p.image)))
    if arch.is_v4:
        print("  ped model index %d, %d ped page(s), %d texture(s), %d palette row(s)" %
              (arch.ped_model_index, len(arch.ped_pages),
               len(arch.ped_textures), len(arch.ped_palettes)))
    rebuilt = archive_bytes(arch)
    original = open(path, "rb").read()
    print("  re-serialize: %s" % ("byte-identical" if rebuilt == original
                                  else "DIFFERS (%d vs %d bytes)" % (len(rebuilt), len(original))))
    return 0 if rebuilt == original else 1


if __name__ == "__main__":
    import sys
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        raise SystemExit(_selftest())
    if len(args) == 2 and args[0] == "--inspect":
        raise SystemExit(_inspect(args[1]))
    print("usage: jmpped_common.py --selftest | --inspect ARCHIVE")
    raise SystemExit(2)
