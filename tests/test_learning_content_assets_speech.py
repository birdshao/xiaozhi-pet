"""LCP-06: offline image source/byte integrity and synthetic TTS tests.

No source illustration is downloaded and no real Edge online voice is called.
Generated graphics and tones are synthetic CI fixtures, not reviewed child
content. The test-only audio flag prevents normal asset-map preparation.
"""
from __future__ import annotations

import contextlib
from io import BytesIO, StringIO
import json
from pathlib import Path
import tempfile
import unittest
import wave

from PIL import Image

from content_pipeline.adapters import import_bookdash
from content_pipeline.assets import (
    MAX_IMAGE_BYTES, bookdash_url, combine_book_assets, fetch_bookdash_images)
from content_pipeline.cli import main
from content_pipeline.game_lessons import original_math_lesson, audio_script
from content_pipeline.game_packaging import stage_game, build_reviewed_game
from content_pipeline.packaging import stage_book, build_reviewed_book
from content_pipeline.localize import localize_bookdash
from content_pipeline.schema import ContentError, write_json
from content_pipeline.speech import narrate_book, narrate_game
from tools import game_pack as gp

PIN = "c" * 40
SOURCE = "sample-book/en/index.md"
STORY = """---
title: "Two Friends"
---
# Two Friends

![A circle]({{ site.image-set }}/01.jpg)

A circle lives in a round house.

![A square]({{ site.image-set }}/02.jpg)

A square lives in a square house.
"""


def _jpg(*, shade: str = "white") -> bytes:
    stream = BytesIO()
    Image.new("RGB", (460, 320), shade).save(stream, format="JPEG")
    return stream.getvalue()


def _synthetic_tone(text: str, path: Path, voice: str) -> None:
    # Tone has no linguistic content; CI fixtures only.
    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(24000)
        audio.writeframes(gp.tone([440 + len(text) * 5], note_s=0.12))


class BookAssets(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.doc = import_bookdash(
            STORY, source_item_id=SOURCE,
            source_url=f"https://github.com/bookdash/bookdash-books/blob/{PIN}/{SOURCE}",
            source_ref=PIN, content_id=1110, skills=["language.story_sequence"],
            locale="en", license_id="CC-BY-4.0")
        self.doc["source"]["creators"] = {
            "text": ["Fixture writer"], "illustration": ["Fixture artist"],
            "translation": []}
        self.lcir = self.root / "book.json"
        write_json(self.lcir, self.doc)
        self.assets = self.root / "assets"
        self.calls = []

    def tearDown(self):
        self.tmp.cleanup()

    def fetch(self, url, limit):
        self.calls.append(url)
        self.assertEqual(limit, MAX_IMAGE_BYTES)
        return _jpg(shade="lightgray" if url.endswith("/02.jpg") else "white")

    def test_exact_pinned_urls_and_image_manifest(self):
        manifest = fetch_bookdash_images(self.lcir, self.assets, fetch=self.fetch)
        self.assertEqual(len(manifest["images"]), 2)
        self.assertTrue(all(x["source_url"].startswith(
            f"https://raw.githubusercontent.com/bookdash/bookdash-books/{PIN}/{SOURCE[:-8]}")
            for x in manifest["images"]))
        self.assertEqual(manifest["status"], "draft")
        self.assertIn("images/page_01.jpg", [x["path"] for x in manifest["images"]])
        with self.assertRaisesRegex(ContentError, "already exists"):
            fetch_bookdash_images(self.lcir, self.assets, fetch=self.fetch)
        self.assertEqual(len(fetch_bookdash_images(
            self.lcir, self.assets, fetch=self.fetch, replace=True)["images"]), 2)

    def test_invalid_images_and_no_url_override(self):
        def broken(url, limit):
            return b"some HTML pretending to be JPEG"
        with self.assertRaisesRegex(ContentError, "unsafe JPEG|malformed"):
            fetch_bookdash_images(self.lcir, self.assets, fetch=broken)
        self.assertFalse((self.assets / "images.manifest.json").exists())
        doc = json.loads(self.lcir.read_text(encoding="utf-8"))
        doc["pages"][0]["image_ref"] = "../../outside.jpg"
        write_json(self.lcir, doc)
        with self.assertRaisesRegex(ContentError, "unsafe"):
            bookdash_url(doc, "../../outside.jpg")
        with self.assertRaisesRegex(ContentError, "unsafe"):
            fetch_bookdash_images(self.lcir, self.assets, fetch=self.fetch)
        doc = json.loads(self.lcir.read_text(encoding="utf-8"))
        doc["pages"][0]["image_ref"] = "images/01.jpg"
        doc["source"]["provider"] = "global-asp"
        with self.assertRaisesRegex(ContentError, "Book Dash"):
            bookdash_url(doc, "images/01.jpg")

    def test_book_audio_source_hash_and_stage_book(self):
        fetch_bookdash_images(self.lcir, self.assets, fetch=self.fetch)
        audio = narrate_book(self.lcir, self.assets, voice="en-US-AriaNeural",
                             synthesize=_synthetic_tone, test_only=True)
        self.assertEqual(audio["status"], "test_only")
        self.assertEqual(len(audio["audio"]), 2)
        with self.assertRaisesRegex(ContentError, "invalid manifest"):
            combine_book_assets(self.lcir, self.assets)
        mapped = combine_book_assets(self.lcir, self.assets,
                                     allow_test_artifacts=True)
        self.assertEqual(len(mapped["pages"]), 2)
        self.assertEqual(mapped["pages"]["1"]["audio"], "audio/page_01.wav")
        stage = self.root / "stage"
        candidate = stage_book(self.lcir, self.assets / "assets.json", stage)
        self.assertEqual(candidate["schema_version"], 1)
        review_path = self.root / "parent_review.json"
        write_json(review_path, {
            "status": "reviewed", "reviewer": "CI synthetic fixture",
            "checked_hash": candidate["checked_hash"],
            "attestations": {
                "source_license": True, "illustrations": True,
                "text": True, "audio": True, "age_safety": True,
            },
        })
        with self.assertRaisesRegex(ContentError, "synthetic test-only narration"):
            build_reviewed_book(stage, review_path, self.root / "not_child.kpb")
        with Image.open(stage / "page_02.png") as im:
            self.assertEqual(im.size, (800, 480))
        img = self.assets / "images" / "page_01.jpg"
        img.write_bytes(img.read_bytes() + b"modified")
        with self.assertRaisesRegex(ContentError, "changed"):
            combine_book_assets(self.lcir, self.assets, allow_test_artifacts=True)

    def test_book_text_or_voice_change_fails_closed(self):
        with self.assertRaisesRegex(ContentError, "voice must match"):
            narrate_book(self.lcir, self.assets, voice="zh-CN-XiaoxiaoNeural",
                         synthesize=_synthetic_tone, test_only=True)
        with self.assertRaisesRegex(ContentError, "explicitly test_only"):
            narrate_book(self.lcir, self.assets, voice="en-US-AriaNeural",
                         synthesize=_synthetic_tone)
        fetch_bookdash_images(self.lcir, self.assets, fetch=self.fetch)
        narrate_book(self.lcir, self.assets, voice="en-US-AriaNeural",
                      synthesize=_synthetic_tone, test_only=True)
        with self.assertRaisesRegex(ContentError, "already exists"):
            narrate_book(self.lcir, self.assets, voice="en-US-AriaNeural",
                         synthesize=_synthetic_tone, test_only=True)
        self.doc["pages"][0]["text"] += " More words."
        write_json(self.lcir, self.doc)
        with self.assertRaisesRegex(ContentError, "stale"):
            combine_book_assets(self.lcir, self.assets, allow_test_artifacts=True)

    def test_cli_uses_book_asset_checks(self):
        with contextlib.redirect_stdout(StringIO()), contextlib.redirect_stderr(StringIO()):
            self.assertEqual(main(["tts-book", "--input", str(self.lcir),
                                   "--output-dir", str(self.assets),
                                   "--voice", "zh-CN-XiaoxiaoNeural"]), 2)

    def test_bad_image_ref_unrecognized_scheme(self):
        with self.assertRaisesRegex(ContentError, "unsafe|expected"):
            bookdash_url(self.doc, "images/../../api.jpg")
        with self.assertRaisesRegex(ContentError, "expected"):
            bookdash_url(self.doc, "images/hello.svg")
        with self.assertRaisesRegex(ContentError, "expected"):
            bookdash_url(self.doc, "images/hello.png")


class GameAudio(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.lesson = original_math_lesson()
        self.lesson_file = self.root / "lesson.json"
        write_json(self.lesson_file, self.lesson)
        self.assets = self.root / "assets"

    def tearDown(self):
        self.temp.cleanup()

    def test_15_rounds_audio_batch_and_stage(self):
        manifest = narrate_game(self.lesson_file, self.assets,
                                voice="zh-CN-XiaoxiaoNeural",
                                synthesize=_synthetic_tone, test_only=True)
        self.assertEqual(manifest["status"], "test_only")
        self.assertEqual(len(manifest["audio"]), len(audio_script(self.lesson)["clips"]))
        self.assertLessEqual(len(manifest["audio"]), gp.MAX_CLIPS)
        staged = stage_game(self.lesson_file, self.assets / "audio_map.json",
                            self.root / "stage")
        self.assertEqual(staged["schema_version"], 1)
        review = self.root / "review.json"
        write_json(review, {
            "status": "reviewed", "reviewer": "CI synthetic fixture",
            "checked_hash": staged["checked_hash"],
            "attestations": {
                "educational_accuracy": True, "answer_uniqueness": True,
                "audio": True, "age_safety": True, "source": True,
            },
        })
        with self.assertRaisesRegex(ContentError, "synthetic test-only TTS"):
            build_reviewed_game(self.root / "stage", review,
                                self.root / "game" / "default.kpg")
        with self.assertRaisesRegex(ContentError, "already exists"):
            narrate_game(self.lesson_file, self.assets, voice="zh-CN-XiaoxiaoNeural",
                         synthesize=_synthetic_tone, test_only=True)

    def test_bad_game_voice_and_sample_rate(self):
        with self.assertRaisesRegex(ContentError, "voice must match"):
            narrate_game(self.lesson_file, self.assets, voice="en-US-AriaNeural",
                         synthesize=_synthetic_tone, test_only=True)
        def bad_sample(text, target, voice):
            with wave.open(str(target), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(b"\x00\x00" * 2000)
        with self.assertRaisesRegex(ValueError, "24 kHz"):
            narrate_game(self.lesson_file, self.assets, voice="zh-CN-XiaoxiaoNeural",
                         synthesize=bad_sample, test_only=True)



class LocalizedBook(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.english = self.root / "english.json"
        doc = import_bookdash(
            STORY, source_item_id=SOURCE,
            source_url=f"https://github.com/bookdash/bookdash-books/blob/{PIN}/{SOURCE}",
            source_ref=PIN, content_id=1101, skills=["language.story_sequence"],
            locale="en", license_id="CC-BY-4.0")
        doc["source"]["creators"] = {
            "text": ["Research writer"], "illustration": ["Research artist"],
            "translation": []}
        write_json(self.english, doc)
        self.trans = self.root / "translation.json"
        write_json(self.trans, {
            "schema_version": 1, "title": "两个好朋友",
            "translator": "人工复核负责人",
            "pages": ["圆圆住在圆圆的小房子里。", "方方住在方方的小房子里。"],
        })
        self.chinese = self.root / "chinese.json"
        self.assets = self.root / "assets"

    def tearDown(self):
        self.temp.cleanup()

    def test_chinese_images_and_narration_follow_same_pinned_original(self):
        original = json.loads(self.english.read_text(encoding="utf-8"))
        translated = localize_bookdash(self.english, self.trans, self.chinese,
                                      content_id=1301)
        self.assertEqual(translated["locale"], "zh-Hans")
        self.assertEqual(translated["source"]["source_ref"], PIN)
        self.assertEqual(translated["pages"][0]["image_ref"], original["pages"][0]["image_ref"])
        self.assertEqual(translated["review"]["status"], "draft")
        self.assertEqual(translated["source"]["creators"]["translation"],
                         ["人工复核负责人"])
        self.assertEqual(len(translated["transformations"]), 1)
        manifest = fetch_bookdash_images(
            self.chinese, self.assets, fetch=lambda url, limit: _jpg())
        self.assertEqual(len(manifest["images"]), 2)
        wavs = narrate_book(self.chinese, self.assets, voice="zh-CN-XiaoxiaoNeural",
                            synthesize=_synthetic_tone, test_only=True)
        self.assertEqual(len(wavs["audio"]), 2)
        mapped = combine_book_assets(self.chinese, self.assets,
                                     allow_test_artifacts=True)
        self.assertEqual(len(mapped["pages"]), 2)
        staged = stage_book(self.chinese, self.assets / "assets.json",
                            self.root / "staged")
        self.assertEqual(staged["schema_version"], 1)

    def test_translation_rejects_missing_pages_and_non_distinct_ids(self):
        with self.assertRaisesRegex(ContentError, "distinct content_id"):
            localize_bookdash(self.english, self.trans, self.chinese, content_id=1101)
        raw = json.loads(self.trans.read_text(encoding="utf-8"))
        raw["pages"].pop()
        write_json(self.trans, raw)
        with self.assertRaisesRegex(ContentError, "exactly one"):
            localize_bookdash(self.english, self.trans, self.chinese, content_id=1301)

if __name__ == "__main__":
    unittest.main()
