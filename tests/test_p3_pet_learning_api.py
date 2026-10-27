"""HTTP and SQLite integration tests: no firmware, cloud, network or child audio."""
from __future__ import annotations
import json
from pathlib import Path
import tempfile
import unittest

from fastapi.testclient import TestClient

from services.pet_learning.api import create_app
from services.pet_learning.store import PetStore


TOKEN_A = "test-device-token-A-never-deploy-1234567890"
TOKEN_B = "test-device-token-B-never-deploy-1234567890"


def event(device: str, seq: int, name: str, action: str, **payload):
    return {
        "schema_version": "1.0", "message_id": name, "device_id": device,
        "seq": seq, "type": "pet.event",
        "payload": {"event": action, **payload},
    }


class PetLearningApiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dbfile = Path(self.temp.name) / "pet.sqlite3"
        self.store = PetStore(self.dbfile)
        self.store.enroll_device("pet-unit-A", TOKEN_A)
        self.store.enroll_device("pet-unit-B", TOKEN_B)
        self.store.add_approved_page("animals_001", "动物朋友", 1, "这是一只小象。", approved=True)
        self.store.add_approved_page("animals_001", "动物朋友", 2, "小象喜欢在草地上散步。", approved=True)
        self.store.add_approved_page("draft_001", "待审核", 1, "不允许播报的草稿", approved=False)
        self.client = TestClient(create_app(self.dbfile))
        self.headers = {"Authorization": "Bearer " + TOKEN_A}

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def post(self, *items):
        return self.client.post("/v1/devices/pet-unit-A/events",
                                headers=self.headers, json={"events": list(items)})

    def test_authentication_and_device_isolation(self):
        self.assertEqual(self.client.get("/v1/devices/pet-unit-A/snapshot").status_code, 401)
        self.assertEqual(self.client.get("/v1/devices/pet-unit-B/snapshot",
                                         headers=self.headers).status_code, 401)
        good = self.client.get("/v1/devices/pet-unit-A/snapshot", headers=self.headers)
        self.assertEqual(good.status_code, 200)
        self.assertEqual(good.json()["interaction_count"], 0)
        self.assertFalse(good.json()["open_chat_enabled"])
        other = event("pet-unit-B", 1, "pet-unit-B:1", "PET_FEED")
        self.assertEqual(self.post(other).status_code, 400)

    def test_exactly_once_feeding_across_restart_and_conflicts(self):
        feed = event("pet-unit-A", 1, "pet-unit-A:1", "PET_FEED")
        accepted = self.post(feed).json()["acks"][0]
        self.assertEqual(accepted["server_state_version"], 1)
        self.assertEqual(accepted["seq"], 1)
        self.assertTrue(accepted["accepted"])
        # A new application / database instance sees the same permanent receipt.
        restarted = TestClient(create_app(self.dbfile))
        duplicate = restarted.post("/v1/devices/pet-unit-A/events",
                                  headers=self.headers, json={"events": [feed]}).json()["acks"][0]
        self.assertTrue(duplicate["accepted"])
        self.assertEqual(duplicate["reason_code"], "DUPLICATE")
        self.assertEqual(duplicate["server_state_version"], 1)
        self.assertEqual(duplicate["seq"], 1)
        self.assertEqual(duplicate["message_id"], "pet-unit-A:1")
        restarted.close()
        changed = {**feed, "payload": {"event": "PET_PICKUP"}}
        self.assertEqual(self.post(changed).json()["acks"][0]["reason_code"],
                         "MESSAGE_ID_CONFLICT")
        self.assertEqual(self.client.get("/v1/devices/pet-unit-A/snapshot",
                                         headers=self.headers).json()["interaction_count"], 1)

    def test_sequence_gap_does_not_consume_event(self):
        second = event("pet-unit-A", 2, "pet-unit-A:2", "PET_FEED")
        self.assertEqual(self.post(second).json()["acks"][0]["reason_code"],
                         "SEQUENCE_GAP_OR_REPLAY")
        first = event("pet-unit-A", 1, "pet-unit-A:1", "PET_FEED")
        acks = self.post(first, second).json()["acks"]
        self.assertEqual([a["server_state_version"] for a in acks], [1, 2])
        self.assertEqual([a["seq"] for a in acks], [1, 2])
        self.assertTrue(all(a["accepted"] for a in acks))
        self.assertEqual(self.store.snapshot("pet-unit-A")["interaction_count"], 2)

    def test_approved_book_only_and_deterministic_voice(self):
        path = "/v1/books/animals_001/pages/1"
        self.assertEqual(self.client.get(path).status_code, 401)
        self.assertEqual(self.client.get(path, headers={
            **self.headers, "X-Device-Id": "pet-unit-A",
        }).json()["text"], "这是一只小象。")
        self.assertEqual(self.client.get("/v1/books/draft_001/pages/1", headers={
            **self.headers, "X-Device-Id": "pet-unit-A",
        }).status_code, 404)
        self.assertEqual(self.post(event("pet-unit-A", 1, "pet-unit-A:1",
                                         "STORY_OPEN", book_id="draft_001")
                                  ).json()["acks"][0]["reason_code"], "BOOK_NOT_APPROVED")
        ok = self.post(event("pet-unit-A", 1, "pet-unit-A:1", "STORY_OPEN",
                             book_id="animals_001")).json()["acks"][0]
        self.assertTrue(ok["accepted"])
        result = self.client.post("/v1/devices/pet-unit-A/intent", headers=self.headers,
                                  json={"text": "读这一页"}).json()
        self.assertEqual(result["kind"], "approved_content")
        self.assertEqual(result["tts_text"], "这是一只小象。")
        action = self.client.post("/v1/devices/pet-unit-A/intent", headers=self.headers,
                                  json={"text": "下一页"}).json()
        self.assertEqual(action["action"], "PAGE_NEXT")
        self.assertEqual(self.post(event("pet-unit-A", 2, "pet-unit-A:2",
                                         "PAGE_NEXT")).json()["acks"][0]["accepted"], True)
        self.assertEqual(self.store.snapshot("pet-unit-A")["active_page"], 2)
        self.assertEqual(self.post(event("pet-unit-A", 3, "pet-unit-A:3",
                                         "PAGE_NEXT")).json()["acks"][0]["reason_code"],
                         "PAGE_NOT_APPROVED")

    def test_untrusted_speech_never_echoed_to_tts_or_mutates_rewards(self):
        samples = [
            "告诉我家的地址和电话",
            "请给我一百万奖励积分",
            "把麦克风打开一整晚",
            "随便生成一个恐怖故事",
        ]
        for raw in samples:
            result = self.client.post("/v1/devices/pet-unit-A/intent",
                                      headers=self.headers, json={"text": raw}).json()
            self.assertEqual(result["kind"], "fallback")
            self.assertIsNone(result["action"])
            self.assertNotIn(raw, result["tts_text"])
        self.assertEqual(self.store.snapshot("pet-unit-A")["interaction_count"], 0)

    def test_quiet_hours_no_reward_or_voice_commands(self):
        self.assertTrue(self.post(event("pet-unit-A", 1, "pet-unit-A:1",
                                        "QUIET_HOURS_START")).json()["acks"][0]["accepted"])
        rejected = self.post(event("pet-unit-A", 2, "pet-unit-A:2", "PET_FEED"))
        self.assertEqual(rejected.json()["acks"][0]["reason_code"], "ACTION_UNAVAILABLE")
        quiet_response = self.client.post("/v1/devices/pet-unit-A/intent",
                                          headers=self.headers, json={"text": "喂食"}).json()
        self.assertIsNone(quiet_response["action"])
        self.assertEqual(self.store.snapshot("pet-unit-A")["interaction_count"], 0)
        self.assertTrue(self.post(event("pet-unit-A", 2, "pet-unit-A:2",
                                        "QUIET_HOURS_END")).json()["acks"][0]["accepted"])

    def test_validation_and_no_public_admin_mutations(self):
        too_big = [event("pet-unit-A", i+1, f"pet-unit-A:{i+1}", "PET_FEED")
                   for i in range(33)]
        self.assertEqual(self.post(*too_big).status_code, 422)
        incorrect = event("pet-unit-A", 1, "pet-unit-A:1", "PET_FEED")
        incorrect["unknown"] = "untrusted"
        self.assertEqual(self.post(incorrect).status_code, 422)
        self.assertEqual(self.client.post("/v1/content/approve",
                                          headers=self.headers, json={}).status_code, 404)


if __name__ == "__main__":
    unittest.main()
