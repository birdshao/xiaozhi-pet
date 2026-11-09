"""V9 1E (G4): spoken game rules. Wording, ids, checklist, pack catalogue.

The wording is the child-voice rewrite of docs (phrases proposal, table F):
it lives once, in PlayGamePromptText (play_content.cc); tools/game_prompts.py
reads it and gives every distinct line a KPS1 id of 600 + game id.
"""
from __future__ import annotations

import contextlib
import io
import re
import tempfile
import unittest
import wave
from pathlib import Path

from tools import game_prompts, sound_pack, v8_quiz_content

ROOT = Path(__file__).resolve().parent.parent
GAME_RULES_H = ROOT / "firmware/components/kid_app/include/kid_app/game_rules.h"
PLAY_CONTENT_CC = ROOT / "firmware/components/kid_app/src/play_content.cc"

# Reviewed wording (child voice), by growth memory id. Memory 4 and 6 (21, 22)
# say the same words: one clip, KPS1 id 621.
EXPECTED = {
    621: "找一找，哪两张是一样的？",
    623: "把团团歪一歪，帮团团找到胡萝卜吧。",
    624: "摇一摇骰子，数一数有几个点。",
    625: "摇一摇小树，数一数掉下几个果子。",
    626: "先动一动，听到“木头人”，就一动也不动。",
    627: "天黑啦，把团团扣下来睡觉；天亮啦，再翻回来。",
    628: "找一找，哪个和它一样？",
    629: "数一数，有几个？",
    631: "两排图里，有一个不一样，是第几个？",
    632: "哪一个是它的宝宝？",
    633: "大、中、小排好队，缺的是哪一个？",
    634: "接下来是哪一个？",
    635: "看上面的小图，找出符合它的那一个。",
    636: "哪一个是它的影子？",
    637: "箭头指向哪一边，就选哪一边。",
    638: "现在是这个时间，我们该做什么？",
    639: "今天天气这样，团团该穿什么？",
}
# Words a 3-8 year old does not know or that sound like a lecture.
FORBIDDEN = ("设备", "按键", "家长", "绘本", "屏幕", "拨轮", "大人", "板子", "模式", "按下")


class RuleTableTests(unittest.TestCase):
    def test_wording_is_the_reviewed_child_voice(self):
        rows = game_prompts.table()
        self.assertEqual({r.id: r.text for r in rows}, EXPECTED)

    def test_ids_follow_game_ids_in_the_free_range(self):
        rows = game_prompts.table()
        self.assertEqual(len(rows), 17)
        self.assertEqual([r.id for r in rows], sorted(r.id for r in rows))
        for r in rows:
            self.assertTrue(600 <= r.id <= 999, r)
            self.assertEqual(r.id, 600 + (21 if r.games[0] == 22 else r.games[0]))
        by_id = {r.id: r.games for r in rows}
        self.assertEqual(by_id[621], (21, 22))  # memory 4 and 6 share one clip
        games = sorted(g for r in rows for g in r.games)
        self.assertEqual(games, [21, 22, 23, 24, 25, 26, 27, 28, 29,
                                 31, 32, 33, 34, 35, 36, 37, 38, 39])

    def test_ids_match_the_firmware_header(self):
        h = GAME_RULES_H.read_text(encoding="utf-8")
        self.assertIn("kGameRuleKpsBase = 600;", h)
        self.assertEqual(game_prompts.KPS_BASE, 600)
        for name, value in (("kGameRuleFirstId", 21), ("kGameRuleLastId", 39),
                            ("kGameRuleNoId", 30), ("kGameRuleMemory4", 21),
                            ("kGameRuleMemory6", 22)):
            self.assertRegex(h, rf"{name} = {value}\b")
        # Every game of play_content.cc's table has a rule, and only those.
        ids = {int(i) for i in re.findall(r'\{PlayGame::\w+, (\d+), "', PLAY_CONTENT_CC.read_text(
            encoding="utf-8"))}
        self.assertEqual(ids, {g for r in game_prompts.table() for g in r.games})

    def test_text_is_short_and_child_friendly(self):
        for r in game_prompts.table():
            self.assertLessEqual(len(r.text), 24, r)
            for word in FORBIDDEN:
                self.assertNotIn(word, r.text, r)
            self.assertNotIn("{", r.text)

    def test_one_source_for_the_wording(self):
        # The review checklist generator and this tool read the same strings.
        spoken = {t for _, t in v8_quiz_content.play_prompts()}
        self.assertEqual(spoken, set(EXPECTED.values()))

    def test_games_without_a_rule_are_rejected(self):
        with self.assertRaises(ValueError):
            game_prompts.table(play_prompts=[("Memory4", "一"), ("Memory6", "二")])
        with self.assertRaises(ValueError):
            game_prompts.table(play_prompts=[("Nope", "一")])


class ChecklistTests(unittest.TestCase):
    def test_checklist_lists_every_clip(self):
        text = game_prompts.checklist()
        for r in game_prompts.table():
            self.assertIn(f"| {r.id}.wav |", text)
            self.assertIn(r.text, text)
        self.assertIn("成人试听", text)

    def test_cli_checklist(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(game_prompts.main(["checklist"]), 0)
        self.assertIn("621.wav", out.getvalue())


class PackCatalogueTests(unittest.TestCase):
    def test_default_pack_catalogue_has_the_rules(self):
        rules = [c for c in sound_pack.catalog() if c.kind == "game_rule"]
        self.assertEqual({c.id: c.text for c in rules}, EXPECTED)
        ids = [c.id for c in sound_pack.catalog()]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertLessEqual(len(ids), sound_pack.MAX_CLIPS)
        names = [c.name for c in sound_pack.catalog()]
        self.assertEqual(len(names), len(set(names)))
        self.assertIn("621", names)

    def test_clip_limit_leaves_room_and_matches_the_reader(self):
        # A pack over the limit is refused whole: every line would fall back
        # to a tone. Keep room for the V9 care lines merged next to this.
        header = (ROOT / "firmware/components/kid_app/include/kid_app/"
                  "sound_pack_reader.h").read_text(encoding="utf-8")
        m = re.search(r"kMaxClips = (\d+);", header)
        self.assertIsNotNone(m)
        self.assertEqual(int(m.group(1)), sound_pack.MAX_CLIPS)
        self.assertGreaterEqual(sound_pack.MAX_CLIPS, len(sound_pack.catalog()) + 64)

    def test_sound_pack_checklist_names_them(self):
        text = sound_pack.checklist()
        self.assertIn("| 621 |", text)
        self.assertIn("游戏", text)


def fake_tts(text: str, output: Path, voice: str) -> None:
    """Offline backend: a short silent 24 kHz mono PCM16 WAV."""
    assert text and voice.startswith("zh-CN-")
    output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(output), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(24000)
        w.writeframes(b"\0\0" * 4800)


class SynthTests(unittest.TestCase):
    def test_synth_writes_drafts_named_by_id_and_they_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "clips"
            m = game_prompts.synth(out, synthesize=fake_tts)
            self.assertEqual(m["status"], "draft_needs_audition")
            self.assertEqual(len(m["lines"]), 17)
            self.assertEqual({p.name for p in out.glob("*.wav")},
                             {f"{i}.wav" for i in EXPECTED})
            pack = Path(tmp) / "default.kps"
            made = sound_pack.make_pack(out, pack, reviewed=True)
            _, clips = sound_pack.decode(pack.read_bytes())
            self.assertEqual(sorted(clips), sorted(EXPECTED))
            self.assertEqual(made["missing_fall_back_to_builtin"] != [], True)

    def test_only_selects_some_clips(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "clips"
            m = game_prompts.synth(out, only={621, 639}, synthesize=fake_tts)
            self.assertEqual([x["id"] for x in m["lines"]], [621, 639])


if __name__ == "__main__":
    unittest.main()
