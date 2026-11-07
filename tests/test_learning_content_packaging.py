"""Stage real 800x480 images/WAVs and build KPB2 only with human review.

All example images/audio here are synthetic local test fixtures, NOT child content.
"""
from __future__ import annotations

import json
import math
import struct
from pathlib import Path
import tempfile
import unittest
import wave

from PIL import Image

from content_pipeline.adapters import import_bookdash
from content_pipeline.packaging import build_reviewed_book, stage_book
from content_pipeline.review_packet import make_review_packet
from content_pipeline.review_templates import make_review_template
from content_pipeline.schema import ContentError, write_json
from tools import kpb2

SHA = "b" * 40
BOOK = """---
title: "Two Toy Friends"
---
# Two Toy Friends

![A circle]({{ site.image-set }}/01.jpg)

A circle is round.

![A square]({{ site.image-set }}/02.jpg)

A square has four sides.
"""


def write_tone(path, frames=12000, amplitude=8000):
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(24000)
        f.writeframes(b"".join(
            struct.pack("<h", int(amplitude * math.sin(i / 8))) for i in range(frames)))


class Packaging(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.assets = self.root / "assets"
        self.assets.mkdir()
        for i in (1, 2):
            im = Image.new("RGB", (400 + i * 50, 320), ("white" if i == 1 else "lightgray"))
            im.save(self.assets / f"image_{i}.png")
            write_tone(self.assets / f"audio_{i}.wav")
        self.map_file = self.assets / "assets.json"
        write_json(self.map_file, {
            "schema_version": 1,
            "pages": {
                str(i): {"image": f"image_{i}.png", "audio": f"audio_{i}.wav"}
                for i in (1, 2)
            }
        })
        self.lcir = self.root / "imported.json"
        self.write_doc("CC-BY-4.0")
        self.stage = self.root / "stage"

    def tearDown(self):
        self.tmp.cleanup()

    def write_doc(self, license_id):
        doc = import_bookdash(
            BOOK, source_item_id="two-toy-friends/en/index.md",
            source_url="https://github.com/bookdash/bookdash-books/blob/" + SHA +
                       "/two-toy-friends/en/index.md",
            source_ref=SHA, content_id=1101,
            license_id=license_id,
            skills=["math.shapes_basic"],
        )
        doc["source"]["creators"] = {"text": ["Test Writer"],
                                     "illustration": ["Test Artist"],
                                     "translation": []}
        write_json(self.lcir, doc)

    def review(self, hash):
        file = self.root / "review.json"
        write_json(file, {
            "status": "reviewed",
            "reviewer": "Test reviewer",
            "checked_hash": hash,
            "attestations": {
                "source_license": True, "illustrations": True,
                "text": True, "audio": True, "age_safety": True,
                "voice_terms_cleared": True,  # fixture has no audio.manifest.json
            }
        })
        return file

    def test_real_kpb2_roundtrip(self):
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        staged_book = json.loads((self.stage / "book.json").read_text(encoding="utf-8"))
        self.assertFalse(staged_book["approved"])
        self.assertTrue(all(not p["approved"] for p in staged_book["pages"]))
        with Image.open(self.stage / "page_01.png") as im:
            self.assertEqual(im.size, (800, 480))
        for mode in ("mono", "grey"):
            preview = self.stage / "preview" / f"page_01_{mode}.png"
            self.assertIn(f"preview/page_01_{mode}.png", candidate["preview_files"])
            with Image.open(preview) as im:
                self.assertEqual(im.size, (800, 480))
                self.assertEqual(im.mode, "L")
        result = build_reviewed_book(self.stage, self.review(candidate["checked_hash"]),
                                     self.root / "book.kpb")
        self.assertGreater(result["package_bytes"], 0)
        self.assertTrue((self.root / "book.manifest.json").exists())
        parsed = kpb2.parse((self.root / "book.kpb").read_bytes())
        self.assertEqual(parsed["format"], "KPB2")
        self.assertEqual(len(parsed["pages"]), 3)  # 2 + credits page
        self.assertFalse((self.stage / "__reviewed_kpb2_temp.json").exists())
        self.assertFalse(staged_book["approved"])

    def test_read_only_epaper_review_packet(self):
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        review = make_review_packet(self.stage)
        self.assertEqual(review["status"], "unreviewed")
        self.assertEqual(review["pages"], 3)  # 2 + credits page
        self.assertEqual(review["checked_hash"], candidate["checked_hash"])
        page = (self.stage / "review/index.html").read_text(encoding="utf-8")
        self.assertIn("NOT APPROVED", page)
        self.assertIn("Two Toy Friends", page)
        self.assertIn("../page_01.wav", page)
        self.assertFalse((self.stage / "__reviewed_kpb2_temp.json").exists())
        for image in ("contact-mono.png", "contact-grey.png"):
            with Image.open(self.stage / "review" / image) as sheet:
                self.assertGreater(sheet.width, 800)
                self.assertGreater(sheet.height, 190)
        with (self.stage / "preview/page_01_mono.png").open("ab") as out:
            out.write(b"tamper")
        with self.assertRaisesRegex(ContentError, "changed"):
            make_review_packet(self.stage)

    def test_modified_stage_rejected(self):
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        review = self.review(candidate["checked_hash"])
        with (self.stage / "page_01.wav").open("ab") as f:
            f.write(b"modification")
        with self.assertRaisesRegex(ContentError, "changed"):
            build_reviewed_book(self.stage, review, self.root / "book.kpb")

    def test_review_template_defaults_false_and_cannot_publish_book(self):
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        report = make_review_template(self.stage, kind="book")
        self.assertEqual(report["checked_hash"], candidate["checked_hash"])
        template_file = self.stage / "review.template.json"
        values = json.loads(template_file.read_text(encoding="utf-8"))
        self.assertEqual(values["status"], "review_pending")
        self.assertTrue(all(check is False for check in values["attestations"].values()))
        with self.assertRaisesRegex(ContentError, "not approved"):
            build_reviewed_book(self.stage, template_file, self.root / "book.kpb")

    def test_modified_epaper_preview_rejected(self):
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        review = self.review(candidate["checked_hash"])
        preview = self.stage / "preview" / "page_01_mono.png"
        with preview.open("ab") as output:
            output.write(b"tampered")
        with self.assertRaisesRegex(ContentError, "changed"):
            build_reviewed_book(self.stage, review, self.root / "untrusted.kpb")

    def test_not_reviewed_and_unknown_license_rejected(self):
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        review = self.review(candidate["checked_hash"])
        data = json.loads(review.read_text(encoding="utf-8"))
        data["attestations"]["audio"] = False
        write_json(review, data)
        with self.assertRaisesRegex(ContentError, "five human"):
            build_reviewed_book(self.stage, review, self.root / "no.kpb")
        self.write_doc("unknown")
        stage_book(self.lcir, self.map_file, self.stage)
        candidate = json.loads((self.stage / "candidate.json").read_text(encoding="utf-8"))
        review = self.review(candidate["checked_hash"])
        with self.assertRaisesRegex(ContentError, "exact source license"):
            build_reviewed_book(self.stage, review, self.root / "no.kpb")

    def test_invalid_audio_and_long_story_rejected(self):
        with wave.open(str(self.assets / "audio_2.wav"), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b"\x00\x00" * 100)
        with self.assertRaisesRegex(ContentError, "24 kHz"):
            stage_book(self.lcir, self.map_file, self.stage)
        doc = json.loads(self.lcir.read_text(encoding="utf-8"))
        doc["pages"][0]["text"] = "很" * 321
        write_json(self.lcir, doc)
        with self.assertRaisesRegex(ContentError, "too long"):
            stage_book(self.lcir, self.map_file, self.stage)

    def test_symlink_outside_asset_root_rejected(self):
        outside = self.root / "outside.png"
        Image.new("RGB", (200, 200), "white").save(outside)
        try:
            (self.assets / "escaped.png").symlink_to(outside)
        except OSError as exc:
            self.skipTest(f"cannot create symlinks on this OS: {exc}")
        mapping = json.loads(self.map_file.read_text(encoding="utf-8"))
        mapping["pages"]["1"]["image"] = "escaped.png"
        write_json(self.map_file, mapping)
        with self.assertRaisesRegex(ContentError, "escapes"):
            stage_book(self.lcir, self.map_file, self.stage)


if __name__ == "__main__":
    unittest.main()
