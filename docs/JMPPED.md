# JMPPED — how the pedestrian archive is put together

This is the canonical explanation of a `JMPPED.LEV` bake: where its models,
pages and skeleton come from, what the builder writes, and how the engine reads
it. The toolkit quick-start stays in
[`../tools/README.md`](../tools/README.md); this page is the full assembly map.

## 1. What a JMPPED is

`JMPPED.LEV` is a self-contained pedestrian archive loaded from
`DRIVER2/LEVELS/JMPPED.LEV` by `JerPedImportBaked()`. It is engine-owned: the
`mp` module has no asset dependency (`JERICHO/MODS/mp/mod.toml` has
`dependencies = []`) and does not ship its own copy. `JERICHO/build.py` copies
the checked-in `JERICHO/MODS/mp/tools/JMPPED.LEV` beside the built game.

Two archive versions matter:

| Version | Shape | Use |
| --- | --- | --- |
| **3** | magic `JMPPED\0\0`, version, 14 model blobs, page records | the original single 14-slot character; still read by the engine and used as `base.LEV` |
| **5** | magic, version, entry count, then per entry: name, 14 model blobs, page records | multi-entry archive; up to `JMPPED_MAX_ENTRIES` / `JER_PED_MAX_ENTRIES` = **8** entries |

Version 4 is a legacy read-only shape (the old civilian-head experiment). The
current toolkit reads it but writes v3 or v5 only.

The engine's character id mapping is:

- `0` — Tanner, the engine's resident models (`pmTannerModels`)
- `1` — archive entry 0 (the Jericho set)
- `2..` — archive entries 1.. in the order they appear in the v5 archive

The mp layer (`MpPedModelName/Selectable/Available/Effective`) asks
`JerPedEntryCount()`/`JerPedEntryName()` and never owns the asset.

## 2. Source extraction

The source is a **Driver 2 level**, not a previous `JMPPED.LEV`. By default it is
`src_rebuild/bin/Release/DRIVER2/LEVELS/HAVANA.LEV`:

```
jmpped_export.py
  └─ bake_jmpped.py --no-copy
       reads the level's model table, writes base.LEV (v3, 14 models, 3 pages)
  └─ writes tanner/, jericho/, skeleton/, jmpped/, jmpped.json, base_v5.LEV
```

`jmpped_source.py` reads the level lumps:

- `LUMP_MODELNAMES` (12) — the model-name table
- `LUMP_MODELS` — the model blobs
- `LUMP_TEXTUREINFO` (34) — texture-page records
- `LUMP_SPOOLINFO` (26) — which pages belong to the Havana model areas
- `LUMP_MOTIONCAPTURE` (22) — the shared motion block used for the bind skeleton

The source split is:

- **Tanner**: the full `TORSO, HEAD, U_ARM_*, L_ARM_*, HAND_*, THIGH_*, CALF_*,
  FOOT_*, NECK, HIPS, BAG` set, pages `{2}`.
- **Jericho**: `JERI_TORSO, JERI_HEAD, JERI_U_ARM_*, JERI_L_ARM_*`, pages
  `{23, 25}`; the playable v3 Jericho also uses Tanner's limbs and hands.

The motion block supplies the 23-bone hierarchy. `skeleton.json` stores both the
motion-space positions (`motion_raw`, +Y up) and the engine draw positions
(`engine`, +Y down), plus `variants.tanner.bindings` and
`variants.jericho.bindings`.

## 3. The 14-slot map

Every entry has exactly these 14 model slots. Slots 0–5 are the Jericho upper
body; slots 6–13 are Tanner's limbs and hands. The bind bone column is the
Jericho/`OTHER_MODEL` binding; the Tanner variant binds `LTOE`/`RTOE` to the
opposite foot names, which is why `skeleton.json` keeps both variants.

| Slot | Part | Source ped | Bind bone | Entry-0 page set |
| ---: | --- | --- | --- | ---: |
| 0 | `JERI_TORSO` | Jericho | `JOINT_1` | 23 |
| 1 | `JERI_HEAD` | Jericho | `HEAD` | 25 |
| 2 | `JERI_U_ARM_LEFT` | Jericho | `LELBOW` | 23 |
| 3 | `JERI_L_ARM_LEFT` | Jericho | `LHAND` | 23 |
| 4 | `JERI_U_ARM_RIGHT` | Jericho | `RELBOW` | 23 |
| 5 | `JERI_L_ARM_RIGHT` | Jericho | `RHAND` | 23 |
| 6 | `THIGH_LEFT` | Tanner | `LKNEE` | 2 |
| 7 | `CALF_LEFT` | Tanner | `LFOOT` | 2 |
| 8 | `FOOT_LEFT` | Tanner | `LTOE` | 2 |
| 9 | `THIGH_RIGHT` | Tanner | `RKNEE` | 2 |
| 10 | `CALF_RIGHT` | Tanner | `RFOOT` | 2 |
| 11 | `FOOT_RIGHT` | Tanner | `RTOE` | 2 |
| 12 | `HAND_LEFT` | Tanner | `LFINGERS` | 2 |
| 13 | `HAND_RIGHT` | Tanner | `RFINGERS` | 2 |

The full Tanner torso/head/arms/neck/hips are exported for reference but have
`bake_slot: null`; v3/v5 cannot store them as separate slots.

## 4. Workspace and manifests

The default workspace is `tools/out/jmppeds/v3` (gitignored):

```
base.LEV                 untouched v3 archive from the same level
base_v5.LEV              v5 seed with entry 0
jmpped.json              the manifest: pages, entries, slots, skeleton, hashes
skeleton/skeleton.json   23 bones, motion_raw/engine positions, variant bindings
skeleton/skeleton_*.obj  raw/edit bone-edge views
tanner/...               full Tanner reference subexport
jericho/...              entry 0 parts, pages 23/25 and bindpose
entries/<slug>/...       later v5 entries (add-entry output)
jmpped/assembled_*.obj   rough visual reference for the 14 slots
```

`jmpped.json` (manifest version 2) is the builder's input:

- `source_level`, `source_sha256` — the HAVANA level used for the export
- `base_archive`, `base_v5_archive` and their SHA-256 values
- `archive_version` — what the builder writes
- `peds` — the source subexports (`tanner`, `jericho`)
- `entries` — the v5 entries. Each has `index`, `name`, `directory`,
  `source_ped`, `page_sets`, `pages`, `slots`, `bindpose`, `palette_crops`
- `slots` — the 14 descriptors: `slot`, `ped`, `part`, `page_sets`,
  `obj_raw`/`obj_edit` and hashes, `offset`/`scale`, `materials`, `crops`
- `pages` — page set, storage (raw/compressed), `display_clut`, page/crop paths
  and hashes
- `skeleton` — the hierarchy and variant bindings
- `bake.slots` — the authoritative slot→ped/part map

Each entry also has a `ped.json` mirror for humans and per-entry tooling. Hashes
are the change detector: a part is rebuilt when its selected OBJ copy, its page,
a crop, or its `offset`/`scale` differs from the exported value. A transform is
bake-time and applied to the vertices; it is not an engine runtime parameter.

## 5. Assembly sequence

```sh
BAKE_JMPPED.bat export [--source LEVEL] [--out DIR]
# edit obj/edit or obj/raw, png pages/crops, bindpose_edit.obj, jmpped.json
BAKE_JMPPED.bat split [--variant jericho|tanner] [--and-build]
BAKE_JMPPED.bat build [--use auto|edit|raw] [--entry NAME] [--copy]
BAKE_JMPPED.bat preview [--frames N] [--size N]
BAKE_JMPPED.bat all
BAKE_JMPPED.bat add-entry --name "Display Name" [--from-entry Jericho]
```

1. **Export.** `jmpped_export.py` calls `bake_jmpped.py --no-copy`, writes
   `base.LEV`, the per-ped OBJs/PNGs, the skeleton, `base_v5.LEV` and
   `jmpped.json`.
2. **Edit.** Change one OBJ copy per part (`obj/edit` for Blender or `obj/raw`
   for engine units), one texture form per page (RGB or indexed), and/or a
   part's `offset`/`scale` in the JSON. Whole-character edits can be made in
   `bindpose_edit.obj` and split back.
3. **Split.** `jmpped_split.py` reads `bindpose_edit.obj`, subtracts each part's
   bone bind offset to return to model-local space, writes only changed groups
   to `obj/edit/<part>.obj`, records the combined-scene hash, and can run the
   builder.
4. **Build.** `jmpped_build.py` reads `base.LEV` (or `base_v5.LEV` through the
   v5 path), rebuilds changed model blobs and changed pages/crops, applies
   `v' = v*scale + offset`, writes `JMPPED.LEV`, and re-reads it as a self-check.
   A no-edit build is deterministic; untouched slots keep the base bytes.
5. **Copy.** `--copy` writes the archive to `DRIVER2/LEVELS`; a normal game
   build also runs `build.py`'s `copy_mod_assets()` for the same target.
6. **Preview/verify.** `preview_jmpped.py` renders a turntable GIF; the runtime
   checks use `MP_TEST_JERICHO` and a two-seat rig.

## 6. Pages and CLUTs

A page is 256×256 at 4bpp. Each page carries a CLUT table with one 16-entry row
per texture id; the engine selects `civ_clut[0][texture_number][pal]`. The page
therefore cannot be represented by one palette row for every material.

- `page_<set>_indexed.png` — the page's indexed source. It is internal/base
  data, not the normal edit surface.
- `png/palettes/page_<set>.png` — a strip of every CLUT row.
- `png/parts/<part>_set<set>_t<id>.png` — the **single editable RGB crop** for
  that material. There is no `_indexed` twin; the builder paletteizes it.
- Each crop records `display_clut` (its texture row). A crop with ≤16 colours is
  preserved exactly; more colours are median-cut to 16, then the row and page
  indices are written automatically.

Edit the per-material crop. Do not edit the indexed page or palette strip unless
you are repairing page-level data; the crop is the one image the baker expects.

Page sets are disjoint per entry:

| Entry | Torso/arms | Head | Legs/hands |
| ---: | ---: | ---: | ---: |
| 0 (Jericho) | 23 | 25 | 2 |
| 1 | 26 | 27 | 3 |
| 2 | 28 | 29 | 4 |
| 3 | 30 | 31 | 5 |
| 4 | 32 | 33 | 6 |
| 5 | 34 | 35 | 7 |
| 6 | 36 | 37 | 8 |
| 7 | 38 | 39 | 9 |

The allocation is `ENTRY_PAGE_SETS` in `jmpped_common.py`; `add-entry` remaps
every `s<set>_t<id>` material and crop to the new entry's sets.

## 7. Builder rules and refusals

The builder fails with the ped/part/source named rather than silently dropping
data:

- Only FT3/FT4 pedestrian polys are rebuilt; a model with normal tables or an
  unsupported poly kind is refused.
- Faces must be triangles or quads, and must keep a material named
  `s<set>_t<id>` (textured) or `color_R_G_B` (flat).
- The model must match the slot's base model name and the entry must claim all
  14 slots exactly once.
- A referenced page set must belong to that entry; unknown pages are refused.
- Both `obj/edit` and `obj/raw` changed is ambiguous and refused; similarly a
  full page and one of its crops together.
- `offset`/`scale` must be three finite numbers; a zero scale is refused.
- A transform-only change still rebuilds the part, even when both OBJ copies
  match their hashes.
- No-edit v3 output is byte-identical to `base.LEV`; the v5 writer preserves
  untouched entry 0 model bytes and rebuilds later entries to remap page sets.
- After writing, the builder re-reads the archive and checks version, entry
  count and 14-model shape before reporting success.

## 8. Engine load path

`JerPedImportBaked()` opens `DRIVER2\LEVELS\JMPPED.LEV` and accepts v3 or v5.
The v5 parser (`JerPedImportBakedV5`) validates:

- entry count 1..`JER_PED_MAX_ENTRIES` (8)
- each entry's name, exactly 14 model blobs, and up to 16 page records
- page and model bounds before allocating

It builds each entry's models into:

- `pmJerichoModels[entry][0..5]` — the six JERI parts
- `pmJerichoLegModels[entry][0..5]` — Tanner legs/feet
- `pmJerichoHandModels[entry][0..1]` — Tanner hands

and exposes `JerPedEntryCount()`, `JerPedEntryName(entry)` and
`JerPedModelForEntry(entry, slot)`. The selected entry is carried per
pedestrian in `PEDESTRIAN.pedEntry`; `SetSkelModelPointersForEntry(type, entry)`
binds `Skel[].pModel` to that entry's banks before the draw.

The v5 import uploads every entry's pages through
`JerPedUploadBakedPage()` and releases them with `JerPedReleasePins()` on
`JerPedImportReset()`/level change. The live-Havana fallback uses
`JerPedPinTpages()`/`JerPedPinsReady()` instead. `JerPedImportReady()` means the
bake/v5 models and pages are in place; the mp apply path waits for it.

`JERICHO/build.py` copies `tools/JMPPED.LEV` to
`src_rebuild/bin/<config>/DRIVER2/LEVELS/JMPPED.LEV` (and to a runtime JERICHO
beside the exe when present). A missing bake is not a build failure; the engine
falls back to Havana's streamed pages, and otherwise to Tanner.

## 9. Adding a new selectable entry

```sh
BAKE_JMPPED.bat add-entry --name "Jericho (Driv3r)"
# or: python jmpped_add_entry.py --name "Jericho (Driv3r)"
```

`add-entry`:

- copies the source entry's 14 slot OBJs, three pages, palette strips and
  bindpose into `entries/<slug>/`
- assigns the next entry page-set triplet from `ENTRY_PAGE_SETS`
- rewrites every `s<set>_t<id>` material and crop to those sets
- adds the entry to `jmpped.json` and writes `entries/<slug>/ped.json`

Then edit the copied 14 parts, textures and/or bindpose, set per-part
`offset`/`scale` where proportions need it, and run `build`. The v5 archive
carries the entry name; the engine sees it as `JerPedEntryCount()`/`Name()`, and
the mp picker exposes it as `MP_PED_MODEL_*` id `entry index + 1`.

### Registering a completed entry folder

If `entries/<slug>/` already contains `ped.json`, all 14 `obj/raw|edit` parts,
the three pages/palette strips and the bindpose:

```sh
python jmpped_register.py --folder entries/<slug>
# or drag the folder onto REGISTER_JMPPED.bat
```

It validates the 14 slots, the three page sets, the page/crop/bindpose files and
the entry index → page-set allocation, adds or replaces the entry in
`jmpped.json`, builds the v5 archive, and copies the bake to
`tools/JMPPED.LEV` plus the existing `Release`/`Release_dev` `DRIVER2/LEVELS`
directories. `--no-build` registers without baking; `--no-copy` leaves the bake
in the workspace. The folder must be inside the workspace; `add-entry` is the
way to allocate a brand-new entry and page-set triplet from an existing one.

Limits and rules:

- at most 8 entries; the ninth is refused
- every entry owns its own three page sets, so pages/CLUTs cannot alias
- every entry must cover all 14 slots; the slot names must match the base map
- the shared skeleton and Tanner bindings are reused; a new entry does not get
  its own skeleton

## 10. Troubleshooting

**Wrong colours.** Use the single per-material crop
(`png/parts/<part>_set<set>_t<id>.png`). It is auto-palettized into the texture
row recorded in `display_clut`; `palettes/page_<set>.png` shows the rows. The
page's indexed PNG is internal/base data, not the editing surface.

**Mashed torso/legs.** `JERI_TORSO` is much longer than Tanner's `TORSO` and
extends through the hips/thighs. Fix it with a per-part `offset`/`scale` on the
torso (or split the long coat), not by scaling the whole character. A uniform
height change still needs the skeleton bind offsets plus collision/camera/seat
offsets; the mesh transform alone does not move those.

**Missing entry or page.** Check `jmpped.json` `entries`/`slots`, the entry's
`page_sets`, and the archive version. The engine logs the entry count, model
count and page count it loaded; an entry id beyond `JerPedEntryCount()` falls
back to Tanner/Jericho in mp.

**v3 vs v5.** v3 is one 14-slot character and is still the `base.LEV` format.
v5 is required for more than one entry. The builder writes v5 when the manifest
has `entries`; the legacy v3 path remains for old workspaces.

## 11. Assembly map

| Stage | Command | Writes | Engine reads |
| --- | --- | --- | --- |
| source extraction | `BAKE_JMPPED export` | `base.LEV`, `base_v5.LEV`, `jmpped.json`, `ped.json`, OBJ/PNG | — |
| whole-character edit | `BAKE_JMPPED split` | `obj/edit/<part>.obj`, bindpose hash | — |
| per-part/texture edit | edit files + `jmpped.json` | OBJ/PNG/`offset`/`scale` | — |
| bake | `BAKE_JMPPED build` | `JMPPED.LEV` (v3/v5) | `JerPedImportBaked()` |
| runtime copy | `--copy` or `JERICHO/build.py` | `bin/<config>/DRIVER2/LEVELS/JMPPED.LEV` | engine load |
| preview | `BAKE_JMPPED preview` | `preview_360.gif` | — |

## 12. Verification

- `python JERICHO/MODS/mp/tools/jmpped_common.py --selftest` — archive parse/round-trip.
- `BAKE_JMPPED.bat all` — export, build and preview in one run.
- `python tools/doccheck.py` — links, docs index and counted claims.
- `python JERICHO/build.py game mod mp` — the module build.
- A two-seat Havana rig with `MP_TEST_PED_MODEL` — character sync/apply.
- `MP_TEST_JERICHO` — import the archive without a session.
- A missing `JMPPED.LEV` must leave both seats on Tanner with no crash.
