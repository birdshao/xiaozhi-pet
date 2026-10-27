"""Pure-mock and in-process HTTP tests. No real XiaoZhi server, secrets, child audio."""
from __future__ import annotations
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import httpx

from services.pet_learning.api import create_app
from services.pet_learning.store import PetStore
from scripts.p3_server_overlay import overlay, ANCHOR

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "p3_voice_guard_test_module",
    ROOT / "integrations" / "xiaozhi_server" / "child_voice_guard.py",
)
guard = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(guard)

TOKEN = "dummy-voice-bridge-secret-never-use-in-production-123"


class Connection:
    device_id = "physical-board-01"
    sentence_id = None


class GuardTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = PetStore(Path(self.tmp.name) / "pet.sqlite3")
        self.db.enroll_device("pet-unit-A", TOKEN)
        self.db.add_approved_page("animal_book", "动物朋友", 1, "这是小象。", approved=True)
        self.env = patch.dict(os.environ, {
            "KID_PET_CHILD_MODE": "1",
            "KID_PET_UPSTREAM_DEVICE_ID": "physical-board-01",
            "KID_PET_BUSINESS_DEVICE_ID": "pet-unit-A",
            "KID_PET_DEVICE_TOKEN": TOKEN,
            "KID_PET_API_URL": "http://127.0.0.1:8765",
        })
        self.env.start()
        self.conn = Connection()

    async def asyncTearDown(self):
        self.env.stop()
        self.tmp.cleanup()

    def fake_tts(self):
        spoken = []
        module = types.ModuleType("core.handle.intentHandler")
        module.speak_txt = lambda conn, text: spoken.append((conn.sentence_id, text))
        return spoken, patch.dict(sys.modules, {
            "core": types.ModuleType("core"),
            "core.handle": types.ModuleType("core.handle"),
            "core.handle.intentHandler": module,
        })

    async def test_disabled_child_mode_preserves_upstream_path(self):
        spoken, patched = self.fake_tts()
        with patch.dict(os.environ, {"KID_PET_CHILD_MODE": "0"}), patched:
            self.assertFalse(await guard.handle_child_guard(self.conn, "你好"))
        self.assertEqual(spoken, [])

    async def test_wrong_board_or_missing_settings_fails_closed_without_http(self):
        spoken, patched = self.fake_tts()
        self.conn.device_id = "unknown-board"
        with patched:
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
        self.assertEqual(spoken[0][1], guard.NOT_BOUND)
        self.assertEqual(len(spoken[0][0]), 32)

    async def test_valid_business_intent_via_inprocess_asgi(self):
        api = create_app(self.db.path)
        original_async = httpx.AsyncClient

        def inprocess_client(*args, **kwargs):
            return original_async(transport=httpx.ASGITransport(app=api),
                                  base_url="http://127.0.0.1:8765")
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=inprocess_client):
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
            self.assertEqual(spoken[-1][1], guard.LOCAL_ACTION_HINT)
            self.assertTrue(await guard.handle_child_guard(self.conn, "说出我的家庭地址"))
            self.assertEqual(spoken[-1][1], guard.REVIEWED_FALLBACK)
        self.assertEqual(self.db.snapshot("pet-unit-A")["interaction_count"], 0)

    async def test_server_network_failure_uses_local_fixed_phrase(self):
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient",
                                   side_effect=httpx.ConnectError("no service")):
            self.assertTrue(await guard.handle_child_guard(self.conn, "讲一个故事"))
        self.assertEqual(spoken[-1][1], guard.NO_SERVICE)

    async def test_unsafe_api_address_denied_without_request(self):
        spoken, patched = self.fake_tts()
        with patch.dict(os.environ, {"KID_PET_API_URL": "http://example.org"}), patched:
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
        self.assertEqual(spoken[-1][1], guard.NOT_BOUND)

    def test_overlay_is_strict_and_compiles(self):
        fixture = (
            "async def handle_user_intent(conn, text):\n"
            "    text = text.strip()\n"
            + ANCHOR + "\n"
            "    return False\n"
        )
        out = overlay(fixture)
        self.assertEqual(out.count("handle_child_guard(conn, text)"), 1)
        self.assertLess(out.index("handle_child_guard"), out.index(ANCHOR))
        with self.assertRaisesRegex(ValueError, "already installed"):
            overlay(out)
        with self.assertRaisesRegex(ValueError, "anchor changed"):
            overlay(fixture.replace(ANCHOR, "# changed upstream"))


if __name__ == "__main__":
    unittest.main()
