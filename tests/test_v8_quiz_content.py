"""V8 voice-kind question banks: 20+ rounds, valid KPG1 packs, review list in sync."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools import game_pack as gp
from tools import v8_quiz_content as qc


class QuizBanks(unittest.TestCase):
    def test_every_voice_kind_has_twenty_rounds(self) -> None:
        kinds = {}
        for make in qc.PACKS.values():
            pack = make()
            self.assertGreaterEqual(len(pack.rounds), 20)
            self.assertLessEqual(len(pack.rounds), gp.MAX_ROUNDS)
            self.assertGreaterEqual(pack.content_id, gp.MIN_CONTENT_ID)
            kinds[pack.rounds[0].kind] = len(pack.rounds)
            keys = set()
            for r in pack.rounds:
                arts = [a for a, _ in r.options]
                self.assertEqual(len(set(arts)), len(arts))
                self.assertIn(r.answer, arts)
                self.assertTrue(2 <= len(arts) <= 3)
                keys.add((r.prompt.text, tuple(arts), r.answer))
            self.assertGreaterEqual(len(keys), 20, "no copy-pasted filler rounds")
        self.assertEqual(set(kinds), {"animal_sound", "big_small", "english_word"})

    def test_content_ids_are_unique(self) -> None:
        ids = [make().content_id for make in qc.PACKS.values()]
        self.assertEqual(len(set(ids)), len(ids))

    def test_big_small_answers_match_the_question(self) -> None:
        for r in qc.bigsmall_pack().rounds:
            size = r.answer.rsplit("_", 1)[1]
            want = {"哪个大？": "large", "哪个小？": "small", "哪个最大？": "large",
                    "哪个最小？": "small", "哪个是中间的？": "medium"}[r.prompt.text]
            self.assertEqual(size, want)

    def test_packs_compile_and_verify(self) -> None:
        for name, make in qc.PACKS.items():
            pack = make()
            with tempfile.TemporaryDirectory() as tmp:
                source = qc.write_source(pack, Path(tmp) / name, "tones", approve=True)
                out = Path(tmp) / f"{name}.kpg"
                manifest = gp.make_pack(source, out, reviewed=True, placeholder=True)
                self.assertTrue(manifest["placeholder"])
                parsed = gp.parse(out.read_bytes())
                self.assertEqual(len(parsed["rounds"]), len(pack.rounds))
                self.assertLessEqual(len(manifest["clips"]), gp.MAX_CLIPS)

    def test_unreviewed_source_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = qc.write_source(qc.animal_pack(), Path(tmp) / "a", "tones", approve=False)
            with self.assertRaises(ValueError):
                gp.make_pack(source, Path(tmp) / "a.kpg", reviewed=True)

    def test_checklist_is_in_sync_and_lists_every_game_prompt(self) -> None:
        text = qc.checklist()
        committed = (qc.ROOT / "docs/V8_QUIZ_REVIEW_CHECKLIST.md").read_text(encoding="utf-8")
        self.assertEqual(committed.replace("\r\n", "\n"), text)
        games = {g for g, _ in qc.play_prompts()}
        self.assertEqual(games, {"Memory4", "Memory6", "Maze", "Dice", "Tree", "Statue",
                                 "DayNight", "Shapes", "Counting", "SpotDiff", "MamaBaby",
                                 "SizeOrder", "Pattern", "Classify", "Shadow", "LeftRight",
                                 "Clock", "Weather"})


if __name__ == "__main__":
    unittest.main()
