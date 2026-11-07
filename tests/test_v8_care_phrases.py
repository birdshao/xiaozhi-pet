"""V8 care lines: the review checklist and TTS drafts come from care_phrases.cc."""
from __future__ import annotations

import json
import re
import tempfile
import unittest
import wave
from pathlib import Path

from tools import care_phrases, sound_pack

ROOT = Path(__file__).resolve().parent.parent


def fake_tts(text: str, output: Path, voice: str) -> None:
    """Offline test backend: a short silent 24 kHz mono PCM16 WAV."""
    assert text and voice.startswith("zh-CN-")
    output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(output), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(24000)
        w.writeframes(b"\0\0" * 4800)


class CarePhraseTableTests(unittest.TestCase):
    def test_table_matches_the_firmware_source(self):
        rows = care_phrases.table()
        source = care_phrases.CARE_CC.read_text(encoding="utf-8")
        self.assertEqual(len(rows), len(re.findall(r"^\s*\{1\d{3}, L::", source, re.M)))
        self.assertEqual(len({r.id for r in rows}), len(rows))
        for r in rows:
            self.assertTrue(1000 <= r.id <= 1999 and r.text, r)
            self.assertIn(r.line, care_phrases.WHEN)
        # Every temperament has its own diary opening / closing.
        for line in ("DiaryOpen", "DiaryClose"):
            self.assertEqual({r.temperament for r in rows if r.line == line},
                             {"curious", "gentle", "lively"})

    def test_checklist_lists_every_line_once(self):
        text = care_phrases.checklist()
        for r in care_phrases.table():
            self.assertEqual(text.count(f"| {r.id}.wav |"), 1)
        self.assertIn("真人录音", text)  # bedtime lines want a real voice

    def test_sound_pack_catalog_includes_care_lines(self):
        ids = {c.id for c in sound_pack.catalog()}
        self.assertTrue({r.id for r in care_phrases.table()} <= ids)
        self.assertLessEqual(len(ids), sound_pack.MAX_CLIPS)

    def test_synth_drafts_with_the_pipeline_backend(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "care"
            manifest = care_phrases.synth(out, only={1001, 1151}, synthesize=fake_tts)
            self.assertEqual([x["id"] for x in manifest["lines"]], [1001, 1151])
            self.assertEqual(manifest["status"], "draft_needs_audition")
            self.assertTrue((out / "1001.wav").is_file() and (out / "1151.wav").is_file())
            saved = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
            self.assertTrue(saved["lines"][0]["human_recording_wanted"])
            # No silent overwrite of an existing draft.
            with self.assertRaises(Exception):
                care_phrases.synth(out, only={1001}, synthesize=fake_tts)
            care_phrases.synth(out, only={1001}, synthesize=fake_tts, replace=True)
            # The drafts pack like any other clip (file stem = id).
            pack = Path(tmp) / "default.kps"
            sound_pack.make_pack(out, pack, reviewed=True)
            self.assertEqual(set(sound_pack.decode(pack.read_bytes())[1]), {1001, 1151})


if __name__ == "__main__":
    unittest.main()
