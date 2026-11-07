"""S5 voice turn: fixed commands, quiet hours, page Q&A, token scope, fallbacks."""
from __future__ import annotations

import hashlib
import hmac
import json
import logging
import threading
import sqlite3
from contextlib import closing
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

from fastapi.testclient import TestClient

from services.pet_learning.api import create_app
from services.pet_learning.config import Settings
from services.pet_learning.intent import ACTIONS, match_fixed, normalize_speech
from services.pet_learning.store import PetStore
from services.pet_learning.voice import ContextCache

DEVICE = "pet-unit-A"
DEVICE_TOKEN = "test-device-token-A-never-deploy-1234567890"
VOICE_TOKEN = "test-voice-token-never-deploy-0123456789abcdef"
VOICE = {"Authorization": "Bearer " + VOICE_TOKEN}
DEVICE_AUTH = {"Authorization": "Bearer " + DEVICE_TOKEN}

COMMANDS = {
    "SLEEP_REQUEST": ["团团睡觉", "团团，睡觉！", "我们睡觉吧", "团团睡觉吧"],
    "PAGE_NEXT": ["下一页", "翻页", "下一页吧", "翻到下一页"],
    "PAGE_PREV": ["上一页", "翻到上一页"],
    "OPEN_READER": ["讲故事", "读绘本", "讲个故事"],
    "OPEN_GAME": ["玩游戏", "玩个游戏", "我要玩游戏"],
    "PET_FEED": ["喂宠物", "喂团团", "喂食"],
    "PET_GROOM": ["给团团梳毛", "梳毛"],
    "GO_HOME": ["回家", "回首页", "回到首页"],
}
NEAR_MISSES = [
    "我不想睡觉", "团团睡觉了吗", "睡觉", "下一页是什么", "不要下一页", "喂宠物吃糖", "喂鸭子",
    "我想回家", "回家吃饭", "讲故事给我听好不好", "几点了呀你说", "还有多少钱", "玩具", "梳头",
    "喂，团团！", "喂 团团", "喂，宠物", "团团睡觉？", "团团睡觉?", "回家？", "梳毛？", "下一页？",
    "喂团团？", "next page", "ＮＥＸＴ", "", "。", "团团", "睡",
]


def at(hour, minute, tz="Asia/Shanghai"):
    from zoneinfo import ZoneInfo
    return datetime(2026, 11, 6, hour, minute, tzinfo=ZoneInfo(tz)).timestamp()


class VoiceBase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.db = str(Path(self.temp.name) / "pet.sqlite3")
        self.store = PetStore(self.db)
        self.store.enroll_device(DEVICE, DEVICE_TOKEN)
        self.store.add_approved_page("animals_001", "动物朋友", 1, "这是一只小象。", approved=True)
        self.store.add_approved_page("animals_001", "动物朋友", 2, "小象喜欢散步。", approved=True)
        self.store.add_approved_page("draft_001", "待审核", 1, "不允许播报的草稿", approved=False)
        settings = Settings(database=self.db, voice_token=VOICE_TOKEN)
        self.app = create_app(self.db, settings=settings)
        self.now = at(9, 30)
        self.mono = 1000.0
        turns = self.app.state.voice_turns
        turns.clock = lambda: self.now
        turns.contexts = ContextCache(clock=lambda: self.mono)
        self.turns = turns
        self.client = TestClient(self.app, client=("127.0.0.1", 5000))

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def turn(self, text, device=DEVICE, headers=None, **extra):
        return self.client.post(f"/v1/voice/{device}/turn", headers=VOICE if headers is None else headers,
                                json={"text": text, **extra})

    def say(self, text, **kw):
        response = self.turn(text, **kw)
        self.assertEqual(response.status_code, 200, response.text)
        data = response.json()
        self.assertEqual(set(data), {"kind", "tts_text", "action", "reason"})
        return data

    def sql(self, statement, params=()):
        with closing(sqlite3.connect(self.db)) as db:
            rows = db.execute(statement, params).fetchall()
            db.commit()
        return rows

    def set_quiet(self, quiet=True):
        self.sql("UPDATE devices SET quiet=? WHERE device_id=?", (int(quiet), DEVICE))

    def put_context(self, **body):
        body = {"scene": "READER", "page": 1, "book_id": "animals_001", **body}
        return self.client.put(f"/v1/devices/{DEVICE}/context", headers=DEVICE_AUTH, json=body)

    def add_battery(self, pct, received_at):
        self.sql("CREATE TABLE IF NOT EXISTS device_state(device_id TEXT PRIMARY KEY,"
                 "battery_pct, received_at)")
        self.sql("INSERT OR REPLACE INTO device_state VALUES(?,?,?)", (DEVICE, pct, received_at))


class FixedCommandTests(VoiceBase):
    def test_every_command_and_synonym(self):
        for action, phrases in COMMANDS.items():
            for phrase in phrases:
                data = self.say(phrase)
                self.assertEqual((data["kind"], data["action"]), ("local_command", action), phrase)
                self.assertTrue(data["tts_text"])
        self.assertEqual(set(COMMANDS), set(ACTIONS))

    def test_time_and_battery_questions_may_end_with_question_mark(self):
        self.assertEqual(self.say("几点了？")["reason"], "tell_time")
        self.assertEqual(self.say("还有多少电?")["reason"], "battery_unavailable")

    def test_required_sentences(self):
        self.assertEqual(self.say("团团睡觉")["tts_text"], "好的，我们准备睡觉啦。")
        self.assertEqual(self.say("喂团团")["tts_text"], "好呀，我们给团团喂一口。")

    def test_near_misses_never_produce_an_action(self):
        for phrase in NEAR_MISSES:
            data = self.say(phrase) if phrase.strip("。") else self.say("。")
            self.assertIsNone(data["action"], phrase)
            self.assertNotEqual(data["kind"], "local_command", phrase)

    def test_normalisation(self):
        self.assertEqual(normalize_speech(" 下 一页！？"), "下一页")
        self.assertIsNotNone(match_fixed("下一页。"))
        self.assertIsNone(match_fixed("下一页下一页"))

    def test_tell_time_uses_configured_timezone_and_digit_slots(self):
        self.assertEqual(self.say("几点了")["tts_text"], "现在是9点30分。")
        self.now = at(9, 0)
        self.assertEqual(self.say("现在几点")["tts_text"], "现在是9点整。")
        self.now = datetime(2026, 11, 6, 1, 5, tzinfo=timezone.utc).timestamp()
        self.assertEqual(self.say("几点了")["tts_text"], "现在是9点5分。")
        self.app.state.voice_turns.settings = Settings(
            database=self.db, voice_token=VOICE_TOKEN, timezone="America/New_York")
        self.assertEqual(self.say("几点了")["tts_text"], "现在是20点5分。")
        self.assertEqual(self.say("几点了")["reason"], "tell_time")

    def test_battery_fresh_stale_missing_and_invalid(self):
        self.assertEqual(self.say("还有多少电")["tts_text"], "看看屏幕右上角的电池吧。")  # no S2 table
        self.add_battery(64, int(self.now) - 30)
        data = self.say("还有多少电")
        self.assertEqual((data["kind"], data["tts_text"], data["action"]),
                         ("fixed", "还有百分之64的电。", None))
        self.add_battery(64, int(self.now) - 600)
        self.assertEqual(self.say("还有多少电")["reason"], "tell_battery")  # exactly 10 min is fresh
        self.add_battery(64, int(self.now) - 601)
        data = self.say("还有多少电")
        self.assertEqual((data["tts_text"], data["reason"]), ("看看屏幕右上角的电池吧。", "battery_stale"))
        for pct, received in ((101, int(self.now)), (-1, int(self.now)), (50, int(self.now) + 3600),
                              ("64", int(self.now)), (50, "now")):
            self.add_battery(pct, received)
            self.assertEqual(self.say("还有多少电")["tts_text"], "看看屏幕右上角的电池吧。")


class QuietHoursTests(VoiceBase):
    def test_quiet_hours_precede_commands_and_page_qa(self):
        self.put_context()
        self.set_quiet()
        for phrase in ("喂宠物", "几点了", "这一页讲了什么", "你好"):
            data = self.say(phrase)
            self.assertEqual((data["kind"], data["action"], data["reason"]),
                             ("fixed", None, "quiet_hours"), phrase)
        self.set_quiet(False)
        self.assertEqual(self.say("喂宠物")["action"], "PET_FEED")


class PageQaTests(VoiceBase):
    def test_reads_only_approved_page_from_context(self):
        self.assertEqual(self.put_context().json(), {"status": "ok", "expires_in": 120})
        data = self.say("这一页讲了什么")
        self.assertEqual((data["kind"], data["tts_text"], data["action"], data["reason"]),
                         ("approved_content", "这是一只小象。", None, "page_qa"))
        self.put_context(page=2, content_id=17, revision=2, header_crc="9a0c11e2")
        self.assertEqual(self.say("读这一页")["tts_text"], "小象喜欢散步。")

    def test_context_expires_after_120_seconds_and_is_not_persisted(self):
        self.put_context()
        self.mono += 119
        self.assertEqual(self.say("这一页是什么")["kind"], "approved_content")
        self.mono += 2
        data = self.say("这一页是什么")
        self.assertEqual((data["tts_text"], data["reason"], data["kind"]),
                         ("我们先翻到书里再问吧。", "no_page_context", "fallback"))
        self.put_context()
        raw = Path(self.db).read_bytes() + (Path(self.db + "-wal").read_bytes()
                                            if Path(self.db + "-wal").exists() else b"")
        self.assertNotIn("animals_001".encode() + b"\x00", b"")  # sanity
        with sqlite3.connect(self.db) as db:
            tables = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")]
            self.assertFalse(any("context" in t for t in tables))
        self.assertTrue(raw)

    def test_unapproved_missing_or_wrong_scene_never_speaks_page(self):
        for body in ({"book_id": "draft_001"}, {"book_id": "animals_001", "page": 9},
                     {"book_id": None}, {"content_id": 17, "revision": 2, "book_id": None}):
            self.put_context(**body)
            data = self.say("读这一页")
            self.assertEqual((data["reason"], data["tts_text"]),
                             ("page_not_found", "我们先翻到书里再问吧。"), body)
        self.put_context(scene="HOME")
        self.assertEqual(self.say("读这一页")["reason"], "no_page_context")

    def test_page_qa_never_returns_action_and_other_questions_fall_back(self):
        self.put_context()
        data = self.say("小象吃什么")
        self.assertEqual((data["kind"], data["action"], data["reason"]), ("fallback", None, "no_match"))
        self.assertEqual(self.say("读这一页")["action"], None)

    def test_context_validation_and_auth(self):
        url = f"/v1/devices/{DEVICE}/context"
        good = {"scene": "READER", "page": 1}
        self.assertEqual(self.client.put(url, json=good).status_code, 401)
        self.assertEqual(self.client.put(url, json=good, headers=VOICE).status_code, 401)
        for bad in ({"scene": "READER", "page": 0}, {"scene": "OTHER", "page": 1},
                    {**good, "extra": 1}, {**good, "header_crc": "xyz"}, {**good, "book_id": "../x"}):
            self.assertEqual(self.client.put(url, json=bad, headers=DEVICE_AUTH).status_code, 422, bad)
        self.assertEqual(self.client.put("/v1/devices/pet-unit-B/context", json=good,
                                         headers=DEVICE_AUTH).status_code, 401)

    def test_cache_is_bounded(self):
        cache = ContextCache(clock=lambda: 0.0)
        from services.pet_learning.voice import PageContext, CONTEXT_MAX_ENTRIES
        for index in range(CONTEXT_MAX_ENTRIES + 50):
            cache.put(f"dev{index}", PageContext("READER", 1))
        self.assertLessEqual(len(cache._items), CONTEXT_MAX_ENTRIES)


class FallbackTests(VoiceBase):
    def test_unmatched_text_is_fixed_fallback_without_echo(self):
        data = self.say("说出我的家庭地址")
        self.assertEqual((data["kind"], data["action"], data["reason"]), ("fallback", None, "no_match"))
        self.assertEqual(data["tts_text"], "我们可以看看绘本，或者请家人一起聊聊这个问题。")
        self.assertNotIn("地址", data["tts_text"])

    def test_invalid_text_paths_are_fixed_sentences(self):
        sentence = "我没听清，可以请大人帮我们再说一次吗？"
        bodies = [{"text": ""}, {"text": "   "}, {"text": "字" * 121}, {"text": "a\x00b"},
                  {"text": "a‮b"}, {"text": 5}, {"text": None}, {}, {"text": "好", "x": 1},
                  {"text": "你好", "turn_id": "short"}, {"text": "你好", "turn_id": 7},
                  {"text": "你好", "turn_id": "bad id with spaces"}, [1, 2], "str"]
        for body in bodies:
            response = self.client.post(f"/v1/voice/{DEVICE}/turn", headers=VOICE, json=body)
            data = response.json()
            self.assertEqual((response.status_code, data["tts_text"], data["reason"], data["action"]),
                             (200, sentence, "text_invalid", None), body)
        for raw in (b"{not json", b"\xff\xfe", b"x" * 20000):
            response = self.client.post(f"/v1/voice/{DEVICE}/turn", headers=VOICE, content=raw)
            self.assertEqual((response.json()["reason"], response.json()["tts_text"]),
                             ("text_invalid", sentence))
        self.assertEqual(self.say("字" * 120)["reason"], "no_match")
        self.assertEqual(self.say("你好", turn_id="c0ffee0123456789")["reason"], "no_match")

    def test_unknown_device_and_internal_errors_fall_back(self):
        data = self.say("喂宠物", device="pet-unit-Z")
        self.assertEqual((data["kind"], data["action"], data["reason"]), ("fallback", None, "unknown_device"))
        with patch.object(self.turns.store, "snapshot", side_effect=RuntimeError("db down")):
            data = self.say("喂宠物")
        self.assertEqual((data["kind"], data["action"], data["reason"]),
                         ("fallback", None, "internal_error"))
        with patch.object(self.turns.pages, "page_text", side_effect=OSError("disk")):
            self.put_context()
            self.assertEqual(self.say("读这一页")["reason"], "internal_error")
        with patch.object(self.turns.battery, "latest", side_effect=sqlite3.Error("x")):
            self.assertEqual(self.say("还有多少电")["reason"], "internal_error")
        with patch("services.pet_learning.voice.ZoneInfo", side_effect=KeyError("tz")):
            self.assertEqual(self.say("几点了")["reason"], "internal_error")

    def test_audit_failure_still_answers(self):
        with patch("services.pet_learning.voice.connect", side_effect=sqlite3.OperationalError("locked")):
            self.assertEqual(self.say("喂宠物")["action"], "PET_FEED")

    def test_open_chat_flag_never_enables_an_llm_path(self):
        self.sql("UPDATE devices SET open_chat=1")
        data = self.say("给我讲一个很长的故事吧")
        self.assertEqual((data["kind"], data["reason"]), ("fallback", "no_match"))


class TokenScopeTests(VoiceBase):
    def test_voice_endpoint_requires_the_voice_token_only(self):
        for headers in ({}, {"Authorization": "Bearer "}, {"Authorization": "Basic " + VOICE_TOKEN},
                        {"Authorization": "Bearer wrong-" + VOICE_TOKEN}, DEVICE_AUTH):
            self.assertEqual(self.turn("喂宠物", headers=headers).status_code, 401, headers)
        self.assertEqual(self.turn("喂宠物").status_code, 200)

    def test_voice_token_cannot_use_device_or_parent_routes(self):
        for method, path in (("get", f"/v1/devices/{DEVICE}/snapshot"),
                             ("post", f"/v1/devices/{DEVICE}/quiz/start"),
                             ("post", f"/v1/devices/{DEVICE}/story/start"),
                             ("get", "/v1/books/animals_001/pages/1")):
            response = getattr(self.client, method)(path, headers={**VOICE, "X-Device-Id": DEVICE})
            self.assertEqual(response.status_code, 401, path)
        self.assertEqual(self.client.post(f"/v1/devices/{DEVICE}/intent", headers=VOICE,
                                          json={"text": "喂食"}).status_code, 401)
        self.assertEqual(self.client.post("/v1/parent/logout", headers=VOICE).status_code, 401)

    def test_unset_voice_token_rejects_everything(self):
        app = create_app(self.db, settings=Settings(database=self.db))
        with TestClient(app) as client:
            for headers in (VOICE, DEVICE_AUTH, {"Authorization": "Bearer "}, {}):
                response = client.post(f"/v1/voice/{DEVICE}/turn", headers=headers, json={"text": "喂宠物"})
                self.assertEqual(response.status_code, 401)

    def test_proxied_requests_are_refused(self):
        self.assertEqual(self.turn("喂宠物", headers={**VOICE, "X-Forwarded-For": "1.2.3.4"}).status_code, 403)
        self.assertEqual(self.turn("喂宠物", headers={**VOICE, "Forwarded": "for=1.2.3.4"}).status_code, 403)
        proxied = Settings(database=self.db, voice_token=VOICE_TOKEN, trusted_proxy="127.0.0.1")
        app = create_app(self.db, settings=proxied)
        with TestClient(app, client=("127.0.0.1", 5000)) as client:
            response = client.post(f"/v1/voice/{DEVICE}/turn", headers=VOICE, json={"text": "喂宠物"})
            self.assertEqual(response.status_code, 403)

    def test_peer_allowlist_and_proxy_headers(self):
        self.assertEqual(self.turn("喂宠物", headers={**VOICE, "X-Real-IP": "1.2.3.4"}).status_code, 403)
        for peer, status in (("127.0.0.1", 200), ("::1", 200), ("10.0.0.5", 403), ("172.18.0.3", 403),
                             ("8.8.8.8", 403), ("not-an-ip", 403)):
            with TestClient(self.app, client=(peer, 5000)) as client:
                response = client.post(f"/v1/voice/{DEVICE}/turn", headers=VOICE, json={"text": "喂宠物"})
            self.assertEqual(response.status_code, status, peer)
        settings = Settings(database=self.db, voice_token=VOICE_TOKEN,
                            voice_allowed_peers="172.18.0.0/16, 127.0.0.1")
        app = create_app(self.db, settings=settings)
        for peer, status in (("172.18.0.3", 200), ("172.19.0.3", 403), ("127.0.0.1", 200)):
            with TestClient(app, client=(peer, 5000)) as client:
                response = client.post(f"/v1/voice/{DEVICE}/turn", headers=VOICE, json={"text": "喂宠物"})
            self.assertEqual(response.status_code, status, peer)

    def test_settings_reject_bad_peer_lists(self):
        for bad in ("", " , ", "pet-learning", "10.0.0.0/99"):
            with self.assertRaises(ValueError, msg=bad):
                Settings(voice_allowed_peers=bad).validate()

    def test_response_is_signed_over_turn_id_and_body(self):
        turn_id = "c0ffee0123456789"
        response = self.turn("喂宠物", turn_id=turn_id)
        expected = hmac.new(VOICE_TOKEN.encode(), turn_id.encode() + b"\n" + response.content,
                            hashlib.sha256).hexdigest()
        self.assertEqual(response.headers["x-voice-signature"], expected)
        self.assertEqual(json.loads(response.content)["action"], "PET_FEED")
        other = self.turn("喂宠物", turn_id="deadbeef01234567")
        self.assertNotEqual(other.headers["x-voice-signature"], response.headers["x-voice-signature"])
        # Invalid turn_id still yields a signed (fixed) reply bound to the empty id.
        bad = self.turn("喂宠物", turn_id="x")
        self.assertEqual(bad.json()["reason"], "text_invalid")
        self.assertEqual(bad.headers["x-voice-signature"], hmac.new(
            VOICE_TOKEN.encode(), b"\n" + bad.content, hashlib.sha256).hexdigest())

    def test_malformed_device_ids_never_reach_lookup_or_audit(self):
        before = self.sql("SELECT COUNT(*) FROM audit_log")
        for device in ("a", "..", "a b", "x" * 65, "-bad", "%00"):
            response = self.client.post(f"/v1/voice/{device}/turn", headers=VOICE, json={"text": "喂宠物"})
            if response.status_code == 404:
                continue
            self.assertEqual((response.json()["reason"], response.json()["action"]),
                             ("unknown_device", None), device)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM audit_log"), before)

    def test_context_cache_is_thread_safe(self):
        from services.pet_learning.voice import PageContext
        cache = ContextCache(clock=lambda: 0.0)
        errors = []

        def worker(start):
            try:
                for index in range(300):
                    cache.put(f"dev{start}-{index % 40}", PageContext("READER", 1))
                    cache.get(f"dev{start}-{index % 40}")
            except Exception as error:  # pragma: no cover - failure path
                errors.append(error)
        threads = [threading.Thread(target=worker, args=(n,)) for n in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(errors, [])
        self.assertTrue(hasattr(cache, "_lock"))

    def test_settings_reject_short_voice_token_and_bad_timezone(self):
        with self.assertRaises(ValueError):
            Settings(voice_token="short").validate()
        with self.assertRaises(ValueError):
            Settings(timezone="Mars/Olympus").validate()
        Settings(voice_token=VOICE_TOKEN).validate()


class PrivacyTests(VoiceBase):
    def test_utterance_not_logged_audited_or_stored(self):
        secret = "我家住在幸福路一百号"
        with self.assertLogs("pet_learning.voice", level=logging.DEBUG) as logs:
            self.say(secret)
            self.say("喂宠物")
        joined = "\n".join(logs.output)
        self.assertNotIn(secret, joined)
        self.assertNotIn("幸福", joined)
        self.assertIn("kind=fallback reason=no_match", joined)
        with sqlite3.connect(self.db) as db:
            rows = db.execute("SELECT actor,action,target,result,ip FROM audit_log WHERE actor='voice'").fetchall()
            dump = "\n".join(db.iterdump())
        self.assertEqual(rows, [("voice", "voice.turn", DEVICE, "fallback:no_match", None),
                                ("voice", "voice.turn", DEVICE, "local_command:fixed_command", None)])
        self.assertNotIn("幸福", dump)
        self.assertNotIn(VOICE_TOKEN, dump)
        self.assertNotIn(VOICE_TOKEN, joined)

    def test_every_turn_is_audited_including_failures(self):
        self.say("几点了")
        self.turn("", headers=VOICE)
        self.say("喂宠物", device="pet-unit-Z")
        results = [r[0] for r in self.sql("SELECT result FROM audit_log WHERE action='voice.turn'")]
        self.assertEqual(results, ["fixed:tell_time", "fallback:text_invalid", "fallback:unknown_device"])

    def test_turn_does_not_change_pet_projection(self):
        before = self.store.snapshot(DEVICE)
        for phrase in ("喂宠物", "下一页", "讲故事", "读这一页"):
            self.say(phrase)
        self.assertEqual(self.store.snapshot(DEVICE), before)


class LegacyEndpointTests(VoiceBase):
    def test_legacy_intent_endpoint_still_works_for_devices(self):
        response = self.client.post(f"/v1/devices/{DEVICE}/intent", headers=DEVICE_AUTH, json={"text": "喂食"})
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["action"], "PET_FEED")


if __name__ == "__main__":
    unittest.main()
