"""Pure-mock and in-process HTTP tests. No real XiaoZhi server, secrets, child audio."""
from __future__ import annotations
import hashlib
import hmac
import importlib.util
import json
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
from scripts.p3_server_overlay import overlay, ANCHOR, INSERT

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "p3_voice_guard_test_module",
    ROOT / "integrations" / "xiaozhi_server" / "child_voice_guard.py",
)
guard = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(guard)

TOKEN = "dummy-voice-bridge-secret-never-use-in-production-123"
VOICE_TOKEN = "dummy-voice-service-secret-never-use-in-production-9"


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
            "KID_PET_VOICE_TOKEN": VOICE_TOKEN,
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

    def signed(self, body, *, key=VOICE_TOKEN, raw=None):
        """MockTransport handler that signs like PetLearning (or not, with a wrong key)."""
        seen = []

        def handler(request):
            turn_id = json.loads(request.content)["turn_id"]
            content = raw if raw is not None else json.dumps(body, ensure_ascii=False).encode("utf-8")
            sig = hmac.new(key.encode(), turn_id.encode() + b"\n" + content, hashlib.sha256).hexdigest()
            seen.append(request)
            return httpx.Response(200, content=content, headers={"X-Voice-Signature": sig})
        return handler, seen

    async def speak_with(self, handler, text="几点了"):
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(handler))):
            self.assertTrue(await guard.handle_child_guard(self.conn, text))
        return spoken[-1][1]

    async def test_guard_forwards_text_and_speaks_returned_sentence(self):
        handler, seen = self.signed({"kind": "fixed", "tts_text": "现在是9点整。",
                                     "action": None, "reason": "tell_time"})
        self.assertEqual(await self.speak_with(handler), "现在是9点整。")
        request = seen[0]
        self.assertEqual(str(request.url), "http://127.0.0.1:8765/v1/voice/pet-unit-A/turn")
        self.assertEqual(request.headers["authorization"], "Bearer " + VOICE_TOKEN)
        self.assertEqual(json.loads(request.content)["text"], "几点了")

    async def test_unexpected_kind_or_missing_text_is_fixed_error(self):
        for body in ({"kind": "open_chat", "tts_text": "x"}, {"kind": "fixed"},
                     {"kind": "fixed", "tts_text": "x" * 321}, [1]):
            handler, _ = self.signed(body)
            self.assertEqual(await self.speak_with(handler, "你好"), guard.NO_SERVICE)

    async def test_unsigned_or_wrongly_signed_reply_is_never_spoken(self):
        body = {"kind": "fixed", "tts_text": "我是坏人说的话。", "action": None, "reason": "x"}
        handler, _ = self.signed(body, key="another-secret-another-secret-another-1")
        self.assertEqual(await self.speak_with(handler), guard.NO_SERVICE)
        unsigned = lambda request: httpx.Response(200, json=body)
        self.assertEqual(await self.speak_with(unsigned), guard.NO_SERVICE)
        # A valid signature for a different turn_id (replay) is rejected too.
        replay = lambda request: httpx.Response(200, content=json.dumps(body).encode(), headers={
            "X-Voice-Signature": hmac.new(VOICE_TOKEN.encode(), b"0" * 32 + b"\n" + json.dumps(body).encode(),
                                          hashlib.sha256).hexdigest()})
        self.assertEqual(await self.speak_with(replay), guard.NO_SERVICE)

    async def test_signed_local_command_still_says_use_the_buttons(self):
        handler, _ = self.signed({"kind": "local_command", "tts_text": "好呀", "action": "PET_FEED",
                                  "reason": "fixed_command"})
        self.assertEqual(await self.speak_with(handler, "喂食"), guard.LOCAL_ACTION_HINT)

    async def test_invalid_url_is_a_fixed_error_not_an_exception(self):
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=httpx.InvalidURL("bad")):
            self.assertTrue(await guard.handle_child_guard(self.conn, "你好"))
        self.assertEqual(spoken[-1][1], guard.NO_SERVICE)
        with patch.dict(os.environ, {"KID_PET_BUSINESS_DEVICE_ID": "pet unit\n"}):
            spoken, patched = self.fake_tts()
            with patched:
                await guard.handle_child_guard(self.conn, "你好")
        self.assertIn(spoken[-1][1], (guard.NOT_BOUND, guard.NO_SERVICE))

    async def test_missing_voice_token_or_device_token_only_fails_closed(self):
        spoken, patched = self.fake_tts()
        with patch.dict(os.environ, {"KID_PET_VOICE_TOKEN": "", "KID_PET_DEVICE_TOKEN": TOKEN}), patched:
            await guard.handle_child_guard(self.conn, "喂食")
        self.assertEqual(spoken[-1][1], guard.NOT_BOUND)

    async def test_api_address_policy_is_fixed_in_code(self):
        self.assertEqual(guard._approved_local_url(), "http://127.0.0.1:8765")
        for good in ("http://localhost:1", "http://[::1]:8780", "http://172.18.0.3:8780",
                     "http://10.1.2.3", "http://192.168.1.9:80", "http://169.254.1.1:80"):
            with patch.dict(os.environ, {"KID_PET_API_URL": good}):
                self.assertEqual(guard._approved_local_url(), good, good)
        for bad in ("https://127.0.0.1", "http://pet-learning:8780", "http://example.org",
                    "http://8.8.8.8", "http://0.0.0.0:80", "http://127.0.0.1:99999",
                    "http://127.0.0.1:abc", "http://u:p@127.0.0.1", "http://127.0.0.1/x",
                    "http://127.0.0.1?a=1", "http://[::", "", "http://10.0.0.1.evil.com"):
            with patch.dict(os.environ, {"KID_PET_API_URL": bad}):
                self.assertIsNone(guard._approved_local_url(), bad)
        # The old env knob must not widen anything.
        with patch.dict(os.environ, {"KID_PET_API_URL": "http://example.org",
                                     "KID_PET_API_ALLOWED_HOSTS": "example.org"}):
            self.assertIsNone(guard._approved_local_url())

    def test_overlay_pin_matches_version_lock(self):
        import json as _json
        from scripts.p3_server_overlay import PINNED_SERVER_COMMIT
        lock = _json.loads((ROOT / "version-lock.json").read_text(encoding="utf-8"))
        self.assertEqual(PINNED_SERVER_COMMIT, lock["voice_server"]["commit"])
        doc = (ROOT / "docs" / "P3_XIAOZHI_VOICE_BRIDGE.md").read_text(encoding="utf-8")
        self.assertIn(PINNED_SERVER_COMMIT, doc)

    def test_guard_module_is_thin(self):
        source = (ROOT / "integrations" / "xiaozhi_server" / "child_voice_guard.py").read_text(encoding="utf-8")
        self.assertIn("/v1/voice/", source)
        self.assertNotIn("/intent", source)
        self.assertNotIn("KID_PET_DEVICE_TOKEN", source)
        self.assertNotIn("ALLOWED_HOSTS", source)
        self.assertIn("compare_digest", source)
        self.assertNotRegex(source, r"(?m)^\s*(from|import)\s+(openai|anthropic|dashscope)")

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
        # The overlay stays a single insertion before the anchor; nothing else changes.
        self.assertEqual(out.replace(INSERT, ""), fixture)
        self.assertEqual(INSERT.count("handle_child_guard"), 2)
        self.assertEqual(len(INSERT.strip().splitlines()), 5)
        with self.assertRaisesRegex(ValueError, "anchor changed"):
            overlay(fixture + ANCHOR + "\n")


if __name__ == "__main__":
    unittest.main()
