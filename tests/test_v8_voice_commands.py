"""V8-03 fixed voice commands: ask needs, game, song, good night, joke, brush, wash.

The matcher only proposes actions; quiet hours still precede every command and
no step uses a model. The joke library is fixed, reviewed text.
"""
from __future__ import annotations

import re
import unittest

from services.pet_learning.intent import ACTIONS, NEUTRAL, match_fixed
from services.pet_learning.jokes import JOKES, MAX_CHARS, REVIEW_ID
from test_v6_voice_turn import VoiceBase

NEW = {
    "ASK_NEEDS": ["团团饿了吗", "团团饿了吗？", "团团饿了吗?", "团团，饿不饿", "团团饿了没有"],
    "OPEN_GAME": ["玩游戏", "我要玩游戏"],
    "PLAY_SONG": ["唱首歌", "唱首歌吧", "唱歌", "团团，唱首歌！"],
    "SLEEP_REQUEST": ["晚安", "团团晚安", "晚安团团", "晚安！"],
    "TELL_JOKE": ["讲个笑话", "讲笑话", "团团讲个笑话", "说个笑话！"],
    "START_BRUSH": ["刷牙", "刷牙吧", "该刷牙了", "我们刷牙"],
    "START_WASH": ["洗手", "洗手吧", "去洗手", "团团洗手"],
}
NEAR = [
    "我不想刷牙", "刷牙了吗", "刷牙？", "洗手？", "洗手间在哪", "晚安？", "唱歌好听吗", "讲笑话给我听好不好",
    "笑话", "饿", "团团饿", "我饿了", "歌", "游戏", "洗", "刷", "洗手了没有呀", "晚安曲",
]


class FixedCommandMatching(unittest.TestCase):
    def test_every_new_phrase_maps_to_its_action(self):
        for action, phrases in NEW.items():
            self.assertIn(action, ACTIONS)
            for phrase in phrases:
                match = match_fixed(phrase)
                self.assertIsNotNone(match, phrase)
                self.assertEqual((match.kind, match.action), ("local_command", action), phrase)

    def test_near_misses_do_not_act(self):
        for phrase in NEAR:
            match = match_fixed(phrase)
            self.assertTrue(match is None or match.action is None, phrase)

    def test_only_the_hunger_question_may_be_a_question(self):
        for phrase in ("刷牙？", "洗手?", "唱首歌？", "讲个笑话？", "晚安？", "玩游戏？"):
            self.assertIsNone(match_fixed(phrase), phrase)
        self.assertEqual(match_fixed("团团饿了吗？").action, "ASK_NEEDS")


class JokeLibrary(unittest.TestCase):
    def test_library_is_large_unique_short_and_tts_friendly(self):
        self.assertGreaterEqual(len(JOKES), 30)
        self.assertEqual(len(set(JOKES)), len(JOKES))
        self.assertTrue(REVIEW_ID)
        for joke in JOKES:
            self.assertLessEqual(len(joke), MAX_CHARS, joke)
            self.assertIsNone(re.search(r"[0-9A-Za-z]", joke), joke)  # TTS reads these badly
            self.assertTrue(joke.endswith(("。", "！", "？")), joke)

    def test_no_scary_or_unkind_words(self):
        banned = ("死", "杀", "血", "鬼", "怪物", "打", "笨", "傻", "蠢", "丑", "胖", "哭", "坏蛋", "枪", "恨", "偷")
        for joke in JOKES:
            for word in banned:
                self.assertNotIn(word, joke, joke)


class VoiceTurnNewCommands(VoiceBase):
    def test_actions_and_replies(self):
        for action, phrases in NEW.items():
            for phrase in phrases:
                data = self.say(phrase)
                self.assertEqual((data["kind"], data["action"]), ("local_command", action), phrase)
                self.assertTrue(data["tts_text"], phrase)
        self.assertEqual(self.say("团团饿了吗")["tts_text"], "我来看看团团的小肚子。")
        self.assertEqual(self.say("晚安")["tts_text"], "晚安，做个好梦。")

    def test_joke_is_from_the_library_and_deterministic_for_a_turn_id(self):
        first = self.say("讲个笑话", turn_id="turn-0001-aaaa")["tts_text"]
        self.assertIn(first, JOKES)
        self.assertEqual(self.say("讲笑话", turn_id="turn-0001-aaaa")["tts_text"], first)
        seen = {self.say("讲个笑话", turn_id=f"turn-{n:04d}-bbbb")["tts_text"] for n in range(60)}
        self.assertTrue(seen <= set(JOKES))
        self.assertGreater(len(seen), 10)

    def test_joke_without_turn_id_follows_the_clock(self):
        self.now = 1000.0
        self.turns.clock = lambda: self.now
        self.assertEqual(self.say("讲个笑话")["tts_text"], JOKES[1000 % len(JOKES)])

    def test_quiet_hours_precede_new_commands(self):
        self.set_quiet()
        for phrase in ("讲个笑话", "刷牙", "洗手", "团团饿了吗", "玩游戏", "喂团团"):
            data = self.say(phrase)
            self.assertEqual((data["kind"], data["action"], data["reason"]),
                             ("fixed", None, "quiet_hours"), phrase)

    def test_good_night_is_welcome_in_quiet_hours_and_is_the_one_sleep_action(self):
        self.set_quiet()
        for phrase in ("晚安", "团团晚安", "团团睡觉"):
            data = self.say(phrase)
            self.assertEqual((data["kind"], data["action"]), ("local_command", "SLEEP_REQUEST"), phrase)
        self.assertEqual(self.say("晚安")["tts_text"], "晚安，做个好梦。")
        self.assertNotIn("GOOD_NIGHT", ACTIONS)
        # "唱首歌" at night opens the lullaby-only player; the device filters.
        data = self.say("唱首歌")
        self.assertEqual((data["kind"], data["action"]), ("local_command", "PLAY_SONG"))

    def test_refusable_activities_get_a_neutral_confirmation(self):
        # The device may refuse (low battery, a book is open): the server never promises.
        for phrase in ("刷牙", "洗手", "唱首歌"):
            text = self.say(phrase)["tts_text"]
            self.assertEqual(text, NEUTRAL, phrase)
            self.assertNotIn("好呀", text)

    def test_commands_do_not_change_the_pet_or_memory(self):
        before = self.sql("SELECT * FROM audit_log")
        for phrase in ("刷牙", "洗手", "唱首歌", "晚安"):
            self.say(phrase)
        after = self.sql("SELECT action, result FROM audit_log")
        self.assertEqual(len(after), len(before) + 4)
        self.assertTrue(all(row[0] == "voice.turn" for row in after[len(before):]))


if __name__ == "__main__":
    unittest.main()
