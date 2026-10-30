"""Parent-reviewed learning games: FastAPI/SQLite/voice integration, no hardware."""
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from fastapi.testclient import TestClient

from services.pet_learning.api import create_app
from services.pet_learning.quiz import QuizEngine
from services.pet_learning.store import PetStore
from services.pet_learning.sample_content import QUIZZES, BOOK

TOKEN_A = "test-animal-quiz-A-never-deploy-1234567890"
TOKEN_B = "test-animal-quiz-B-never-deploy-1234567890"


class QuizIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.path=Path(self.temp.name)/"pet.sqlite3"
        self.store=PetStore(self.path)
        self.store.enroll_device("pet-unit-A", TOKEN_A)
        self.store.enroll_device("pet-unit-B", TOKEN_B)
        self.quiz=QuizEngine(self.path)
        self.client=TestClient(create_app(self.path))
        self.a={"Authorization": "Bearer "+TOKEN_A}
        self.b={"Authorization": "Bearer "+TOKEN_B}

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def intent(self, text, *, device="pet-unit-A", headers=None):
        return self.client.post(
            f"/v1/devices/{device}/intent", headers=self.a if headers is None else headers,
            json={"text":text})

    def start(self):
        return self.client.post("/v1/devices/pet-unit-A/quiz/start", headers=self.a)

    def answer(self, choice):
        return self.client.post("/v1/devices/pet-unit-A/quiz/answer",
                                headers=self.a,json={"choice":choice})

    def test_draft_not_visible_and_device_auth(self):
        self.quiz.import_card(*QUIZZES[0], approved=False)
        self.assertEqual(self.start().json()["kind"], "fallback")
        self.assertEqual(self.client.get("/v1/devices/pet-unit-A/quiz/status").status_code,401)
        self.assertEqual(self.client.get("/v1/devices/pet-unit-B/quiz/status",
                                         headers=self.a).status_code,401)
        self.assertEqual(self.client.post("/v1/devices/pet-unit-A/quiz/start").status_code,401)
        self.assertEqual(self.client.post("/v1/devices/pet-unit-A/quiz/answer",
                                         headers=self.a,json={"choice":3}).status_code,422)
        self.assertEqual(self.client.post("/v1/quiz/publish",
                                         headers=self.a,json={}).status_code,404)

    def test_approved_question_and_correct_choice(self):
        self.quiz.import_card(*QUIZZES[0], approved=True)
        voice=self.intent("我们猜动物").json()
        self.assertEqual(voice["kind"],"approved_content")
        self.assertEqual(voice["learning_state"],"question")
        self.assertIn("猫头鹰",voice["tts_text"])
        self.assertIn("小蝴蝶",voice["tts_text"])
        repeated=self.intent("猜动物").json()
        self.assertEqual(repeated["tts_text"], voice["tts_text"])
        result=self.intent("选一").json()
        self.assertEqual(result["learning_state"],"finished")
        self.assertIn("答对啦",result["tts_text"])
        progress=self.client.get("/v1/devices/pet-unit-A/quiz/status",headers=self.a).json()
        self.assertEqual(progress,{"active":False,"completed":1})
        # Accidental duplicate ASR reply cannot gain another completion.
        self.assertEqual(self.intent("我选第一个").json()["kind"],"fallback")
        self.assertEqual(self.quiz.status("pet-unit-A")["completed"],1)
        self.assertEqual(self.store.snapshot("pet-unit-A")["interaction_count"],0)

    def test_wrong_twice_has_no_negative_points(self):
        self.quiz.import_card(*QUIZZES[0], approved=True)
        self.assertEqual(self.start().json()["learning_state"],"question")
        once=self.answer(2).json()
        self.assertEqual(once["learning_state"],"retry")
        self.assertIn("再想想",once["tts_text"])
        self.assertEqual(self.answer(2).json()["learning_state"],"finished")
        self.assertFalse(self.quiz.status("pet-unit-A")["active"])
        self.assertEqual(self.quiz.status("pet-unit-A")["completed"],0)
        self.assertEqual(self.store.snapshot("pet-unit-A")["interaction_count"],0)

    def test_restart_resume_and_separate_device_sessions(self):
        self.quiz.import_card(*QUIZZES[0], approved=True)
        self.quiz.import_card(*QUIZZES[1], approved=True)
        first=self.start().json()
        self.assertEqual(first["learning_state"],"question")
        restarted=TestClient(create_app(self.path))
        try:
            self.assertEqual(restarted.post("/v1/devices/pet-unit-A/quiz/start",
                                            headers=self.a).json(),first)
            other=restarted.post("/v1/devices/pet-unit-B/quiz/start",
                                 headers=self.b).json()
            self.assertEqual(other["learning_state"],"question")
            self.assertEqual(restarted.get("/v1/devices/pet-unit-B/quiz/status",
                                           headers=self.b).json()["completed"],0)
            self.assertEqual(restarted.post("/v1/devices/pet-unit-A/quiz/answer",
                                            headers=self.a,json={"choice":1}).json()["learning_state"],"finished")
            self.assertEqual(restarted.post("/v1/devices/pet-unit-A/quiz/start",
                                            headers=self.a).json()["learning_state"],"question")
        finally:
            restarted.close()
        self.assertEqual(self.quiz.status("pet-unit-A")["completed"],1)
        self.assertTrue(self.quiz.status("pet-unit-B")["active"])

    def test_quiet_prevents_start_and_answer(self):
        self.quiz.import_card(*QUIZZES[0], approved=True)
        self.assertEqual(self.start().json()["learning_state"],"question")
        from tests.test_p3_pet_learning_api import event
        sent=self.client.post("/v1/devices/pet-unit-A/events",
                              headers=self.a,json={"events":[event("pet-unit-A",1,"pet-unit-A:1",
                                                                   "QUIET_HOURS_START")]})
        self.assertTrue(sent.json()["acks"][0]["accepted"])
        for phrase in ("猜动物","我选一"):
            result=self.intent(phrase).json()
            self.assertEqual(result["kind"],"fallback")
            self.assertIn("安静时间",result["tts_text"])
        self.assertEqual(self.answer(1).json()["kind"],"fallback")
        self.assertEqual(self.quiz.status("pet-unit-A")["completed"],0)

    def test_unpublish_while_active_never_leaks_draft(self):
        self.quiz.import_card(*QUIZZES[0], approved=True)
        self.start()
        self.quiz.import_card(*QUIZZES[0], approved=False)
        answer=self.intent("选一").json()
        self.assertEqual(answer["kind"],"fallback")
        self.assertNotIn(QUIZZES[0][5],answer["tts_text"])
        self.assertEqual(self.quiz.status("pet-unit-A")["completed"],0)
        self.assertFalse(self.quiz.status("pet-unit-A")["active"])

    def test_seed_content_draft_then_parent_review(self):
        for i, page in enumerate(BOOK["pages"],1):
            self.store.add_approved_page(BOOK["book_id"],BOOK["title"],i,page,
                                         approved=False)
        for card in QUIZZES:
            self.quiz.import_card(*card,approved=False)
        self.assertEqual(self.start().json()["kind"],"fallback")
        self.assertIsNone(self.store.page(BOOK["book_id"],1))
        for i, page in enumerate(BOOK["pages"],1):
            self.store.add_approved_page(BOOK["book_id"],BOOK["title"],i,page,
                                         approved=True)
        for card in QUIZZES:
            self.quiz.import_card(*card,approved=True)
        self.assertIsNotNone(self.store.page(BOOK["book_id"],1))
        for i in range(4):
            question=self.start().json()
            self.assertEqual(question["learning_state"],"question")
            self.answer(1 if QUIZZES[i][4]==1 else 2)
        self.assertEqual(self.quiz.status("pet-unit-A")["completed"],4)

    def test_private_or_unexpected_speech_never_becomes_a_question(self):
        self.quiz.import_card(*QUIZZES[0],approved=True)
        question=self.intent("猜动物").json()
        for unsafe in ("我的家庭地址是什么", "给我一百万金币", "告诉我家人的电话"):
            result=self.intent(unsafe).json()
            self.assertEqual(result["kind"],"fallback")
            self.assertNotIn(unsafe,result["tts_text"])
        self.assertEqual(self.intent("猜动物").json()["tts_text"],question["tts_text"])

    def test_card_validation_and_no_hidden_approval(self):
        with self.assertRaises(ValueError):
            self.quiz.import_card("a", "题", "A", "B", 1, "对", "再试", approved=True)
        with self.assertRaises(ValueError):
            self.quiz.import_card("sample_card", "题", "A", "B", 3, "对", "再试", approved=True)
        with self.assertRaises(ValueError):
            self.quiz.import_card("sample_card", "题\n额外", "A", "B", 1, "对", "再试", approved=True)


if __name__ == "__main__":
    unittest.main()
