"""COURSEINDEX1: verify real device package bytes and produce local-only index.

This suite uses synthetic tones/graphics, NEVER child-facing content.
"""
from __future__ import annotations

import contextlib
import copy
import hashlib
from io import StringIO
import json
from pathlib import Path
import tempfile
import unittest

from content_pipeline.catalog import build_index, load_course, load_skills
from content_pipeline.cli import main
from content_pipeline.schema import ContentError, write_json
from tools import game_pack, make_reader_test_book, story_pack

ROOT = Path(__file__).resolve().parent.parent
SKILLS = ROOT / "curriculum" / "skills.v1.json"
COURSE = ROOT / "curriculum" / "courses" / "shapes_and_numbers.v1.json"


class CourseCatalog(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        src = game_pack.demo_source(self.dir / "source", content_id=20001,
                                    note_s=0.08)
        self.pack = self.dir / "games" / "default.kpg"
        game_pack.make_pack(src, self.pack, reviewed=True, placeholder=True)
        self.out = self.dir / "out" / "course_index.json"

    def tearDown(self):
        self.tmp.cleanup()

    def test_course_skills_and_local_test_only_index(self):
        skills = load_skills(SKILLS)
        course = load_course(COURSE, skills)
        self.assertEqual(len(course["units"]), 3)
        self.assertTrue({"math.shapes_basic", "math.count_1_5",
                         "math.size_compare"} <= skills)
        with self.assertRaisesRegex(ContentError, "test tone placeholders"):
            build_index(COURSE, SKILLS, [self.pack], self.out)
        result = build_index(COURSE, SKILLS, [self.pack], self.out,
                             allow_test_artifacts=True)
        self.assertEqual(result["status"], "test_only")
        self.assertEqual(result["course_id"], "preschool.shapes_and_numbers")
        self.assertEqual(len(result["packages"]), 1)
        self.assertEqual(len(result["units"]), 3)
        self.assertEqual(result["packages"][0]["sd_path"], "/games/default.kpg")
        self.assertEqual(result["packages"][0]["sha256"],
                         hashlib.sha256(self.pack.read_bytes()).hexdigest())
        self.assertEqual(json.loads(self.out.read_text(encoding="utf-8")), result)
        self.assertEqual(result, build_index(
            COURSE, SKILLS, [self.pack], self.out, allow_test_artifacts=True))

    def test_tampered_binary_or_sidecar_rejected(self):
        contents = self.pack.read_bytes()
        self.pack.write_bytes(contents[:-1] + bytes([contents[-1] ^ 1]))
        with self.assertRaisesRegex(ContentError, "hash mismatch"):
            build_index(COURSE, SKILLS, [self.pack], self.out,
                        allow_test_artifacts=True)
        self.pack.write_bytes(contents)
        sidecar = self.pack.with_suffix(".manifest.json")
        meta = json.loads(sidecar.read_text(encoding="utf-8"))
        meta["content_id"] = 20002
        write_json(sidecar, meta)
        with self.assertRaisesRegex(ContentError, "sidecar/header"):
            build_index(COURSE, SKILLS, [self.pack], self.out,
                        allow_test_artifacts=True)

    def test_unknown_skill_unreferenced_and_duplicate_pack(self):
        course = json.loads(COURSE.read_text(encoding="utf-8"))
        course["units"][0]["skill_id"] = "math.unknown_skill"
        modified = self.dir / "badcourse.json"
        write_json(modified, course)
        with self.assertRaisesRegex(ContentError, "unknown skill"):
            build_index(modified, SKILLS, [self.pack], self.out,
                        allow_test_artifacts=True)
        with self.assertRaisesRegex(ContentError, "repeated content_id"):
            build_index(COURSE, SKILLS, [self.pack, self.pack], self.out,
                        allow_test_artifacts=True)
        course["units"][0]["skill_id"] = "math.shapes_basic"
        course["units"][0]["items"][0]["content_id"] = 123
        write_json(modified, course)
        with self.assertRaisesRegex(ContentError, "missing package"):
            build_index(modified, SKILLS, [self.pack], self.out,
                        allow_test_artifacts=True)

    def test_read_back_real_kpb2_and_kpg1(self):
        book_source = make_reader_test_book.write_source(self.dir / "books-src")
        data = json.loads(book_source.read_text(encoding="utf-8"))
        data["content_id"] = 1001
        write_json(book_source, data)
        book = self.dir / "books" / "reader_test.kpb"
        story_pack.make_pack(book_source, book, reviewed=True)
        course = json.loads(COURSE.read_text(encoding="utf-8"))
        course["units"].append({
            "unit_id": "read_story", "name": "学习动作词",
            "skill_id": "language.action_verbs",
            "items": [{"kind": "book", "content_id": 1001}],
        })
        path = self.dir / "with_book.json"
        write_json(path, course)
        result = build_index(path, SKILLS, [book, self.pack], self.out,
                             allow_test_artifacts=True)
        self.assertEqual(len(result["packages"]), 2)
        book_record = next(p for p in result["packages"] if p["kind"] == "book")
        self.assertEqual(book_record["item_count"], 3)
        self.assertEqual(book_record["sd_path"], "/books/reader_test.kpb")
        self.assertEqual(result["units"][-1]["items"][0]["sha256"],
                         book_record["sha256"])

    def test_cli_index_test_only(self):
        with contextlib.redirect_stdout(StringIO()):
            self.assertEqual(main([
                "build-course-index", "--course", str(COURSE),
                "--skills", str(SKILLS), "--package", str(self.pack),
                "--output", str(self.out), "--test-only",
            ]), 0)
        self.assertEqual(json.loads(self.out.read_text(encoding="utf-8"))["status"],
                         "test_only")


if __name__ == "__main__":
    unittest.main()
