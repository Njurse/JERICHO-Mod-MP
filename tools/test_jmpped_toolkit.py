#!/usr/bin/env python3
"""Round-trip tests for the multi-entry JMPPED archive.

Run from the mp module root:
    python -m unittest discover -s tools -p 'test_jmpped_toolkit.py' -v
"""

from __future__ import annotations

import copy
import dataclasses
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import jmpped_common as jc
import jmpped_build as jb
import jmpped_entries as je

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, "JMPPED.LEV")


class ArchiveV5Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = jc.read_archive(BASE)
        cls.entry = cls.base.entries[0]

    def _archive(self, entries):
        return jc.Archive(version=jc.VERSION_V5,
                          model_names=jc.MODEL_NAMES,
                          models=entries[0].models,
                          pages=entries[0].pages,
                          raw_models=entries[0].raw_models,
                          raw_pages=entries[0].raw_pages,
                          entries=entries)

    def _second_entry(self, name="Driv3r"):
        sets = jc.entry_page_sets(1)
        pages = [jc.Page(sets[i], p.npalettes, p.clut, p.image, p.flags)
                 for i, p in enumerate(self.entry.pages)]
        return jc.Entry(name, list(self.entry.models), pages,
                        list(self.entry.raw_models))

    def test_v3_reads_as_jericho_entry(self):
        self.assertEqual(len(self.base.entries), 1)
        self.assertEqual(self.base.entries[0].name, "Jericho")
        self.assertEqual(len(self.base.entries[0].models), 14)
        self.assertEqual([p.set for p in self.base.entries[0].pages], [23, 25, 2])

    def test_v5_one_entry_round_trip(self):
        data = jc.archive_bytes(self._archive([self.entry]))
        again = jc.parse_archive(data)
        self.assertEqual(again.version, jc.VERSION_V5)
        self.assertEqual(len(again.entries), 1)
        self.assertEqual(again.entries[0].name, "Jericho")
        self.assertEqual(again.entries[0].models[0].blob,
                         self.base.entries[0].models[0].blob)
        self.assertEqual([p.set for p in again.entries[0].pages], [23, 25, 2])
        self.assertEqual(jc.archive_bytes(again), data)

    def test_v5_two_entries_preserve_untouched_entry(self):
        second = self._second_entry()
        data = jc.archive_bytes(self._archive([self.entry, second]))
        again = jc.parse_archive(data)

        self.assertEqual([e.name for e in again.entries], ["Jericho", "Driv3r"])
        self.assertEqual([p.set for p in again.entries[0].pages], [23, 25, 2])
        self.assertEqual([p.set for p in again.entries[1].pages], [26, 27, 3])
        for i in range(14):
            self.assertEqual(again.entries[0].models[i].blob,
                             self.base.entries[0].models[i].blob)
            self.assertEqual(again.entries[1].models[i].blob,
                             self.base.entries[0].models[i].blob)
        self.assertEqual(jc.archive_bytes(again), data)

    def test_duplicate_page_set_is_refused(self):
        second = jc.Entry("Driv3r", list(self.entry.models), list(self.entry.pages),
                          list(self.entry.raw_models))
        with self.assertRaises(jc.JMPPEDError):
            jc.archive_bytes(self._archive([self.entry, second]))

    def test_missing_slot_is_refused(self):
        short = jc.Entry("Short", list(self.entry.models[:-1]),
                         list(self.entry.pages), list(self.entry.raw_models[:-1]))
        with self.assertRaises(jc.JMPPEDError):
            jc.archive_bytes(self._archive([short]))


class ToolkitHelpers(unittest.TestCase):
    def test_slugify(self):
        self.assertEqual(je.slugify("Jericho (Driv3r)"), "jericho-driv3r")
        self.assertEqual(je.slugify("  My Ped  "), "my-ped")

    def test_material_map(self):
        src = {"name": "Jericho", "page_sets": [23, 25, 2]}
        self.assertEqual(je.material_map(src, (26, 27, 3)),
                         {23: 26, 25: 27, 2: 3})

    def test_remap_materials(self):
        text = "usemtl s23_t9\nusemtl s2_t12\n"
        self.assertEqual(je.remap_materials(text, {23: 26, 25: 27, 2: 3}),
                         "usemtl s26_t9\nusemtl s3_t12\n")


class RefusalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = jc.read_archive(BASE)
        cls.entry = cls.base.entries[0]

    def _entry_dict(self):
        return {
            "name": self.entry.name,
            "directory": "test-entry",
            "page_sets": [p.set for p in self.entry.pages],
            "pages": [{"set": p.set} for p in self.entry.pages],
            "slots": [{"slot": i} for i in range(14)],
            "bindpose": {"edit": "bindpose.obj"},
        }

    def test_missing_slot_is_refused_by_entry_validation(self):
        e = self._entry_dict()
        e["slots"] = e["slots"][:-1]
        with self.assertRaises(jc.JMPPEDError):
            je.validate_entry({}, e)

    def test_duplicate_slot_is_refused_by_entry_validation(self):
        e = self._entry_dict()
        e["slots"].append(copy.deepcopy(e["slots"][0]))
        with self.assertRaises(jc.JMPPEDError):
            je.validate_entry({}, e)

    def test_face_over_four_corners_is_refused(self):
        face = [(0, (0.0, 0.0), "s23_t1")] * 5
        with self.assertRaises(jc.JMPPEDError):
            jb._polys_from_obj([face], "five.obj")

    def test_model_with_non_ft_poly_is_refused(self):
        model = copy.deepcopy(self.base.models[0])
        model.normals = []
        model.point_normals = []
        model.polys = [jc.Poly(kind=jc.POLY_GT4, set=23, id=1,
                               v=[0, 1, 2, 3], uv=[(0, 0)] * 4)]
        with self.assertRaises(jc.JMPPEDError):
            jb._load_model("unused.obj", False, model, {23, 25, 2}, "TEST")

    def test_unowned_page_is_refused(self):
        model = copy.deepcopy(self.base.models[0])
        model.normals = []
        model.point_normals = []
        fd, path = tempfile.mkstemp(suffix=".obj")
        os.close(fd)
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write("o X\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                        "vt 0 0\nvt 1 0\nvt 0 1\n"
                        "usemtl s99_t1\nf 1/1 2/2 3/3\n")
            with self.assertRaises(jc.JMPPEDError):
                jb._load_model(path, False, model, {23, 25, 2}, "TEST")
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
