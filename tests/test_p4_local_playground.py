"""Check opt-in local developer UI, static assets and live API contracts.

This is not a substitute for physical ePaper, microphone or speaker testing.
"""
from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient
from services.pet_learning.api import create_app
from services.pet_learning.store import PetStore
from services.pet_learning.quiz import QuizEngine
from services.pet_learning.sample_content import BOOK, QUIZZES


class ElementCollector(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids: set[str] = set()
        self.scripts: list[str] = []
        self.styles: list[str] = []
    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            if a["id"] in self.ids:
                raise ValueError("Duplicate UI element ID: " + a["id"])
            self.ids.add(a["id"])
        if tag == "script" and "src" in a:
            self.scripts.append(a["src"])
        if tag == "link" and a.get("rel") == "stylesheet":
            self.styles.append(a["href"])


class LocalPlaygroundTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = Path(self.tmp.name) / "pet.sqlite3"
        self.store = PetStore(self.db)
        self.store.enroll_device("pet-demo-001", "test-separate-browser-device-token-1234567890")
        self.client = TestClient(create_app(self.db))
        self.headers = {
            "Authorization": "Bearer test-separate-browser-device-token-1234567890"
        }

    def tearDown(self):
        self.client.close()
        self.tmp.cleanup()

    def test_default_disabled_never_exposes_html_or_assets(self):
        with patch.dict("os.environ", {"KID_PET_ENABLE_PLAYGROUND": "0"}):
            for route in ("/play", "/play/play.js", "/play/play.css"):
                self.assertEqual(self.client.get(route).status_code, 404)

    def test_explicit_optin_get_serves_no_store_with_strict_csp(self):
        with patch.dict("os.environ", {"KID_PET_ENABLE_PLAYGROUND": "1"}):
            page = self.client.get("/play")
            self.assertEqual(page.status_code, 200)
            self.assertIn("text/html", page.headers["content-type"])
            self.assertEqual(page.headers["cache-control"], "no-store")
            self.assertEqual(page.headers["x-content-type-options"], "nosniff")
            self.assertEqual(page.headers["referrer-policy"], "no-referrer")
            self.assertIn("default-src 'none'", page.headers["content-security-policy"])
            self.assertIn("script-src 'self'", page.headers["content-security-policy"])
            self.assertIn("frame-ancestors 'none'", page.headers["content-security-policy"])
            script = self.client.get("/play/play.js")
            css = self.client.get("/play/play.css")
            self.assertEqual(script.status_code, 200)
            self.assertEqual(css.status_code, 200)
            self.assertIn("javascript", script.headers["content-type"])
            self.assertIn("text/css", css.headers["content-type"])
            self.assertEqual(self.client.get("/play/../api.py").status_code, 404)
            self.assertEqual(self.client.get("/play/not-found").status_code, 404)

            tree=ElementCollector()
            tree.feed(page.text)
            self.assertEqual(tree.scripts, ["/play/play.js"])
            self.assertEqual(tree.styles, ["/play/play.css"])
            for required in ("connect-form", "device-id", "device-token",
                             "experience", "pet-feed", "pet-wave", "story-start",
                             "story-next", "story-repeat", "story-stop",
                             "quiz-start", "quiz-1", "quiz-2", "status",
                             "story-status", "quiz-progress", "disconnect",
                             "speak-toggle", "speak-stop", "pet-stats"):
                self.assertIn(required, tree.ids)
                self.assertIn('$("'+required+'")', script.text)
            self.assertNotIn("localStorage", script.text.replace(
                "No LLM, no microphone, no cookies, no persistent browser storage.", ""))
            self.assertNotIn("innerHTML", script.text)
            self.assertNotIn("eval(", script.text)

    def test_existing_real_api_can_drive_mock_ui_session(self):
        for i, text in enumerate(BOOK["pages"], 1):
            self.store.add_approved_page(
                BOOK["book_id"], BOOK["title"], i, text, approved=True)
        for quiz in QUIZZES:
            QuizEngine(self.db).import_card(*quiz, approved=True)
        snap = self.client.get("/v1/devices/pet-demo-001/snapshot", headers=self.headers)
        self.assertEqual(snap.status_code, 200)
        self.assertEqual(snap.json()["interaction_count"], 0)
        story = self.client.post("/v1/devices/pet-demo-001/story/start",
                                 headers=self.headers).json()
        self.assertEqual(story["story_state"], "reading")
        self.assertEqual(story["page"], 1)
        quiz = self.client.post("/v1/devices/pet-demo-001/quiz/start",
                                headers=self.headers).json()
        self.assertEqual(quiz["learning_state"], "question")
        self.assertLessEqual(len(quiz["tts_text"]), 320)
        event = {
            "schema_version": "1.0", "message_id": "play_abc123",
            "device_id": "pet-demo-001", "seq": 1, "type": "pet.event",
            "payload": {"event": "PET_FEED"},
        }
        endpoint = "/v1/devices/pet-demo-001/events"
        ack = self.client.post(endpoint, headers=self.headers,
                               json={"events":[event]}).json()["acks"][0]
        self.assertTrue(ack["accepted"])
        self.assertEqual(ack["message_id"], event["message_id"])
        self.assertEqual(ack["seq"], 1)
        duplicate = self.client.post(endpoint, headers=self.headers,
                                     json={"events":[event]}).json()["acks"][0]
        self.assertTrue(duplicate["accepted"])
        self.assertEqual(duplicate["reason_code"], "DUPLICATE")
        self.assertEqual(self.client.get("/v1/devices/pet-demo-001/snapshot",
                                          headers=self.headers).json()["interaction_count"], 1)


if __name__ == "__main__":
    unittest.main()
