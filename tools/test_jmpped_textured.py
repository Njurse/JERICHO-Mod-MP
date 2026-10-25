#!/usr/bin/env python3
"""Textured-model conversion tests."""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import jmpped_core as jc
import jmpped_entries as je
import jmpped_models as jm
import jmpped_textured as jt


class HeadMappingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = je.load_manifest(HERE)
        cls.character = je.character(cls.manifest, "Jericho Driv3r")
        cls.page_abs = os.path.join(HERE, "characters", "jerichod3", "model.obj")
        cls.groups = jt._read_page_groups(HERE, cls.character, cls.page_abs)

    def test_material_names_round_trip(self):
        self.assertEqual(jt.part_material("JERI_HEAD", 27, 24),
                         "JERI_HEAD__s27_t24")
        self.assertEqual(jt.parse_material("JERI_HEAD__s27_t24"),
                         ("JERI_HEAD", 27, 24))
        self.assertEqual(jt.parse_material("JERI_HEAD__s27_t24.001"),
                         ("JERI_HEAD", 27, 24))
        self.assertIsNone(jt.parse_material("s27_t24"))

    def test_head_crop_round_trip(self):
        for texture, rect in ((24, [144, 120, 16, 24]),
                              (25, [128, 192, 16, 24])):
            crop = jt._crop(self.character, "JERI_HEAD", 27, texture)
            self.assertIsNotNone(crop)
            self.assertEqual(list(crop["rect"]), rect)
            for uv in self._head_uvs(texture):
                local = jt.page_to_crop(uv, rect)
                self.assertTrue(jt._inside(local), (texture, uv, local))
                back = jt.crop_to_page(local, rect)
                self.assertAlmostEqual(back[0], uv[0], places=6)
                self.assertAlmostEqual(back[1], uv[1], places=6)

    def _head_uvs(self, texture):
        material = "s25_t%d" % texture
        out = []
        for face in self.groups["JERI_HEAD"]["faces"]:
            if face[0][2] == material:
                out.extend(uv for _vi, uv, _mat in face)
        self.assertTrue(out, material)
        return out

    def test_shared_material_maps_to_part_crops(self):
        for part in ("JERI_TORSO", "JERI_U_ARM_LEFT", "JERI_U_ARM_RIGHT"):
            materials = {face[0][2] for face in self.groups[part]["faces"]}
            self.assertIn("s23_t9", materials)
            crop = jt._crop(self.character, part, 26, 9)
            self.assertIsNotNone(crop, part)
            self.assertIn("_set26_t9.png", crop["rgb"])


class TempExportTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="jmpped-textured-")
        self.addCleanup(shutil.rmtree, self.root, True)
        src = os.path.join(HERE, "characters", "jerichod3")
        dst = os.path.join(self.root, "characters", "test")
        shutil.copytree(src, dst)
        self.character = jc.load_json(os.path.join(dst, "character.json"))
        je.rewrite_paths(self.character, "characters/jerichod3",
                         "characters/test")
        jc.save_json(os.path.join(dst, "character.json"), self.character)
        jc.save_json(os.path.join(self.root, "project.json"), {
            "project_version": 2,
            "format_version": 5,
            "source": {"skeleton": {"json": "out/source/havana/skeleton/skeleton.json"}},
            "characters": [self.character],
        })
        self.page_abs = os.path.join(dst, "model.obj")
        self.textured = os.path.join(dst, "model_textured.obj")
        self.mtl = os.path.join(dst, "model_textured.mtl")
        self.assertEqual(jt.export_textured(self.root, self.character["name"]), 0)
        self.character = jc.load_json(os.path.join(dst, "character.json"))

    def test_textured_files_and_material_paths(self):
        self.assertTrue(os.path.isfile(self.textured))
        self.assertTrue(os.path.isfile(self.mtl))
        obj = open(self.textured, encoding="utf-8").read()
        mtl = open(self.mtl, encoding="utf-8").read()
        self.assertIn("o JERI_HEAD", obj)
        self.assertIn("usemtl JERI_HEAD__s27_t24", obj)
        self.assertIn("usemtl JERI_HEAD__s27_t25", obj)
        self.assertIn("usemtl JERI_TORSO__s26_t9", obj)
        self.assertIn("usemtl JERI_U_ARM_LEFT__s26_t9", obj)
        self.assertIn("usemtl JERI_U_ARM_RIGHT__s26_t9", obj)
        self.assertIn("newmtl JERI_HEAD__s27_t24", mtl)
        self.assertIn("map_Kd textures/crops/JERI_HEAD_set27_t24.png", mtl)
        self.assertIn("map_Kd textures/crops/JERI_HEAD_set27_t25.png", mtl)
        char_dir = os.path.join(self.root, "characters", "test")
        for line in mtl.splitlines():
            if line.startswith("map_Kd "):
                self.assertTrue(os.path.isfile(os.path.join(char_dir,
                                                            line[7:])),
                                line)

    def test_schema_and_check(self):
        model = self.character["model"]
        self.assertEqual(model["path"], "characters/test/model_textured.obj")
        self.assertEqual(model["page_path"], "characters/test/model.obj")
        self.assertEqual(model["uv_space"], "crop")
        self.assertEqual(model["format"], jt.FORMAT)
        self.assertTrue(model["sha256"])
        self.assertTrue(model["page_sha256"])
        self.assertEqual(jt.check_uvs(self.root, self.character["name"]), 0)

    def test_textured_round_trip_matches_page_model(self):
        page_groups = jt._read_page_groups(self.root, self.character,
                                           self.page_abs)
        groups = jt.read_textured(self.root, self.character)
        for part in ("JERI_HEAD", "JERI_TORSO", "THIGH_LEFT"):
            self.assertEqual(len(groups[part]["faces"]),
                             len(page_groups[part]["faces"]), part)
            page_uvs = sorted((round(uv[0], 4), round(uv[1], 4))
                              for face in page_groups[part]["faces"]
                              for _vi, uv, _mat in face)
            text_uvs = sorted((round(uv[0], 4), round(uv[1], 4))
                              for face in groups[part]["faces"]
                              for _vi, uv, _mat in face)
            self.assertEqual(text_uvs, page_uvs, part)

    def test_write_page_model_from_textured(self):
        groups = jt.read_textured(self.root, self.character)
        jt.write_page_model(self.root, self.character, groups)
        self.assertTrue(os.path.isfile(self.page_abs))
        page_groups = jm.read_combined_obj(
            self.page_abs,
            {p["part"] for p in self.character["parts"]} | {"HIPS"},
            blender=True)
        for part in ("JERI_HEAD", "JERI_TORSO"):
            self.assertEqual(len(page_groups[part]["faces"]),
                             len(groups[part]["faces"]), part)
        self.assertTrue(self.character["model"]["page_sha256"])

    def test_outside_uv_is_refused(self):
        text = open(self.textured, encoding="utf-8").read().splitlines()
        head = text.index("o JERI_HEAD")
        vt = next(i for i in range(head, len(text))
                  if text[i].startswith("vt "))
        text[vt] = "vt 1.500000 0.500000"
        bad = os.path.join(os.path.dirname(self.textured),
                           "model_textured_bad.obj")
        with open(bad, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(text) + "\n")
        self.character["model"]["path"] = "characters/test/model_textured_bad.obj"
        _groups, _stats, issues = jt.scan_textured(self.root, self.character)
        self.assertTrue(issues)
        with self.assertRaises(jc.JMPPEDError):
            jt.read_textured(self.root, self.character)


class CropPaintTests(unittest.TestCase):
    def test_painted_crop_reaches_page_and_clut(self):
        import jmpped_bake as jb
        from PIL import Image

        root = tempfile.mkdtemp(prefix="jmpped-crop-")
        self.addCleanup(shutil.rmtree, root, True)
        path = os.path.join(root, "crop.png")
        Image.new("RGB", (2, 2), (255, 0, 0)).save(path)
        crop = {"rect": [0, 0, 2, 2], "rgb": "crop.png", "rgb_sha256": "",
                "palette_row": 0, "texture": 0, "page_set": 1}
        state = {"indices": bytes(65536), "cluts": [bytes(32)] * 16,
                 "display": 0, "full_page_touched": False,
                 "touched": False, "cluts_touched": False,
                 "path": path, "set": 1}
        jb._apply_crop(state, crop, root, [0])
        self.assertTrue(state["touched"])
        self.assertTrue(state["cluts_touched"])
        self.assertNotEqual(state["cluts"][0], bytes(32))


if __name__ == "__main__":
    unittest.main()
