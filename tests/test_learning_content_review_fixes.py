"""Regression tests for the learning-content stack review findings.

All images/audio are synthetic fixtures, not child content.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest
import wave

from PIL import Image

from content_pipeline.adapters import import_bookdash, import_global_asp
from content_pipeline.audio_qc import analyze_wav
from content_pipeline.game_lessons import (audio_map, audio_script,
                                           original_math_lesson)
from content_pipeline.game_packaging import build_reviewed_game, stage_game
from content_pipeline.game_review import make_game_review_packet
from content_pipeline.packaging import (_render, build_reviewed_book,
                                        credits_text, stage_book)
from content_pipeline.schema import (ContentError, adaptation_terms,
                                     content_digest, write_json)
from tools import game_pack as gp
from tools import kpb2

SHA = "c" * 40
MULTI = """---
title: "Two Toy Friends"
---
# Two Toy Friends

![A circle]({{ site.image-set }}/01.jpg)

A circle is round.

It rolls away.

![A square]({{ site.image-set }}/02.jpg)

A square has
four sides.
"""
ASP = """# 你在做什么？

##

第一段。

第二段。

* License: CC-BY
* Text: 作者甲
* Illustration: 画家乙
* Language: zh
"""


def tone_frames(frames, amplitude):
    return b"".join(struct.pack("<h", int(amplitude * math.sin(i / 8)))
                    for i in range(frames))


def write_tone(path, frames=12000, amplitude=8000):
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(24000)
        f.writeframes(tone_frames(frames, amplitude))


def dash(license_id="CC-BY-4.0", text=MULTI):
    doc = import_bookdash(
        text, source_item_id="two-toy-friends/en/index.md",
        source_url="https://github.com/bookdash/bookdash-books/blob/" + SHA +
                   "/two-toy-friends/en/index.md",
        source_ref=SHA, content_id=1101, license_id=license_id, skills=[])
    doc["source"]["creators"] = {"text": ["Test Writer"], "illustration": ["Test Artist"],
                                 "design": ["Test Designer"], "translation": []}
    return doc


class BookFixtures(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.assets = self.root / "assets"
        self.assets.mkdir()
        for i in (1, 2):
            Image.new("RGB", (400, 300), "white").save(self.assets / f"image_{i}.png")
            write_tone(self.assets / f"audio_{i}.wav")
        self.map_file = self.assets / "assets.json"
        write_json(self.map_file, {"schema_version": 1, "pages": {
            str(i): {"image": f"image_{i}.png", "audio": f"audio_{i}.wav"} for i in (1, 2)}})
        self.lcir = self.root / "doc.json"
        self.stage = self.root / "stage"

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, doc):
        write_json(self.lcir, doc)

    def review(self, checked_hash, **extra):
        attest = {"source_license": True, "illustrations": True, "text": True,
                  "audio": True, "age_safety": True, **extra}
        path = self.root / "review.json"
        write_json(path, {"status": "reviewed", "reviewer": "CI", "attestations": attest,
                          "checked_hash": checked_hash})
        return path

    # 1
    def test_multi_paragraph_text_is_flattened_and_builds(self):
        doc = dash()
        self.assertEqual([p["text"] for p in doc["pages"]],
                         ["A circle is round. It rolls away.", "A square has four sides."])
        asp = import_global_asp(
            ASP, source_item_id="zh/0001_demo.md", source_url="https://example.org/a",
            source_ref=SHA, content_id=1102, license_id="CC-BY-4.0")
        self.assertEqual(asp["pages"][0]["text"], "第一段。第二段。")
        self.put(doc)
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        built = build_reviewed_book(self.stage, self.review(candidate["checked_hash"]),
                                    self.root / "b.kpb")
        self.assertGreater(built["package_bytes"], 0)

    def test_stage_fails_before_review_on_kpb2_violations(self):
        doc = dash()
        doc["title"] = "T" * 81
        self.put(doc)
        with self.assertRaisesRegex(ContentError, "title"):
            stage_book(self.lcir, self.map_file, self.stage)
        doc = dash()
        doc["pages"][0]["text"] = "line one\nline two"  # e.g. an unflattened import
        self.put(doc)
        with self.assertRaisesRegex(ContentError, "Controls"):
            stage_book(self.lcir, self.map_file, self.stage)
        doc = dash()
        doc["pages"][1]["text"] = ""
        self.put(doc)
        with self.assertRaisesRegex(ContentError, "missing"):
            stage_book(self.lcir, self.map_file, self.stage)
        self.assertFalse((self.stage / "candidate.json").exists())

    # 2 + 3
    def test_credits_reach_manifest_and_final_page(self):
        self.put(dash())
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        built = build_reviewed_book(self.stage, self.review(candidate["checked_hash"]),
                                    self.root / "b.kpb")
        meta = json.loads((self.root / "b.manifest.json").read_text(encoding="utf-8"))
        credits = meta["credits"]
        self.assertEqual(credits["creators"]["design"], ["Test Designer"])
        self.assertEqual(credits["creators"]["text"], ["Test Writer"])
        self.assertEqual(credits["license_id"], "CC-BY-4.0")
        self.assertEqual(credits["license_url"], "https://creativecommons.org/licenses/by/4.0/")
        self.assertIn("github.com/bookdash", credits["source_url"])
        self.assertEqual(credits["source_ref"], SHA)
        self.assertEqual(credits["modified"], ["resized", "narrated"])
        self.assertEqual(meta["checked_hash"], built["checked_hash"])
        parsed = kpb2.parse((self.root / "b.kpb").read_bytes())
        self.assertEqual(len(parsed["pages"]), 3)
        self.assertEqual(meta["pages"][-1]["text"], credits_text(credits))
        for needle in ("Test Writer", "Test Artist", "Test Designer", "CC BY 4.0"):
            self.assertIn(needle, meta["pages"][-1]["text"])

    def test_nd_license_refused_and_sa_recorded(self):
        self.put(dash("CC-BY-ND-4.0"))
        with self.assertRaisesRegex(ContentError, "NoDerivatives"):
            stage_book(self.lcir, self.map_file, self.stage)
        with self.assertRaisesRegex(ContentError, "forbids derivatives"):
            adaptation_terms("CC-BY-NC-ND-4.0")
        self.put(dash("CC-BY-SA-4.0"))
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        build_reviewed_book(self.stage, self.review(candidate["checked_hash"]),
                            self.root / "sa.kpb")
        meta = json.loads((self.root / "sa.manifest.json").read_text(encoding="utf-8"))
        self.assertIs(meta["credits"]["share_alike"], True)

    # 4
    def test_near_silent_narration_rejected_at_stage(self):
        write_tone(self.assets / "audio_2.wav", amplitude=20)  # about -64 dBFS RMS
        self.put(dash())
        with self.assertRaisesRegex(ContentError, "near-silent"):
            stage_book(self.lcir, self.map_file, self.stage)
        quiet = self.root / "quiet.wav"
        write_tone(quiet, amplitude=600)  # quiet but usable: warning only
        self.assertIn("low_rms_level", analyze_wav(quiet)["warnings"])

    # 5
    def test_edge_tts_book_needs_voice_terms_attestation(self):
        doc = dash()
        self.put(doc)
        write_json(self.assets / "audio.manifest.json", {
            "schema_version": "BOOKAUDIO1", "status": "draft", "provider": "edge-tts",
            "item_uid": doc["item_uid"], "content_digest": content_digest(doc)})
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        with self.assertRaisesRegex(ContentError, "voice_terms_cleared"):
            build_reviewed_book(self.stage, self.review(candidate["checked_hash"]),
                                self.root / "e.kpb")
        built = build_reviewed_book(
            self.stage, self.review(candidate["checked_hash"], voice_terms_cleared=True),
            self.root / "e.kpb")
        self.assertGreater(built["package_bytes"], 0)

    # 6
    def test_catalog_requires_review_gate_hash(self):
        from content_pipeline.catalog import _verified_package
        from tools import story_pack
        self.put(dash())
        candidate = stage_book(self.lcir, self.map_file, self.stage)
        build_reviewed_book(self.stage, self.review(candidate["checked_hash"]),
                            self.root / "gate.kpb")
        info = _verified_package(self.root / "gate.kpb", allow_test_artifacts=False)
        self.assertEqual(info["kind"], "book")
        # A pack built by calling make_pack directly has no gate hash.
        staged = json.loads((self.stage / "book.json").read_text(encoding="utf-8"))
        staged["approved"] = True
        for page in staged["pages"]:
            page["approved"] = True
        staged["reviewer"] = "direct"
        write_json(self.stage / "direct.json", staged)
        story_pack.make_pack(self.stage / "direct.json", self.root / "direct.kpb",
                             reviewed=True)
        with self.assertRaisesRegex(ContentError, "checked_hash"):
            _verified_package(self.root / "direct.kpb", allow_test_artifacts=False)
        _verified_package(self.root / "direct.kpb", allow_test_artifacts=True)

    # 7
    def test_transparent_png_renders_on_white(self):
        source = self.root / "clear.png"
        Image.new("RGBA", (100, 100), (0, 0, 0, 0)).save(source)
        target = self.root / "out.png"
        _render(source, target)
        with Image.open(target) as image:
            self.assertEqual(image.getpixel((400, 240)), (255, 255, 255))
            self.assertEqual(image.getpixel((0, 0)), (255, 255, 255))


class GameFixtures(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.lesson = original_math_lesson()
        self.lesson_path = self.root / "lesson.json"
        write_json(self.lesson_path, self.lesson)
        self.audio = self.root / "audio"
        (self.audio / "clips").mkdir(parents=True)
        write_json(self.audio / "audio_map.json", audio_map(self.lesson))
        self.keys = list(audio_script(self.lesson)["clips"])
        for n, key in enumerate(self.keys):
            gp.write_wav(self.audio / "clips" / f"{key}.wav",
                         gp.tone([350 + n * 13], note_s=0.10))
        self.stage = self.root / "stage"

    def tearDown(self):
        self.tmp.cleanup()

    def test_review_html_lists_each_warning_clip(self):
        write_tone(self.audio / "clips" / f"{self.keys[0]}.wav", 2400, 600)
        stage_game(self.lesson_path, self.audio / "audio_map.json", self.stage)
        make_game_review_packet(self.stage)
        page = (self.stage / "review" / "index.html").read_text(encoding="utf-8")
        self.assertIn("clipwarnings", page)
        self.assertIn(f"clips/{self.keys[0]}.wav", page)
        self.assertIn("low_rms_level", page)

    def test_near_silent_game_clip_rejected(self):
        write_tone(self.audio / "clips" / f"{self.keys[0]}.wav", 2400, 20)
        with self.assertRaisesRegex(ContentError, "near-silent"):
            stage_game(self.lesson_path, self.audio / "audio_map.json", self.stage)

    def test_edge_tts_game_needs_attestation_unless_smoke(self):
        script = audio_script(self.lesson)["clips"]
        entries = [{"clip_key": key, "spoken_text": text, "path": f"clips/{key}.wav",
                    "sha256": hashlib.sha256(
                        (self.audio / "clips" / f"{key}.wav").read_bytes()).hexdigest()}
                   for key, text in script.items()]
        write_json(self.audio / "audio.manifest.json", {
            "schema_version": "GAMEAUDIO1", "status": "draft", "provider": "edge-tts",
            "lesson_uid": self.lesson["lesson_uid"],
            "lesson_sha256": hashlib.sha256(self.lesson_path.read_bytes()).hexdigest(),
            "audio": entries})
        candidate = stage_game(self.lesson_path, self.audio / "audio_map.json", self.stage)

        def review(**extra):
            path = self.root / "review.json"
            attest = {k: True for k in ("educational_accuracy", "answer_uniqueness",
                                        "audio", "age_safety", "source")}
            write_json(path, {"status": "reviewed", "reviewer": "CI",
                              "attestations": {**attest, **extra},
                              "checked_hash": candidate["checked_hash"]})
            return path
        with self.assertRaisesRegex(ContentError, "voice_terms_cleared"):
            build_reviewed_game(self.stage, review(), self.root / "edge.kpg")
        build_reviewed_game(self.stage, review(), self.root / "smoke.kpg", smoke_only=True)
        result = build_reviewed_game(self.stage, review(voice_terms_cleared=True),
                                     self.root / "edge.kpg")
        meta = json.loads((self.root / "edge.manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(meta["checked_hash"], result["checked_hash"])


class SmallFixes(unittest.TestCase):
    def test_big_small_answer_position_varies_and_shape_names(self):
        lesson = original_math_lesson()
        rounds = [r for r in lesson["rounds"] if r["kind"] == "big_small"]
        positions = {[o["art"] for o in r["options"]].index(r["answer"]) for r in rounds}
        self.assertEqual(positions, {0, 1})
        labels = {o["label"] for r in lesson["rounds"] for o in r["options"]}
        self.assertTrue({"星星", "爱心"} <= labels)
        self.assertFalse({"星形", "爱心形"} & labels)


if __name__ == "__main__":
    unittest.main()
