"""V9 1E (G4): board wiring of the spoken game rules (scripts/p1_integrate.py).

The behaviour of the sessions and the id mapping are host-tested in
tests/cpp/test_game_rule_clips.cc and test_game_rules.cc. These checks keep
the glue honest: a recorded rule plays through the sound bank like a pet line
(same volume, same arbitration), a missing recording falls back to the old
prompt tone, and the self-test scenario stays inside the board parser's
80-character line limit.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INTEGRATE = (ROOT / "scripts/p1_integrate.py").read_text(encoding="utf-8")
SCENARIO = ROOT / "tools/selftest/v9_game_rules.pettest"


def prompt_task() -> str:
    start = INTEGRATE.index("void PromptTaskLoop()")
    return INTEGRATE[start:INTEGRATE.index("prompt_queue_ = xQueueCreate", start)]


class PromptTaskWiring(unittest.TestCase):
    def test_header_is_included(self):
        self.assertIn('#include "kid_app/game_rules.h"', INTEGRATE)

    def test_rule_is_looked_up_in_the_sound_pack_only(self):
        body = prompt_task()
        self.assertIn("kid_app::IsGameRuleClip(request.game)", body)
        self.assertIn("sound_bank_.LoadGameRule(kid_app::GameRuleGameId(request.game)", body)
        # A recording must really come from the pack: no chime stands in for it.
        self.assertNotIn("LoadPhrase(kid_app::GameRule", body)

    def test_missing_recording_falls_back_to_the_prompt_tone(self):
        body = prompt_task()
        self.assertRegex(body, r"if \(rule_asked && !rule_recorded\) request\.game = "
                               r"kid_app::kGameTonePrompt;")
        # A tone request never reaches the sound pack branch.
        self.assertRegex(body, r"const bool game = request\.game != 0 && !rule_recorded;")

    def test_rule_is_logged_for_the_self_test(self):
        body = prompt_task()
        # The scenario reader matches space-free words, so the source is in the tag.
        self.assertIn('"PET_GAME_RULE_PACK game=%u token=%u"', body)
        self.assertIn('"PET_GAME_RULE_TONE game=%u token=%u"', body)

    def test_rule_clip_streams_through_the_normal_player(self):
        body = prompt_task()
        # Same SoundClip.Copy path, same PromptVolumePct, same session family.
        self.assertIn("else if (rule_recorded) clip = rule_clip;", body)
        self.assertIn("(game || rule_recorded)", body)
        self.assertRegex(body, r"const bool care = !phrase && !game && !rule_recorded &&")

    def test_games_still_post_clips_through_post_game_clip(self):
        # The rule is an ordinary game clip id: PostGameClip keeps every guard
        # (Pet mode, sleep, parent menu) and no second posting path exists.
        self.assertNotIn("PostGameRule", INTEGRATE)
        self.assertIn("void PostGameClip(uint16_t clip)", INTEGRATE)


class Scenario(unittest.TestCase):
    def test_lines_fit_the_board_parser(self):
        text = SCENARIO.read_text(encoding="utf-8")
        self.assertTrue(text.startswith("PETSELFTEST1\n"))
        for n, line in enumerate(text.split("\n"), 1):
            self.assertLessEqual(len(line.encode("utf-8")), 80, f"line {n}: {line!r}")

    def test_expectations(self):
        text = SCENARIO.read_text(encoding="utf-8")
        # The rule is asked twice (open + 12 s of silence); no recording on
        # the card, so both fall back to the prompt tone.
        self.assertIn("# expect min:PET_GAME_RULE_TONE 2", text)
        self.assertIn("# expect none:PET_GAME_RULE_PACK 0", text)
        self.assertIn("# expect min:game=memory4 1", text)
        self.assertIn("# expect none:PET_GAME_START_FAILED 0", text)
        self.assertIn("# expect none:PET_GAME_REFUSED 0", text)
        self.assertIn("# expect selftest_done 1", text)
        self.assertNotIn("timescale", text)  # real time: the 12 s repeat is really 12 s
        self.assertRegex(text, r"(?m)^wait 13000$")


if __name__ == "__main__":
    unittest.main()
