"""LCP-04 original math game generation and reviewed KPG1 round-trip.

Synthetic tone WAVs here are test-only. They must never be represented as
approved child-facing speech or published as a normal content pack.
"""
from __future__ import annotations

import contextlib
import copy
from collections import Counter
from io import StringIO
import json
from pathlib import Path
import tempfile
import unittest
import wave

from content_pipeline.cli import main
from content_pipeline.game_lessons import (audio_key, audio_map, audio_script,
                                           original_math_lesson, validate_lesson)
from content_pipeline.game_packaging import build_reviewed_game, stage_game
from content_pipeline.schema import ContentError, write_json
from tools import game_pack as gp


class OriginalMath(unittest.TestCase):
    def setUp(self):
        self.lesson = original_math_lesson()

    def test_15_rounds_and_compatible_kpg1_art(self):
        self.assertEqual(len(self.lesson["rounds"]), 15)
        self.assertEqual(Counter(r["kind"] for r in self.lesson["rounds"]),
                         {"match_shape": 5, "count": 5, "big_small": 5})
        self.assertEqual(self.lesson["review"]["status"], "draft")
        self.assertFalse(self.lesson["source"]["copied_assets"])
        for r in self.lesson["rounds"]:
            self.assertIn(r["kind"], gp.KINDS)
            self.assertIn(r["question_art"], set(gp.ART) | set(gp.QUESTION_ONLY_ART))
            self.assertIn(r["answer"], [o["art"] for o in r["options"]])
            self.assertTrue(all(o["art"] in gp.ART for o in r["options"]))
        self.assertEqual(original_math_lesson(), self.lesson)

    def test_audio_script_is_deterministic_and_deduplicated(self):
        script = audio_script(self.lesson)
        mapping = audio_map(self.lesson)
        self.assertTrue(len(script["clips"]) <= gp.MAX_CLIPS)
        self.assertGreater(len(script["clips"]), 10)
        self.assertEqual(set(script["clips"]), set(mapping["clips"]))
        self.assertEqual(len(set(script["clips"].values())), len(script["clips"]))
        self.assertEqual(script, audio_script(original_math_lesson()))
        self.assertEqual(audio_key("大球"), audio_key("大球"))
        self.assertNotEqual(audio_key("大球"), audio_key("小球"))

    def _bad(self, modify, expected):
        doc = copy.deepcopy(self.lesson)
        modify(doc)
        with self.assertRaisesRegex(ContentError, expected):
            validate_lesson(doc)

    def test_reject_wrong_answers_and_art(self):
        self._bad(lambda d: d["rounds"][0].update(answer="square"), "shape")
        self._bad(lambda d: d["rounds"][5].update(answer="dots2"), "counting")
        self._bad(lambda d: d["rounds"][10].update(answer="ball_small"), "big/small")
        self._bad(lambda d: d["rounds"][10].update(target="small"), "big/small|size")
        self._bad(lambda d: d["rounds"][0]["options"].append(
            d["rounds"][0]["options"][0]), "invalid/repeated|2-3")
        self._bad(lambda d: d["rounds"][0]["options"][0].update(label="不对"),
                  "wrong option label")
        self._bad(lambda d: d["rounds"][0].update(kind="unsupported"), "kind")
        self._bad(lambda d: d["rounds"][0].update(question_art="dog"), "shape")
        self._bad(lambda d: d.update(review={"status": "reviewed"}), "auto-approval")
        self._bad(lambda d: d["source"].update(copied_assets=True), "provenance")

    def test_reject_bad_size_prompt(self):
        self._bad(lambda d: d["rounds"][10].update(prompt="哪个球更小？"), "size")
        self._bad(lambda d: d["rounds"][10].update(prompt="哪个球更大或更小？"),
                  "size")


class GameStage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.draft = original_math_lesson()
        self.lesson_path = self.root / "lesson.json"
        write_json(self.lesson_path, self.draft)
        self.audio = self.root / "audio"
        self.audio.mkdir()
        self.audio_map_path = self.audio / "audio_map.json"
        write_json(self.audio_map_path, audio_map(self.draft))
        for n, key in enumerate(audio_script(self.draft)["clips"]):
            # These are distinct synthetic *tones*, not child-usable narration.
            wav = self.audio / "clips" / f"{key}.wav"
            gp.write_wav(wav, gp.tone([350 + n * 13], note_s=0.10))
        self.stage = self.root / "stage"
        self.out = self.root / "export" / "default.kpg"

    def tearDown(self):
        self.tmp.cleanup()

    def review(self, hash_, *, valid=True):
        path = self.root / "parent_review.json"
        data = {
            "status": "reviewed", "reviewer": "CI synthetic fixture",
            "checked_hash": hash_,
            "attestations": {
                "educational_accuracy": True,
                "answer_uniqueness": True,
                "audio": valid,
                "age_safety": True,
                "source": True,
            }
        }
        write_json(path, data)
        return path

    def test_real_kpg1_with_synthetic_audio_is_placeholder(self):
        candidate = stage_game(self.lesson_path, self.audio_map_path, self.stage)
        unreviewed = json.loads((self.stage / "game.json").read_text(encoding="utf-8"))
        self.assertFalse(unreviewed["approved"])
        self.assertTrue(all(r["approved"] is False for r in unreviewed["rounds"]))
        with self.assertRaisesRegex(ValueError, "not approved"):
            gp.load_source(self.stage / "game.json")
        approved = self.review(candidate["checked_hash"])
        result = build_reviewed_game(self.stage, approved, self.out, smoke_only=True)
        self.assertTrue(result["placeholder"])
        self.assertEqual(result["sd_path"], "/games/default.kpg")
        self.assertEqual(result["rounds"], 15)
        package = gp.parse(self.out.read_bytes())
        self.assertEqual(len(package["rounds"]), 15)
        self.assertLessEqual(len(package["clips"]), gp.MAX_CLIPS)
        self.assertEqual(Counter(r["kind"] for r in package["rounds"]),
                         {gp.KINDS["match_shape"]: 5, gp.KINDS["count"]: 5,
                          gp.KINDS["big_small"]: 5})
        meta = json.loads(self.out.with_suffix(".manifest.json").read_text(encoding="utf-8"))
        self.assertTrue(meta["placeholder"])
        self.assertFalse((self.stage / "__reviewed_game_temp.json").exists())
        self.assertEqual(result["sha256"], build_reviewed_game(
            self.stage, approved, self.out, smoke_only=True)["sha256"])

    def test_hash_change_and_incomplete_review_fail_closed(self):
        candidate = stage_game(self.lesson_path, self.audio_map_path, self.stage)
        approved = self.review(candidate["checked_hash"], valid=False)
        with self.assertRaisesRegex(ContentError, "five explicit checks"):
            build_reviewed_game(self.stage, approved, self.out)
        approved = self.review("bad-digest")
        with self.assertRaisesRegex(ContentError, "current hash"):
            build_reviewed_game(self.stage, approved, self.out)
        approved = self.review(candidate["checked_hash"])
        first_clip = next((self.stage / "clips").glob("*.wav"))
        with first_clip.open("ab") as f:
            f.write(b"tampered")
        with self.assertRaisesRegex(ContentError, "changed"):
            build_reviewed_game(self.stage, approved, self.out)
        self.assertFalse(self.out.exists())

    def test_missing_or_invalid_recording(self):
        mapping = json.loads(self.audio_map_path.read_text(encoding="utf-8"))
        key = next(iter(mapping["clips"]))
        mapping["clips"].pop(key)
        write_json(self.audio_map_path, mapping)
        with self.assertRaisesRegex(ContentError, "clip keys"):
            stage_game(self.lesson_path, self.audio_map_path, self.stage)
        mapping = audio_map(self.draft)
        write_json(self.audio_map_path, mapping)
        key = next(iter(mapping["clips"]))
        broken = self.audio / mapping["clips"][key]
        with wave.open(str(broken), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b"\x00\x00" * 8000)
        with self.assertRaisesRegex(ValueError, "24 kHz"):
            stage_game(self.lesson_path, self.audio_map_path, self.stage)

    def test_stage_symlink_and_output_name_protection(self):
        candidate = stage_game(self.lesson_path, self.audio_map_path, self.stage)
        review = self.review(candidate["checked_hash"])
        with self.assertRaisesRegex(ContentError, "unsafe"):
            build_reviewed_game(self.stage, review, self.root / "Another.kpg")
        named = build_reviewed_game(
            self.stage, review, self.root / "another.kpg", smoke_only=True)
        self.assertEqual(named["sd_path"], "/games/another.kpg")
        self.assertEqual(len(gp.parse((self.root / "another.kpg").read_bytes())["rounds"]), 15)
        external = self.root / "outsider.wav"
        gp.write_wav(external, gp.tone([500]))
        mapping = audio_map(self.draft)
        key = next(iter(mapping["clips"]))
        link = self.audio / "outside.wav"
        try:
            link.symlink_to(external)
        except OSError as exc:
            self.skipTest(f"cannot create symlinks on this OS: {exc}")
        mapping["clips"][key] = "outside.wav"
        write_json(self.audio_map_path, mapping)
        with self.assertRaisesRegex(ContentError, "escapes"):
            stage_game(self.lesson_path, self.audio_map_path, self.stage)

    def test_cli_generate_and_validate(self):
        dest = self.root / "generated"
        with contextlib.redirect_stdout(StringIO()):
            self.assertEqual(main(["make-math-game", "--output-dir", str(dest)]), 0)
            self.assertEqual(main(["check-game", "--input", str(dest / "lesson.json")]), 0)
            self.assertEqual(main(["stage-game", "--input", str(self.lesson_path),
                                   "--audio-map", str(self.audio_map_path),
                                   "--output-dir", str(self.stage)]), 0)
        self.assertEqual(len(json.loads((dest / "audio_script.json").read_text(
            encoding="utf-8"))["clips"]), len(audio_script(self.draft)["clips"]))


if __name__ == "__main__":
    unittest.main()
