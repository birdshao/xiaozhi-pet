"""V9 slice 1C care lines: drafts flagged for adult audition, every new line
has a review note, and the sound pack still fits."""
from __future__ import annotations

import tempfile
import unittest
import wave
from pathlib import Path

from tools import care_phrases, sound_pack


def fake_tts(text: str, output: Path, voice: str) -> None:
    assert text and voice.startswith("zh-CN-")
    output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(output), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(24000)
        w.writeframes(b"\0\0" * 4800)


class V9CareLineTests(unittest.TestCase):
    def test_v9_lines_are_drafts_with_a_note(self):
        rows = care_phrases.table()
        drafts = [r for r in rows if care_phrases.is_draft(r)]
        lines = {r.line for r in drafts}
        for line in ("TickleSeek", "TickleLaugh", "DiaryGameNamed", "BoredGone", "SongAsk",
                     "GrowMorning", "GrowDiary", "BathDone", "BrushDone", "TickleDone"):
            self.assertIn(line, lines)
        # 3 actions x 3 temperaments x 3 cheers.
        cheers = [r for r in drafts if r.line in ("BathDone", "BrushDone", "TickleDone")]
        self.assertEqual(len(cheers), 27)
        self.assertEqual({(r.line, r.temperament) for r in cheers},
                         {(l, t) for l in ("BathDone", "BrushDone", "TickleDone")
                          for t in ("curious", "gentle", "lively")})
        # V8 lines keep their status.
        self.assertFalse(any(care_phrases.is_draft(r) for r in rows if r.id < 1300))

    def test_checklist_marks_drafts(self):
        text = care_phrases.checklist()
        for r in care_phrases.table():
            row = next(l for l in text.splitlines() if l.startswith(f"| {r.id}.wav |"))
            self.assertEqual("V9 TTS 草稿，待成人试听" in row, care_phrases.is_draft(r), row)

    def test_sound_pack_still_fits(self):
        ids = {c.id for c in sound_pack.catalog()}
        self.assertTrue({r.id for r in care_phrases.table()} <= ids)
        self.assertLessEqual(len(ids), sound_pack.MAX_CLIPS)

    def test_manifest_flags_drafts(self):
        with tempfile.TemporaryDirectory() as tmp:
            m = care_phrases.synth(Path(tmp), only={1151, 1331}, synthesize=fake_tts)
            flags = {x["id"]: x["v9_draft"] for x in m["lines"]}
            self.assertEqual(flags, {1151: False, 1331: True})


if __name__ == "__main__":
    unittest.main()
