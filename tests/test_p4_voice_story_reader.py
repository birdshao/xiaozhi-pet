"""Voice story reader API tests, offline SQLite, no audio or child PII."""
from __future__ import annotations
from pathlib import Path
import tempfile
import unittest
from fastapi.testclient import TestClient
from services.pet_learning.api import create_app
from services.pet_learning.store import PetStore
from services.pet_learning.sample_content import BOOK

TOKEN_A="test-voice-story-reader-A-1234567890-ABCD"
TOKEN_B="test-voice-story-reader-B-1234567890-ABCD"

class StoryReaderTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.path=Path(self.temp.name)/"story.sqlite3"
        self.store=PetStore(self.path)
        self.store.enroll_device("pet-unit-A",TOKEN_A)
        self.store.enroll_device("pet-unit-B",TOKEN_B)
        self.client=TestClient(create_app(self.path))
        self.a={"Authorization":"Bearer "+TOKEN_A}
        self.b={"Authorization":"Bearer "+TOKEN_B}

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def publish(self, approved=True):
        for i,page in enumerate(BOOK["pages"],1):
            self.store.add_approved_page(
                BOOK["book_id"],BOOK["title"],i,page,approved=approved)

    def voice(self, text,headers=None):
        return self.client.post("/v1/devices/pet-unit-A/intent",
                                headers=headers or self.a,json={"text":text})

    def endpoint(self,part,headers=None):
        return self.client.post("/v1/devices/pet-unit-A/story/"+part,
                                headers=headers or self.a)

    def status(self,headers=None,device="pet-unit-A"):
        return self.client.get(f"/v1/devices/{device}/story/status",
                               headers=headers or self.a)

    def test_only_approved_pages_can_be_spoken(self):
        self.publish(approved=False)
        empty=self.voice("讲故事").json()
        self.assertEqual(empty["kind"],"fallback")
        self.assertFalse(self.status().json()["active"])
        self.publish(approved=True)
        page1=self.voice("给我讲故事").json()
        self.assertEqual(page1["story_state"],"reading")
        self.assertIn(BOOK["pages"][0],page1["tts_text"])
        self.assertIn(BOOK["title"],page1["tts_text"])
        self.assertEqual(self.status().json(),{"active":True,"page":1})
        self.assertEqual(self.voice("再读一遍").json()["page"],1)
        self.assertEqual(self.voice("继续故事").json()["page"],2)
        self.assertEqual(self.voice("下一页").json()["page"],3)
        self.assertEqual(self.voice("下一页").json()["story_state"],"finished")
        self.assertEqual(self.status().json(),{"active":False,"page":None})

    def test_restart_resumes_exact_approved_page(self):
        self.publish()
        self.endpoint("start")
        self.endpoint("next")
        restarted=TestClient(create_app(self.path))
        try:
            self.assertEqual(restarted.get("/v1/devices/pet-unit-A/story/status",
                                           headers=self.a).json()["page"],2)
            again=restarted.post("/v1/devices/pet-unit-A/story/repeat",
                                 headers=self.a).json()
            self.assertEqual(again["page"],2)
            self.assertEqual(again["tts_text"],BOOK["pages"][1])
        finally:
            restarted.close()

    def test_device_auth_and_separate_progress(self):
        self.publish()
        self.assertEqual(self.endpoint("start",headers=self.b).status_code,401)
        self.assertEqual(self.client.post("/v1/devices/pet-unit-A/story/start").status_code,401)
        self.assertEqual(self.status(headers=self.b).status_code,401)
        self.endpoint("start")
        self.endpoint("next")
        self.assertEqual(self.status().json()["page"],2)
        other=self.client.post("/v1/devices/pet-unit-B/story/start",
                               headers=self.b).json()
        self.assertEqual(other["page"],1)
        self.assertEqual(self.status(device="pet-unit-B",headers=self.b).json()["page"],1)
        self.assertEqual(self.status().json()["page"],2)

    def test_unpublished_next_page_stays_unspoken(self):
        self.publish()
        self.endpoint("start")
        self.store.add_approved_page(BOOK["book_id"],BOOK["title"],2,
                                     BOOK["pages"][1],approved=False)
        response=self.endpoint("next").json()
        self.assertEqual(response["kind"],"fallback")
        self.assertNotIn(BOOK["pages"][1],response["tts_text"])
        self.assertFalse(self.status().json()["active"])

    def test_revoked_current_page_stops_reader(self):
        self.publish()
        self.endpoint("start")
        self.store.add_approved_page(BOOK["book_id"],BOOK["title"],1,
                                     BOOK["pages"][0],approved=False)
        response=self.endpoint("repeat").json()
        self.assertEqual(response["kind"],"fallback")
        self.assertFalse(self.status().json()["active"])

    def test_quiet_mode_blocks_speech_without_advancing(self):
        self.publish()
        self.endpoint("start")
        event={"schema_version":"1.0","message_id":"pet-unit-A:1",
               "device_id":"pet-unit-A","seq":1,"type":"pet.event",
               "payload":{"event":"QUIET_HOURS_START"}}
        result=self.client.post("/v1/devices/pet-unit-A/events",
                                headers=self.a,json={"events":[event]}).json()
        self.assertTrue(result["acks"][0]["accepted"])
        self.assertEqual(self.voice("下一页").json()["kind"],"fallback")
        self.assertEqual(self.endpoint("next").json()["kind"],"fallback")
        self.assertEqual(self.status().json()["page"],1)

    def test_legacy_story_event_intent_is_unchanged(self):
        self.publish()
        event={"schema_version":"1.0","message_id":"pet-unit-A:1",
               "device_id":"pet-unit-A","seq":1,"type":"pet.event",
               "payload":{"event":"STORY_OPEN","book_id":BOOK["book_id"]}}
        r=self.client.post("/v1/devices/pet-unit-A/events",headers=self.a,
                           json={"events":[event]})
        self.assertTrue(r.json()["acks"][0]["accepted"])
        self.assertEqual(self.voice("读这一页").json()["tts_text"],BOOK["pages"][0])
        self.assertEqual(self.voice("下一页").json()["action"],"PAGE_NEXT")

    def test_fixed_TTS_limit_for_long_approved_page(self):
        self.store.add_approved_page("long_test","测试绘本",1,"小"*320,approved=True)
        response=self.endpoint("start").json()
        self.assertEqual(response["kind"],"approved_content")
        self.assertEqual(len(response["tts_text"]),320)
        self.assertEqual(response["tts_text"],"小"*320)

    def test_stop_and_unknown_asr_never_reveal_private_text(self):
        self.publish()
        self.endpoint("start")
        for question in ("我的住址是什么","你要送我红包吗"):
            reply=self.voice(question).json()
            self.assertEqual(reply["kind"],"fallback")
            self.assertNotIn(question,reply["tts_text"])
        self.assertEqual(self.voice("结束故事").json()["story_state"],"finished")
        self.assertFalse(self.status().json()["active"])
        self.assertEqual(self.client.post("/v1/devices/pet-unit-A/story/publish",
                                          headers=self.a,json={}).status_code,404)

if __name__=="__main__":
    unittest.main()
