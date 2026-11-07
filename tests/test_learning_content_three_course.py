"""LCP-10: three real KPG1 test pack identities and safe SD-card staging.

Only synthetic test tones are generated in CI. No automatic parental approval.
"""
from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

from content_pipeline.catalog import build_index
from content_pipeline.game_lessons import (
    original_math_lesson, original_math_unit_lessons, validate_lesson)
from content_pipeline.schema import ContentError, write_json
from tools.learning_course_sd import stage_course_card
from tools.learning_math_unit_sources import write_unit_sources
from tools.learning_three_course_fixture import make_three_course_fixture, COURSE, SKILLS

class ThreeCourseSelection(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.built = tempfile.TemporaryDirectory()
        cls.root = Path(cls.built.name)
        cls.fixture = cls.root / "fixture"
        cls.report = make_three_course_fixture(cls.fixture)

    @classmethod
    def tearDownClass(cls):
        cls.built.cleanup()

    def test_three_unique_valid_draft_course_sources(self):
        docs = original_math_unit_lessons()
        self.assertEqual(list(docs), ["shapes", "counts", "sizes"])
        self.assertEqual([x["content_id"] for x in docs.values()],
                         [20011, 20012, 20013])
        self.assertEqual([len(x["rounds"]) for x in docs.values()], [5, 5, 5])
        self.assertTrue(all(x["review"] == {"status": "draft"}
                            for x in docs.values()))
        original = original_math_lesson()
        self.assertEqual(original["content_id"], 20001)
        self.assertEqual(sum(len(x["rounds"]) for x in docs.values()), 15)
        for name, doc in docs.items():
            self.assertEqual(validate_lesson(doc), doc)
            self.assertEqual(doc["rounds"], [r for r in original["rounds"]
                                             if r["kind"] == doc["rounds"][0]["kind"]])
        mutated = copy.deepcopy(docs["shapes"])
        mutated["rounds"][0]["kind"] = "count"
        with self.assertRaisesRegex(ContentError, "unit course"):
            validate_lesson(mutated)
        mutated = copy.deepcopy(docs["sizes"])
        mutated["content_id"] = 20011
        with self.assertRaisesRegex(ContentError, "identity"):
            validate_lesson(mutated)

    def test_existing_real_kpg1_encoder_and_multi_course_index(self):
        self.assertEqual(self.report["status"], "test_only")
        self.assertEqual(self.report["active"], "counts")
        self.assertEqual([x["content_id"] for x in self.report["packs"]],
                         [20011, 20012, 20013])
        card = self.fixture / "sdcard"
        self.assertEqual((card / "games/active.txt").read_bytes(), b"counts\n")
        names = ["shapes", "counts", "sizes"]
        self.assertEqual(sorted(p.stem for p in (card / "games").glob("*.kpg")),
                         sorted(names))
        index = json.loads((card / "course-index.json").read_text(encoding="utf-8"))
        self.assertEqual(index["status"], "test_only")
        self.assertEqual(len(index["packages"]), 3)
        self.assertEqual({pkg["sd_path"] for pkg in index["packages"]},
                         {f"/games/{name}.kpg" for name in names})
        self.assertEqual(len(index["units"]), 3)

    def test_no_approval_or_placeholder_leaks_to_regular_card(self):
        packages = [self.fixture / f"fixtures/{n}.kpg"
                    for n in ("shapes", "counts", "sizes")]
        with tempfile.TemporaryDirectory() as folder:
            dest = Path(folder) / "production"
            with self.assertRaisesRegex(ContentError, "test tone placeholders"):
                stage_course_card(
                    course_file=COURSE, skills_file=SKILLS, packages=packages,
                    active="counts", output=dest)
            self.assertFalse(dest.exists())
            with self.assertRaisesRegex(ContentError, "not among"):
                stage_course_card(
                    course_file=COURSE, skills_file=SKILLS, packages=packages,
                    active="default", output=dest, test_only=True)
            self.assertFalse(dest.exists())
            with self.assertRaisesRegex(ContentError, "unsafe active"):
                stage_course_card(
                    course_file=COURSE, skills_file=SKILLS, packages=packages,
                    active="../counts", output=dest, test_only=True)
            self.assertFalse(dest.exists())

    def test_tampered_kpg1_rejected_without_partial_card(self):
        packages = [self.fixture / f"fixtures/{n}.kpg"
                    for n in ("shapes", "counts", "sizes")]
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            altered = p / "counts.kpg"
            altered.write_bytes(packages[1].read_bytes()[:-1] + b"\\x00")
            # Preserve the original manifest SHA to detect the binary change.
            altered.with_suffix(".manifest.json").write_bytes(
                packages[1].with_suffix(".manifest.json").read_bytes())
            with self.assertRaisesRegex(ContentError, "hash mismatch"):
                stage_course_card(
                    course_file=COURSE, skills_file=SKILLS,
                    packages=[packages[0], altered, packages[2]],
                    active="counts", output=p / "card", test_only=True)
            self.assertFalse((p / "card").exists())

    def test_writer_exports_review_pending_units(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "drafts"
            result = write_unit_sources(root)
            self.assertEqual(result["status"], "draft")
            self.assertEqual(len(result["units"]), 3)
            for name in ("shapes", "counts", "sizes"):
                lesson = json.loads((root / name / "lesson.json").read_text(
                    encoding="utf-8"))
                self.assertEqual(lesson["review"]["status"], "draft")
                self.assertEqual(len(lesson["rounds"]), 5)
            with self.assertRaisesRegex(ContentError, "overwrite"):
                write_unit_sources(root)


if __name__ == "__main__":
    unittest.main()
