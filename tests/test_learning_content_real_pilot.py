"""Pinned Book Dash source + real 12-page Chinese pilot, offline fixtures.

The fake 12-page Markdown/JPEG and synthesized sine tones in these tests
are NOT the original artwork or child-usable spoken language. Live GitHub
fetch runs in a separate, explicitly networked integration workflow.
"""
from __future__ import annotations

import contextlib
from io import BytesIO, StringIO
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

from content_pipeline.assets import fetch_bookdash_images, combine_book_assets
from content_pipeline.cli import main
from content_pipeline.packaging import stage_book, build_reviewed_book
from content_pipeline.pilot import PILOT_REF, PILOT_SOURCE, prepare_cat_pilot
from content_pipeline.schema import ContentError, content_digest, write_json
from content_pipeline.source_snapshots import (
    fetch_bookdash_source, pinned_markdown_url)
from tools import learning_live_bookdash_pilot as live
from content_pipeline.speech import narrate_book
from tools import game_pack

ROOT = Path(__file__).resolve().parent.parent
TRANSLATIONS = ROOT / "curriculum/pilots/come_back_cat.zh_draft.json"
CREDITS = ROOT / "curriculum/pilots/come_back_cat.credits.json"


def _source_text() -> str:
    intro = (
        '---\nstylesheet: screen.css\ntitle: "Come back, cat!"\n'
        'published: true\nlayout: default\n---\n\n# Come back, cat!\n\n'
    )
    return intro + "".join(
        f'![Picture {n}]({{{{ site.image-set }}}}/{n:02d}.jpg)\n\n'
        + (f'Original English sentence {n}.\n\n' if n not in (5, 12) else '\n')
        for n in range(1, 13)
    )


def _jpeg(num: int) -> bytes:
    stream = BytesIO()
    Image.new("RGB", (300, 220), ("white" if num % 2 else "lightgray")).save(
        stream, format="JPEG")
    return stream.getvalue()


def _tone(text: str, file: Path, voice: str) -> None:
    game_pack.write_wav(file, game_pack.tone([400 + len(text)], note_s=0.12))


class PinnedBookDashPilot(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.snapshot = self.root / "snapshot"
        self.pilot = self.root / "pilot"
        self.source_bytes = _source_text().encode("utf-8")
        self.request_urls = []

    def tearDown(self):
        self.temp.cleanup()

    def _source(self, *, override: bytes | None = None):
        def fetch(url, limit):
            self.request_urls.append(url)
            self.assertEqual(limit, 256 * 1024)
            return self.source_bytes if override is None else override
        return fetch_bookdash_source(
            source_item_id=PILOT_SOURCE, source_ref=PILOT_REF, content_id=1101,
            output_dir=self.snapshot, license_id="unknown",
            skills=["language.story_sequence"], fetch=fetch)

    def _pilot(self):
        self._source()
        return prepare_cat_pilot(
            self.snapshot / "original.lcir.json", TRANSLATIONS, CREDITS,
            self.pilot)

    def test_pinned_upstream_text_and_wordless_pages(self):
        result = self._source()
        self.assertEqual(result["pages"], 12)
        self.assertEqual(result["image_only_pages"], [5, 12])
        self.assertEqual(self.request_urls, [
            "https://raw.githubusercontent.com/bookdash/bookdash-books/" +
            PILOT_REF + "/come-back-cat/en/index.md"])
        doc = json.loads((self.snapshot / "original.lcir.json").read_text(
            encoding="utf-8"))
        self.assertEqual([x["text"] for x in doc["pages"] if not x["text"]], ["", ""])
        self.assertEqual(doc["source"]["license_id"], "unknown")
        self.assertEqual(doc["review"]["status"], "draft")
        self.assertEqual(result["sha256"],
                         doc["source"]["source_files"][0]["sha256"])
        with self.assertRaisesRegex(ContentError, "already exists"):
            self._source()

    def test_reject_unpinned_paths_invalid_utf8_and_empty(self):
        with self.assertRaisesRegex(ContentError, "full 40"):
            pinned_markdown_url(PILOT_SOURCE, "gh-pages")
        with self.assertRaisesRegex(ContentError, "unsafe relative path|expected book-slug"):
            pinned_markdown_url("any/../index.md", PILOT_REF)
        with self.assertRaisesRegex(ContentError, "invalid UTF-8"):
            self._source(override=b"\xff")
        with self.assertRaisesRegex(ContentError, "missing or oversized"):
            self._source(override=b"")
        self.assertFalse((self.snapshot / "original.lcir.json").exists())

    def test_curated_credits_and_12_page_zh_draft(self):
        result = self._pilot()
        self.assertEqual(result["pages"], 12)
        self.assertEqual(result["status"], "draft")
        source = json.loads((self.pilot / "curated.en.lcir.json").read_text(
            encoding="utf-8"))
        zh = json.loads((self.pilot / "come_back_cat.zh.lcir.json").read_text(
            encoding="utf-8"))
        self.assertEqual(source["pages"][4]["text"], "")
        self.assertEqual(zh["pages"][4]["text"],
                         "小猫跑出门，开心地追着蝴蝶。")
        self.assertEqual(zh["pages"][11]["text"],
                         "原来，小猫在沙发上睡得正香呢！")
        self.assertEqual(source["source"]["creators"]["text"], ["Nicola Rijsdijk"])
        self.assertEqual(source["source"]["creators"]["illustration"], ["Karen Lilje"])
        self.assertEqual(zh["source"]["license_id"], "CC-BY-4.0")
        self.assertEqual(zh["review"]["status"], "draft")
        self.assertEqual(zh["transformations"][0]["source_digest"],
                         content_digest(source))
        self.assertTrue(json.loads((self.pilot / "attribution.json").read_text(
            encoding="utf-8"))["review_required"])
        with self.assertRaisesRegex(ContentError, "already exist"):
            prepare_cat_pilot(
                self.snapshot / "original.lcir.json", TRANSLATIONS, CREDITS,
                self.pilot)

    def test_mismatched_credits_and_source_fail_closed(self):
        self._source()
        credits = json.loads(CREDITS.read_text(encoding="utf-8"))
        credits["illustrator"] = "Incorrect Name"
        bad = self.root / "bad_credits.json"
        write_json(bad, credits)
        with self.assertRaisesRegex(ContentError, "credits differ"):
            prepare_cat_pilot(
                self.snapshot / "original.lcir.json", TRANSLATIONS, bad, self.pilot)
        self.assertFalse((self.pilot / "curated.en.lcir.json").exists())
        original = json.loads((self.snapshot / "original.lcir.json").read_text(
            encoding="utf-8"))
        original["source"]["source_ref"] = "d" * 40
        write_json(self.snapshot / "original.lcir.json", original)
        with self.assertRaisesRegex(ContentError, "expected pinned"):
            prepare_cat_pilot(
                self.snapshot / "original.lcir.json", TRANSLATIONS, CREDITS, self.pilot)

    def test_12_image_audio_preview_stages_but_no_release(self):
        self._pilot()
        translated = self.pilot / "come_back_cat.zh.lcir.json"
        asset_root = self.root / "assets"
        def fetch_image(url, limit):
            number = int(url.rsplit("/", 1)[-1].replace(".jpg", ""))
            return _jpeg(number)
        image_manifest = fetch_bookdash_images(
            translated, asset_root, fetch=fetch_image)
        self.assertEqual(len(image_manifest["images"]), 12)
        audio_manifest = narrate_book(
            translated, asset_root, voice="zh-CN-XiaoxiaoNeural",
            synthesize=_tone, test_only=True)
        self.assertEqual(audio_manifest["status"], "test_only")
        with self.assertRaisesRegex(ContentError, "invalid manifest"):
            combine_book_assets(translated, asset_root)
        mapped = combine_book_assets(
            translated, asset_root, allow_test_artifacts=True)
        self.assertEqual(len(mapped["pages"]), 12)
        stage = self.root / "staged"
        candidate = stage_book(translated, asset_root / "assets.json", stage)
        self.assertEqual(len(candidate["preview_files"]), 24)
        with Image.open(stage / "preview/page_12_mono.png") as preview:
            self.assertEqual(preview.size, (800, 480))
        review = self.root / "review.json"
        write_json(review, {
            "status": "reviewed", "reviewer": "CI synthetic test",
            "checked_hash": candidate["checked_hash"],
            "attestations": {
                "source_license": True, "illustrations": True,
                "text": True, "audio": True, "age_safety": True,
            },
        })
        with self.assertRaisesRegex(ContentError, "synthetic test-only narration"):
            build_reviewed_book(stage, review, self.root / "not_child.kpb")

    def test_live_runner_orchestration_with_offline_network_mocks(self):
        def source_stub(**kwargs):
            return fetch_bookdash_source(
                **kwargs, fetch=lambda url, limit: self.source_bytes)

        def image_stub(document, output):
            return fetch_bookdash_images(
                document, output,
                fetch=lambda url, limit: _jpeg(int(
                    url.rsplit("/", 1)[-1].replace(".jpg", ""))))

        with patch.object(live, "fetch_bookdash_source", side_effect=source_stub), \
             patch.object(live, "fetch_bookdash_images", side_effect=image_stub):
            result = live.build_research_preview(self.root / "orchestrated")
        self.assertEqual(result["status"], "test_only")
        self.assertEqual(result["source_images"], 12)
        self.assertEqual(result["audio_clips"], 12)
        self.assertEqual(result["audio_provider"], "offline_synthetic_fixture")
        self.assertEqual(result["epaper_previews"], 24)
        self.assertEqual(result["review_contact_sheets"], 2)
        self.assertTrue((self.root / "orchestrated/staged/review/index.html").exists())
        msg = (self.root / "orchestrated/README-RESEARCH-ONLY.txt").read_text(
            encoding="utf-8")
        self.assertIn("test-only sine tones", msg)
        self.assertIn("Nicola Rijsdijk", msg)

    def test_cli_creates_pilot_offline(self):
        self._source()
        with contextlib.redirect_stdout(StringIO()):
            code = main([
                "prepare-cat-pilot",
                "--input", str(self.snapshot / "original.lcir.json"),
                "--translations", str(TRANSLATIONS),
                "--credits", str(CREDITS),
                "--output-dir", str(self.pilot),
            ])
        self.assertEqual(code, 0)
        self.assertTrue((self.pilot / "come_back_cat.zh.lcir.json").is_file())


if __name__ == "__main__":
    unittest.main()
