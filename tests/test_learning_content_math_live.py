"""Math learning pilot: 15 rounds and complete reviewed-audio staging.

Fixtures deliberately contain tones or mocked online TTS. No real child-facing
speech is uploaded, no adult approval is fabricated, no .kpg is published.
"""
from __future__ import annotations

import contextlib
from io import StringIO
import json
from pathlib import Path
import tempfile
import unittest
import wave
from unittest.mock import patch

from content_pipeline.cli import main
from content_pipeline.audio_qc import analyze_wav
from content_pipeline.game_packaging import _manifest
from content_pipeline.game_review import make_game_review_packet
from content_pipeline.schema import ContentError, write_json
from tools.learning_live_math_pilot import build_math_research, _tone_fixture


class MathLivePilot(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_offline_15_round_review_and_no_automatic_approval(self):
        result = build_math_research(self.root / "smoke", narration="synthetic")
        self.assertEqual(result["status"], "test_only")
        self.assertEqual(result["rounds"], 15)
        self.assertGreater(result["unique_audio_clips"], 10)
        self.assertGreater(result["total_audio_ms"], 1000)
        stage = self.root / "smoke" / "staged"
        staged_game = json.loads((stage / "game.json").read_text(encoding="utf-8"))
        self.assertIs(staged_game["approved"], False)
        self.assertTrue(all(r["approved"] is False for r in staged_game["rounds"]))
        self.assertFalse(list((self.root / "smoke").rglob("*.kpg")))
        report = json.loads((stage / "review/review-summary.json").read_text(encoding="utf-8"))
        self.assertEqual(len(report["rounds"]), 15)
        self.assertEqual(report["unique_audio_clips"], result["unique_audio_clips"])
        self.assertEqual(report["checked_hash"], result["candidate_sha256"])
        html = (stage / "review/index.html").read_text(encoding="utf-8")
        self.assertIn("NOT APPROVED", html)
        self.assertIn("第 15 题", html)
        self.assertIn("正确选项（仅审核者可见）", html)
        self.assertIn("并非固件内置 GameArt 的实际像素", html)
        self.assertIn("../clips/clip_", html)
        self.assertTrue((self.root / "smoke/README-RESEARCH-ONLY.txt").exists())

    def test_staged_correct_answer_edit_rejected(self):
        build_math_research(self.root / "smoke")
        stage = self.root / "smoke/staged"
        game_file = stage / "game.json"
        book = json.loads(game_file.read_text(encoding="utf-8"))
        book["rounds"][0]["answer"] = "square"
        write_json(game_file, book)
        with self.assertRaisesRegex(ContentError, "staged round 1 differs"):
            _manifest(stage)
        with self.assertRaisesRegex(ContentError, "round 1 differs"):
            make_game_review_packet(stage)

    def test_staged_round_label_and_voice_edits_rejected(self):
        build_math_research(self.root / "smoke")
        stage = self.root / "smoke/staged"
        game_file = stage / "game.json"
        doc = json.loads(game_file.read_text(encoding="utf-8"))
        doc["rounds"][0]["options"][0]["label"] = "错误名称"
        write_json(game_file, doc)
        with self.assertRaisesRegex(ContentError, "round 1 differs"):
            _manifest(stage)

    def test_missing_wav_or_changed_voice_rejected(self):
        build_math_research(self.root / "smoke")
        stage = self.root / "smoke/staged"
        wav = next((stage / "clips").glob("*.wav"))
        with wav.open("ab") as out:
            out.write(b"tampered")
        with self.assertRaisesRegex(ContentError, "changed"):
            make_game_review_packet(stage)
        with self.assertRaisesRegex(ContentError, "output directory is not empty"):
            build_math_research(self.root / "smoke")

    def test_edge_path_with_mock_uses_draft_and_not_test_only(self):
        """Check edge workflow data flow without calling any external provider."""
        from content_pipeline import speech
        with patch.object(speech, "_edge_backend", side_effect=_tone_fixture):
            result = build_math_research(self.root / "mock_online", narration="edge")
        self.assertEqual(result["status"], "unreviewed")
        self.assertEqual(result["speech_engine"], "edge-tts")
        self.assertEqual(result["rounds"], 15)
        with (self.root / "mock_online/staged/review/index.html").open(
                encoding="utf-8") as f:
            page = f.read()
        self.assertIn("尚未经真人完整试听", page)
        self.assertNotIn("这些音频是合成测试音调", page)
        self.assertFalse(list((self.root / "mock_online").rglob("*.kpg")))

    def test_audio_qc_rejects_silence_and_warns_on_clipping(self):
        with self.assertRaisesRegex(ContentError, "all-silent"):
            path = self.root / "silence.wav"
            with wave.open(str(path), "wb") as out:
                out.setnchannels(1)
                out.setsampwidth(2)
                out.setframerate(24000)
                out.writeframes(b"\\x00\\x00" * 2400)
            analyze_wav(path, max_seconds=10)

        clipped = self.root / "clipped.wav"
        with wave.open(str(clipped), "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(24000)
            out.writeframes(b"\\xff\\x7f" * 2400)
        report = analyze_wav(clipped, max_seconds=10)
        self.assertIn("possible_clipping", report["warnings"])
        self.assertIn("dc_offset", report["warnings"])
        self.assertEqual(report["duration_ms"], 100)

    def test_silent_clip_in_real_game_draft_never_reaches_review(self):
        build_math_research(self.root / "smoke")
        stage = self.root / "smoke/staged"
        silent = next((stage / "clips").glob("*.wav"))
        with wave.open(str(silent), "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(24000)
            out.writeframes(b"\\x00\\x00" * 4800)
        with self.assertRaisesRegex(ContentError, "all-silent"):
            make_game_review_packet(stage)

    def test_cli_review_game_generated_from_synthetic_assets(self):
        """The CLI must only render a review file; never approve staged game."""
        build_math_research(self.root / "smoke")
        review = self.root / "smoke/staged/review/index.html"
        review.unlink()
        with contextlib.redirect_stdout(StringIO()):
            result = main([
                "make-game-review-packet", "--stage-dir",
                str(self.root / "smoke/staged"),
            ])
        self.assertEqual(result, 0)
        self.assertTrue(review.is_file())


if __name__ == "__main__":
    unittest.main()
