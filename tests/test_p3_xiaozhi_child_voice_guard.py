"""Pure-mock and in-process HTTP tests. No real XiaoZhi server, secrets, child audio."""
from __future__ import annotations
import asyncio
import hashlib
import hmac
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import types
import unittest
from unittest.mock import patch

import httpx

from services.pet_learning.api import create_app
from services.pet_learning.store import PetStore
from scripts.p3_server_overlay import overlay, ANCHOR, INSERT
from scripts.p3_server_overlay import (overlay_mcp_client, overlay_mcp_executor, CLIENT_ANCHOR,
                                       CLIENT_INSERT, EXECUTOR_ANCHOR, EXECUTOR_INSERT)
from scripts.p3_server_overlay import TTS_EGRESS, TTS_EGRESS_PATHS, overlay_tts_egress

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


class FakeWebSocket:
    """Records what the server sends the board (JSON text frames, decoded)."""

    def __init__(self):
        self.sent = []

    async def send(self, data):
        self.sent.append(json.loads(data) if isinstance(data, str) else data)


class Connection:
    device_id = "physical-board-01"
    session_id = "session-1"
    sentence_id = None
    close_after_chat = False
    client_is_speaking = False
    client_abort = False

    def __init__(self):
        self.stop_event = threading.Event()
        self.websocket = FakeWebSocket()
        self.closed = 0

    def clearSpeakStatus(self):
        self.client_is_speaking = False

    async def close(self):
        self.closed += 1

    def tts_states(self):
        return [m["state"] for m in self.websocket.sent if isinstance(m, dict) and m.get("type") == "tts"]


class FakeMcpClient:
    """Stands in for upstream's MCPClient: the device listed self.pet.request."""

    def __init__(self, tools=("self_pet_request",)):
        self.tools = {name: {} for name in tools}

    def has_tool(self, name):
        return name in self.tools


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

    def fake_tts(self, device=None):
        """Upstream's speak_txt, and call_mcp_tool answering with device(action)."""
        spoken = []
        self.calls = []
        module = types.ModuleType("core.handle.intentHandler")
        send = types.ModuleType("core.handle.sendAudioHandle")

        async def send_tts_message(conn, state, text=None):
            # Upstream sendAudioHandle.send_tts_message at the pinned commit (minus audio pacing).
            message = {"type": "tts", "state": state, "session_id": conn.session_id}
            if text is not None:
                message["text"] = text
            if state == "stop":
                conn.clearSpeakStatus()
            await conn.websocket.send(json.dumps(message, ensure_ascii=False))
            if state == "start" and self.abort_after_start:
                # The device's abort lands while the start is on its way.
                conn.client_abort = True

        self.abort_after_start = False

        async def play(conn, sentence_id, text):
            # Upstream TTS threads -> sendAudioMessage: audio, then LAST sends stop and
            # closes when close_after_chat is set. Everything is dropped while aborted.
            if conn.client_abort or sentence_id != conn.sentence_id:
                return
            await conn.websocket.send(b"opus")
            await send_tts_message(conn, "stop")
            if conn.close_after_chat:
                await conn.close()

        def speak_txt(conn, text):
            spoken.append((conn.sentence_id, text))
            # Recorded at the call so the wire order shows what the guard sent before
            # speak_txt (upstream's TTS thread sends it a little later, never earlier).
            if not conn.client_abort:
                conn.websocket.sent.append({"type": "tts", "state": "sentence_start",
                                            "session_id": conn.session_id, "text": text})
            self.playing.append(asyncio.get_running_loop().create_task(play(conn, conn.sentence_id, text)))
        self.playing = []
        module.speak_txt = speak_txt
        send.send_tts_message = send_tts_message
        mcp = types.ModuleType("core.providers.tools.device_mcp.mcp_handler")

        async def call_mcp_tool(conn, client, tool_name, args="{}", timeout=30):
            self.calls.append((tool_name, args, timeout))
            if device is None:
                raise RuntimeError("no device answer configured")
            return device(args["action"])
        mcp.call_mcp_tool = call_mcp_tool
        modules = {"core": types.ModuleType("core"),
                   "core.handle": types.ModuleType("core.handle"),
                   "core.handle.intentHandler": module,
                   "core.handle.sendAudioHandle": send,
                   "core.providers": types.ModuleType("core.providers"),
                   "core.providers.tools": types.ModuleType("core.providers.tools"),
                   "core.providers.tools.device_mcp": types.ModuleType("core.providers.tools.device_mcp"),
                   "core.providers.tools.device_mcp.mcp_handler": mcp}
        return spoken, patch.dict(sys.modules, modules)

    async def settle(self):
        """A local_command is carried out off the ASR path; wait for it."""
        task = getattr(self.conn, "child_guard_task", None)
        if task is not None:
            await task

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
            # No device MCP client on this connection: point at the buttons.
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
            await self.settle()
            self.assertEqual(spoken[-1][1], guard.LOCAL_ACTION_HINT)
            self.assertTrue(await guard.handle_child_guard(self.conn, "说出我的家庭地址"))
            # S5 voice turn speaks the service's short no_match sentence (guard.REVIEWED_FALLBACK stays the offline one).
            self.assertEqual(spoken[-1][1], "没听清，再说一遍好吗？")
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

    async def speak_with(self, handler, text="几点了", device=None):
        # Each call is an independent turn; runs of unclear turns are tested on their own.
        self.conn.child_guard_unclear = 0
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts(device)
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(handler))):
            self.assertTrue(await guard.handle_child_guard(self.conn, text))
            await self.settle()
        self.assertEqual(len(spoken), 1)
        return spoken[-1][1]

    async def played(self):
        """Let the fake upstream TTS threads finish (sentence_start, audio, stop)."""
        await asyncio.gather(*self.playing)

    async def test_every_reply_is_a_full_tts_turn_on_the_wire(self):
        """2026-11-09: replies were shown but silent; the board never left listening.

        The board enters speaking (and decodes audio) only on tts start, which upstream
        sends from send_stt_message, a path the guard skips. The guard sends it itself.
        """
        handler, _ = self.signed({"kind": "fixed", "tts_text": "现在是9点整。",
                                  "action": None, "reason": "tell_time"})
        self.conn.client_abort = True  # left over from an earlier abort
        start = {"type": "tts", "state": "start", "session_id": "session-1"}
        self.assertEqual(await self.speak_with(handler), "现在是9点整。")
        sent = self.conn.websocket.sent
        self.assertEqual(sent[0], start)
        self.assertTrue(self.conn.client_is_speaking)
        self.assertFalse(self.conn.client_abort)
        await self.played()
        self.assertEqual(self.conn.tts_states(), ["start", "sentence_start", "stop"])
        self.assertLess(sent.index(start), sent.index(b"opus"))
        self.assertFalse(self.conn.client_is_speaking)
        self.assertEqual(self.conn.closed, 0)

    async def test_command_and_goodbye_replies_also_start_a_tts_turn(self):
        self.conn.mcp_client = FakeMcpClient()
        await self.speak_with(self.command(), "喂食", self.device("done"))
        await self.played()
        self.assertEqual(self.conn.tts_states(), ["start", "sentence_start", "stop"])
        self.conn.websocket.sent.clear()
        handler, _ = self.signed({"kind": "fixed", "tts_text": "x", "action": None, "reason": "x"})
        self.assertEqual(await self.speak_with(handler, guard._UPSTREAM_END_PROMPT), guard.GOODBYE)
        await self.played()
        self.assertEqual(self.conn.tts_states(), ["start", "sentence_start", "stop"])

    async def turn(self, body, text="嗯嗯"):
        """One turn without resetting the run of unclear replies."""
        handler, _ = self.signed(body)
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(handler))):
            self.assertTrue(await guard.handle_child_guard(self.conn, text))
            await self.played()
        return spoken[-1][1]

    async def test_three_unclear_turns_in_a_row_say_goodbye_and_end_the_chat(self):
        """Room noise kept the chat alive with "没听清" every few seconds for 20+ minutes."""
        unclear = {"kind": "fallback", "tts_text": "没听清，再说一遍好吗？", "action": None, "reason": "no_match"}
        bad_text = {"kind": "fallback", "tts_text": "没听清，再说一遍好吗？", "action": None, "reason": "text_invalid"}
        clear = {"kind": "fixed", "tts_text": "我在呢。", "action": None, "reason": "wake_greeting"}
        # A real turn whose answer is a fallback kind (open chat refused the reply).
        blocked = {"kind": "fallback", "tts_text": "这个问题我们问问大人吧。", "action": None,
                   "reason": "open_chat_output_blocked"}
        self.assertEqual(guard.UNCLEAR_LIMIT, 3)
        # Two unclear turns never end the chat; any valid turn starts the count again.
        for _ in range(2):
            self.assertEqual(await self.turn(unclear), "没听清，再说一遍好吗？")
        self.assertEqual(await self.turn(clear), "我在呢。")
        self.assertEqual(await self.turn(unclear), "没听清，再说一遍好吗？")
        self.assertEqual(await self.turn(bad_text), "没听清，再说一遍好吗？")  # text_invalid counts
        self.assertEqual(self.conn.child_guard_unclear, 2)
        self.assertEqual(await self.turn(blocked), "这个问题我们问问大人吧。")
        self.assertEqual(self.conn.child_guard_unclear, 0)
        self.assertEqual(await self.turn(unclear), "没听清，再说一遍好吗？")
        self.assertEqual(await self.turn(bad_text), "没听清，再说一遍好吗？")
        self.assertFalse(self.conn.close_after_chat)
        self.assertEqual(self.conn.closed, 0)
        # The third in a row: a goodbye that says how to call the pet back, then close.
        self.assertEqual(await self.turn(unclear), guard.UNCLEAR_GOODBYE)
        self.assertLessEqual(len(guard.UNCLEAR_GOODBYE), 17)
        self.assertIn("再叫我", guard.UNCLEAR_GOODBYE)
        self.assertTrue(self.conn.close_after_chat)
        self.assertEqual(self.conn.closed, 1)
        self.assertEqual(self.conn.tts_states()[-3:], ["start", "sentence_start", "stop"])
        self.assertEqual(self.conn.child_guard_unclear, 0)

    async def test_service_down_noise_also_ends_the_chat(self):
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient",
                                   side_effect=httpx.ConnectError("no service")):
            for _ in range(3):
                self.assertTrue(await guard.handle_child_guard(self.conn, "嗯"))
            await self.played()
        self.assertEqual([t for _, t in spoken], [guard.NO_SERVICE] * 2 + [guard.UNCLEAR_GOODBYE])
        self.assertEqual(self.conn.closed, 1)

    async def test_not_bound_counts_as_unclear(self):
        self.conn.device_id = "unknown-board"
        spoken, patched = self.fake_tts()
        with patched:
            for _ in range(3):
                self.assertTrue(await guard.handle_child_guard(self.conn, "嗯"))
            await self.played()
        self.assertEqual([t for _, t in spoken], [guard.NOT_BOUND] * 2 + [guard.UNCLEAR_GOODBYE])
        self.assertTrue(self.conn.close_after_chat)

    async def test_quiet_hours_reply_is_said_once_then_the_chat_ends(self):
        """At night every utterance gets the quiet sentence; room noise must not repeat it."""
        self.conn.child_guard_unclear = 2
        quiet = {"kind": "fixed", "tts_text": "现在是安静时间，我们轻轻休息吧。", "action": None,
                 "reason": "quiet_hours"}
        self.assertEqual(await self.turn(quiet), "现在是安静时间，我们轻轻休息吧。")
        self.assertTrue(self.conn.close_after_chat)
        self.assertEqual(self.conn.closed, 1)
        self.assertEqual(self.conn.child_guard_unclear, 0)
        self.assertEqual(self.conn.tts_states(), ["start", "sentence_start", "stop"])

    async def test_abort_while_waiting_drops_the_sentence(self):
        """The child interrupts (wake word -> abort) while PetLearning is still answering."""
        body = {"kind": "fixed", "tts_text": "现在是9点整。", "action": None, "reason": "tell_time"}
        sign, _ = self.signed(body)
        self.conn.client_abort = True  # left over from an earlier abort: cleared at turn start

        def handler(request):
            self.assertFalse(self.conn.client_abort)
            self.conn.client_abort = True  # handleAbortMessage runs meanwhile
            return sign(request)
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(handler))):
            self.assertTrue(await guard.handle_child_guard(self.conn, "几点了"))
            await self.played()
        self.assertEqual(spoken, [])
        self.assertEqual(self.conn.tts_states(), [])
        self.assertFalse(self.conn.client_is_speaking)

    async def test_abort_while_waiting_for_the_board_drops_the_command_sentence(self):
        self.conn.mcp_client = FakeMcpClient()

        def device(action):
            self.conn.client_abort = True
            return json.dumps({"action": action, "result": "done"})
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts(device)
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(self.command()))):
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
            await self.settle()
            await self.played()
        self.assertEqual(self.calls[0][1], {"action": "PET_FEED"})  # the board did act
        self.assertEqual(spoken, [])
        self.assertEqual(self.conn.tts_states(), [])

    async def test_abort_racing_the_start_still_leaves_speaking(self):
        handler, _ = self.signed({"kind": "fixed", "tts_text": "现在是9点整。",
                                  "action": None, "reason": "tell_time"})
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts()
        self.abort_after_start = True
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(handler))):
            self.assertTrue(await guard.handle_child_guard(self.conn, "几点了"))
            await self.played()
        self.assertEqual(spoken, [])
        self.assertEqual(self.conn.tts_states(), ["start", "stop"])
        self.assertFalse(self.conn.client_is_speaking)

    async def test_wake_text_gets_the_wake_greeting_end_to_end(self):
        """#12: the board's own wake text reaches PetLearning and is answered, not swallowed."""
        api = create_app(self.db.path)
        original_async = httpx.AsyncClient

        def inprocess_client(*args, **kwargs):
            return original_async(transport=httpx.ASGITransport(app=api),
                                  base_url="http://127.0.0.1:8765")
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=inprocess_client):
            for wake in ("你好小智", "你好团团"):
                self.assertTrue(await guard.handle_child_guard(self.conn, wake))
            # Upstream's greeting stand-in is still not a wake word (matching is not widened).
            self.assertTrue(await guard.handle_child_guard(self.conn, "嘿，你好呀"))
            await self.played()
        self.assertEqual([t for _, t in spoken[:2]], ["我在呢。", "我在呢。"])
        self.assertNotEqual(spoken[2][1], "我在呢。")

    async def test_guard_forwards_text_and_speaks_returned_sentence(self):
        handler, seen = self.signed({"kind": "fixed", "tts_text": "现在是9点整。",
                                     "action": None, "reason": "tell_time"})
        self.assertEqual(await self.speak_with(handler), "现在是9点整。")
        request = seen[0]
        self.assertEqual(str(request.url), "http://127.0.0.1:8765/v1/voice/pet-unit-A/turn")
        self.assertEqual(request.headers["authorization"], "Bearer " + VOICE_TOKEN)
        self.assertEqual(json.loads(request.content)["text"], "几点了")

    async def test_unexpected_kind_or_missing_text_is_fixed_error(self):
        for body in ({"kind": "open_chat"}, {"kind": "open_chat", "tts_text": "x" * 321},
                     {"kind": "chat", "tts_text": "x"}, {"kind": "fixed"},
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

    def command(self, action="PET_FEED", spoken="好呀，选一样好吃的喂团团吧。", reason="fixed_command"):
        return self.signed({"kind": "local_command", "tts_text": spoken, "action": action,
                            "reason": reason})[0]

    async def test_open_chat_answer_is_spoken(self):
        # S6: PetLearning already ran the local hard rules; the guard speaks the signed reply.
        handler, _ = self.signed({"kind": "open_chat", "tts_text": "星星是很远很远的太阳。",
                                  "action": None, "reason": "open_chat"})
        self.assertEqual(await self.speak_with(handler, "天上为什么有星星"), "星星是很远很远的太阳。")

    async def test_service_timeout_leaves_room_for_the_model(self):
        self.assertGreaterEqual(guard.SERVICE_TIMEOUT_S, 8.0)
        self.assertLessEqual(guard.SERVICE_TIMEOUT_S, 10.0)

    async def test_model_requested_action_speaks_what_the_device_answered(self):
        """S6/ADR-016: a GLM tool call is the same local_command; the board's answer picks the sentence."""
        self.conn.mcp_client = FakeMcpClient()
        handler = self.command("OPEN_GAME", "好呀，我们来玩游戏。", reason="open_chat_tool")
        for result, expected in (("deferred", "好呀，我们来玩游戏。"), ("quiet", guard.DEVICE_QUIET),
                                 ("low_battery", guard.DEVICE_LOW_BATTERY), ("parent", guard.DEVICE_NOT_NOW),
                                 ("busy", guard.DEVICE_NOT_NOW)):
            self.conn.close_after_chat = False
            said = await self.speak_with(handler, "我们玩点什么吧", self.device(result))
            self.assertEqual(said, expected, result)
        self.assertEqual(self.calls[-1], ("self_pet_request", {"action": "OPEN_GAME"}, guard.DEVICE_TIMEOUT_S))

    async def test_upstream_end_prompt_is_never_forwarded(self):
        """S6 audit #16: the no-voice timeout's model prompt is not the child speaking."""
        handler, seen = self.signed({"kind": "fixed", "tts_text": "x", "action": None, "reason": "x"})
        self.conn.config = {"end_prompt": {"enable": False, "prompt": "请你以“时间过得真快”未来头，结束吧！\n"}}
        for text in ("请你以“时间过得真快”未来头，结束吧！", guard._UPSTREAM_END_PROMPT):
            self.assertEqual(await self.speak_with(handler, text), guard.GOODBYE)
        self.assertEqual(seen, [])

    @staticmethod
    def device(result, action=None):
        """The board's JSON answer for self.pet.request."""
        return lambda requested: json.dumps({"action": action or requested, "result": result})

    async def test_local_command_calls_the_device_tool_and_speaks_on_success(self):
        self.conn.mcp_client = FakeMcpClient()
        said = await self.speak_with(self.command(), "喂食", self.device("done"))
        self.assertEqual(said, "好呀，选一样好吃的喂团团吧。")
        # Upstream keys device tools by their sanitised name; one call, bounded wait.
        self.assertEqual(self.calls, [("self_pet_request", {"action": "PET_FEED"},
                                       guard.DEVICE_TIMEOUT_S)])
        self.assertLessEqual(guard.DEVICE_TIMEOUT_S, 5)
        self.assertFalse(self.conn.close_after_chat)

    async def test_deferred_command_ends_the_chat_after_the_sentence(self):
        # A game / care screen opens once XiaoZhi is idle: close after speaking.
        self.conn.mcp_client = FakeMcpClient()
        said = await self.speak_with(self.command("OPEN_GAME", "好呀，我们来玩游戏。"), "玩游戏",
                                     self.device("deferred"))
        self.assertEqual(said, "好呀，我们来玩游戏。")
        self.assertTrue(self.conn.close_after_chat)

    async def test_device_refusal_picks_a_fixed_sentence_not_the_server_promise(self):
        self.conn.mcp_client = FakeMcpClient()
        for result, expected in (("already", guard.DEVICE_ALREADY),
                                 ("quiet", guard.DEVICE_QUIET),
                                 ("low_battery", guard.DEVICE_LOW_BATTERY),
                                 ("not_in_reader", guard.DEVICE_NOT_IN_READER),
                                 ("busy", guard.DEVICE_NOT_NOW), ("no_book", guard.DEVICE_NOT_NOW),
                                 ("rejected", guard.DEVICE_NOT_NOW), ("parent", guard.DEVICE_NOT_NOW),
                                 ("off", guard.DEVICE_NOT_NOW), ("ignored", guard.DEVICE_NOT_NOW),
                                 ("pending", guard.DEVICE_PENDING)):
            self.conn.close_after_chat = False
            said = await self.speak_with(self.command(), "喂食", self.device(result))
            self.assertEqual(said, expected, result)
            # pending: the board may have deferred it, so the chat ends too.
            self.assertEqual(self.conn.close_after_chat, result == "pending", result)
        for sentence in (guard.DEVICE_ALREADY, guard.DEVICE_QUIET, guard.DEVICE_LOW_BATTERY,
                         guard.DEVICE_NOT_IN_READER, guard.DEVICE_NOT_NOW, guard.DEVICE_PENDING):
            self.assertTrue(0 < len(sentence) <= 40)

    async def test_device_tool_missing_or_failing_falls_back_to_the_buttons(self):
        cases = [
            (None, self.device("done")),                         # no MCP on this board
            (FakeMcpClient(tools=()), self.device("done")),      # tool not listed
            (FakeMcpClient(), None),                             # call raises
            (FakeMcpClient(), lambda a: "not json"),             # garbage answer
            (FakeMcpClient(), lambda a: json.dumps([1])),
            (FakeMcpClient(), self.device("done", action="SLEEP_REQUEST")),  # wrong echo
            (FakeMcpClient(), self.device("exploded")),          # unknown token
        ]
        for client, device in cases:
            self.conn.mcp_client = client
            self.conn.close_after_chat = False
            said = await self.speak_with(self.command(), "喂食", device)
            self.assertEqual(said, guard.LOCAL_ACTION_HINT)
            self.assertFalse(self.conn.close_after_chat)

    async def test_timeout_after_sending_is_treated_as_pending(self):
        # The request reached the board, which may have acted (or deferred it):
        # never say "use the buttons" then; end the chat so a deferred screen opens.
        self.conn.mcp_client = FakeMcpClient()

        def slow(action):
            raise TimeoutError("工具调用请求超时")
        self.assertEqual(await self.speak_with(self.command(), "喂食", slow), guard.DEVICE_PENDING)
        self.assertTrue(self.conn.close_after_chat)

    async def test_malformed_action_never_reaches_the_device(self):
        self.conn.mcp_client = FakeMcpClient()
        for action in (None, "", "pet_feed", "PET_FEED;REBOOT", "X" * 16, 7, ["PET_FEED"]):
            said = await self.speak_with(self.command(action), "喂食", self.device("done"))
            self.assertEqual(said, guard.NO_SERVICE, action)
            self.assertEqual(self.calls, [], action)

    async def test_bad_server_sentence_on_success_is_not_spoken(self):
        self.conn.mcp_client = FakeMcpClient()
        for spoken in ("", "x" * 321, None):
            said = await self.speak_with(self.command(spoken=spoken), "喂食", self.device("done"))
            self.assertEqual(said, guard.DEVICE_OK, spoken)

    async def test_other_kinds_never_call_the_device(self):
        self.conn.mcp_client = FakeMcpClient()
        handler, _ = self.signed({"kind": "fixed", "tts_text": "现在是9点整。", "action": "PET_FEED",
                                  "reason": "tell_time"})
        self.assertEqual(await self.speak_with(handler, "几点了", self.device("done")), "现在是9点整。")
        self.assertEqual(self.calls, [])

    async def test_command_is_carried_out_off_the_asr_path(self):
        # Manual listen mode awaits the ASR inside the websocket receive loop, which
        # also delivers the device's reply: the guard must not wait for it there.
        self.conn.mcp_client = FakeMcpClient()
        gate = asyncio.Event()

        async def call_mcp_tool(conn, client, tool_name, args="{}", timeout=30):
            await gate.wait()
            return json.dumps({"action": args["action"], "result": "done"})
        spoken, patched = self.fake_tts()
        original_async = httpx.AsyncClient
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(self.command()))):
            sys.modules["core.providers.tools.device_mcp.mcp_handler"].call_mcp_tool = call_mcp_tool
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
            self.assertEqual(spoken, [])            # returned before the device answered
            gate.set()
            await self.settle()
        self.assertEqual(spoken[-1][1], "好呀，选一样好吃的喂团团吧。")
        self.assertEqual(len(spoken[-1][0]), 32)

    async def test_a_new_utterance_cancels_the_pending_command(self):
        # "玩游戏" then "几点了" before the board answered: the first one is dropped,
        # so it neither speaks over the second nor ends the chat after it.
        self.conn.mcp_client = FakeMcpClient()
        gate = asyncio.Event()

        async def call_mcp_tool(conn, client, tool_name, args="{}", timeout=30):
            await gate.wait()
            return json.dumps({"action": args["action"], "result": "deferred"})
        game = self.command("OPEN_GAME", "好呀，我们来玩游戏。")
        time_reply = self.signed({"kind": "fixed", "tts_text": "现在是9点整。", "action": None,
                                  "reason": "tell_time"})[0]

        def handler(request):
            said = json.loads(request.content)["text"]
            return (game if said == "玩游戏" else time_reply)(request)
        spoken, patched = self.fake_tts()
        original_async = httpx.AsyncClient
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(handler))):
            sys.modules["core.providers.tools.device_mcp.mcp_handler"].call_mcp_tool = call_mcp_tool
            self.assertTrue(await guard.handle_child_guard(self.conn, "玩游戏"))
            first = self.conn.child_guard_task
            self.assertTrue(await guard.handle_child_guard(self.conn, "几点了"))
            gate.set()
            with self.assertRaises(asyncio.CancelledError):
                await first
        self.assertEqual([s for _, s in spoken], ["现在是9点整。"])
        self.assertFalse(self.conn.close_after_chat)

    async def test_closed_connection_is_not_spoken_to(self):
        self.conn.mcp_client = FakeMcpClient()

        def device(action):
            self.conn.stop_event.set()  # the child walked away meanwhile
            return json.dumps({"action": action, "result": "done"})
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts(device)
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(self.command()))):
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
            await self.settle()
        self.assertEqual(spoken, [])

    async def test_waits_for_the_previous_sentence_before_speaking(self):
        # Changing sentence_id while upstream still plays a sentence drops its audio.
        self.conn.mcp_client = FakeMcpClient()
        self.conn.client_is_speaking = True
        seen = []

        def device(action):
            return json.dumps({"action": action, "result": "done"})
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts(device)
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(self.command()))), \
                patch.object(guard, "SPEAK_WAIT_STEP_S", 0.01):
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
            await asyncio.sleep(0.05)
            seen.append(list(spoken))
            self.conn.client_is_speaking = False
            await self.settle()
        self.assertEqual(seen, [[]])
        self.assertEqual(spoken[-1][1], "好呀，选一样好吃的喂团团吧。")

    async def test_carry_out_never_raises(self):
        self.conn.mcp_client = FakeMcpClient()
        original_async = httpx.AsyncClient
        spoken, patched = self.fake_tts(self.device("done"))

        def broken(conn, text):
            raise RuntimeError("tts gone")
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=lambda *a, **k: original_async(
                transport=httpx.MockTransport(self.command()))), \
                self.assertLogs(guard.__name__, level="ERROR"):
            sys.modules["core.handle.intentHandler"].speak_txt = broken
            self.assertTrue(await guard.handle_child_guard(self.conn, "喂食"))
            await self.settle()  # no exception escapes the task

    def test_device_tool_is_hidden_from_the_model(self):
        """ADR-008: whatever the mode, only the guard may call self_pet_request."""
        fixture = (
            "class MCPClient:\n"
            "    def __init__(self):\n"
            "        self.tools = {}\n"
            "        self._cached_available_tools = None\n"
            "    def get_available_tools(self) -> list:\n"
            "        result = []\n"
            + CLIENT_ANCHOR +
            "            function_def = {\"name\": tool_name, \"description\": tool_data[\"description\"]}\n"
            "            result.append({\"type\": \"function\", \"function\": function_def})\n"
            "        return result\n"
            "    def has_tool(self, name):\n"
            "        return name in self.tools\n"
        )
        patched = overlay_mcp_client(fixture)
        self.assertEqual(patched.replace(CLIENT_INSERT, ""), fixture)
        scope = {}
        exec(compile(patched, "mcp_client.py", "exec"), scope)
        client = scope["MCPClient"]()
        client.tools = {"self_pet_request": {"description": "x"},
                        "self_get_device_status": {"description": "y"}}
        names = [t["function"]["name"] for t in client.get_available_tools()]
        self.assertEqual(names, ["self_get_device_status"])
        self.assertTrue(client.has_tool("self_pet_request"))  # the guard still reaches it
        with self.assertRaisesRegex(ValueError, "already"):
            overlay_mcp_client(patched)
        with self.assertRaisesRegex(ValueError, "anchor changed"):
            overlay_mcp_client(fixture.replace(CLIENT_ANCHOR, "        for x in []:\n"))

        executor = (
            "class DeviceMCPExecutor:\n"
            "    async def execute(self, conn, tool_name, arguments):\n"
            + EXECUTOR_ANCHOR +
            "        return ('called', tool_name)\n"
        )
        patched = overlay_mcp_executor(executor)
        self.assertEqual(patched.replace(EXECUTOR_INSERT, ""), executor)
        scope = {"ActionResponse": lambda **kw: ("refused", kw["action"]),
                 "Action": types.SimpleNamespace(ERROR="ERROR")}
        exec(compile(patched, "mcp_executor.py", "exec"), scope)
        run = scope["DeviceMCPExecutor"]().execute
        self.assertEqual(asyncio.run(run(None, "self_pet_request", {"action": "SLEEP_REQUEST"})),
                         ("refused", "ERROR"))
        self.assertEqual(asyncio.run(run(None, "self_get_device_status", {})),
                         ("called", "self_get_device_status"))
        with self.assertRaisesRegex(ValueError, "already"):
            overlay_mcp_executor(patched)
        self.assertIn(guard.PET_REQUEST_TOOL, CLIENT_INSERT)
        self.assertIn(guard.PET_REQUEST_TOOL, EXECUTOR_INSERT)

    def test_overlay_installs_all_three_patches(self):
        source = (ROOT / "deploy" / "apply_overlay.py").read_text(encoding="utf-8")
        for needle in ("overlay_mcp_client(", "overlay_mcp_executor(", "mcp_client.py",
                       "mcp_executor.py", "overlay_tts_egress(", "TTS_EGRESS_PATHS"):
            self.assertIn(needle, source)
        main = (ROOT / "scripts" / "p3_server_overlay.py").read_text(encoding="utf-8")
        self.assertIn("overlay_mcp_client(", main[main.index("def main()"):])
        self.assertIn("overlay_mcp_executor(", main[main.index("def main()"):])
        self.assertIn("overlay_tts_egress(", main[main.index("def main()"):])

    def test_tts_egress_patches_close_every_upstream_source_in_child_mode(self):
        """S6 TTS egress audit: each patched upstream function is silent in child mode only."""
        fixtures = {
            "receiveAudioHandle.py": (
                "async def startToChat(conn, text):\n    conn.chatted.append(text)\n\n"
                'async def max_out_size(conn: "ConnectionHandler"):\n    conn.spoke.append("limit")\n\n'
                'async def check_bind_device(conn: "ConnectionHandler"):\n    conn.spoke.append("bind")\n\n'
                "async def no_voice_close_connect(conn, have_voice):\n"
                "        if True:\n"
                "            prompt = conn.prompt\n"
                "            await startToChat(conn, prompt)\n"),
            "listenMessageHandler.py": (
                "async def startToChat(conn, text):\n    conn.chatted.append(text)\n\n"
                "async def handle(conn, original_text, is_wakeup_words=False, enable_greeting=False):\n"
                "    if True:\n        if True:\n            if True:\n"
                '                if original_text.startswith("[device_call]"):\n'
                '                    call_text = original_text[len("[device_call]"):].strip()\n'
                "                    conn.spoke.append(call_text)\n"
                "                    return\n"
                "                if is_wakeup_words and not enable_greeting:\n"
                '                    conn.spoke.append("swallowed")\n'),
            "connection.py": (
                "class ConnectionHandler:\n"
                "    def chat(self, query, depth=0):\n"
                "        self.spoke.append(query)\n"
                "        return True\n"),
        }
        self.assertEqual(set(fixtures), set(TTS_EGRESS))
        self.assertEqual(set(TTS_EGRESS_PATHS), set(TTS_EGRESS))

        class Conn:
            def __init__(self):
                self.spoke, self.chatted, self.closed, self.prompt = [], [], 0, "说再见吧"

            async def close(self):
                self.closed += 1

        async def drive(scope, conn):
            await scope["max_out_size"](conn)
            await scope["check_bind_device"](conn)
            await scope["no_voice_close_connect"](conn, False)
        patched = {name: overlay_tts_egress(name, text) for name, text in fixtures.items()}
        for name, text in patched.items():
            for _anchor, insert, _where in TTS_EGRESS[name]:
                self.assertIn(insert, text)
            self.assertEqual(text.count("KID_PET_CHILD_MODE"), len(TTS_EGRESS[name]))
            with self.assertRaisesRegex(ValueError, "already installed"):
                overlay_tts_egress(name, text)
            with self.assertRaisesRegex(ValueError, "anchor changed"):
                overlay_tts_egress(name, fixtures[name].replace(TTS_EGRESS[name][0][0], "# moved\n"))
        for mode, silent in (("1", True), ("", False), ("0", False)):
            with patch.dict(os.environ, {"KID_PET_CHILD_MODE": mode}):
                scope, conn = {}, Conn()
                exec(compile(patched["receiveAudioHandle.py"], "r.py", "exec"), scope)
                asyncio.run(drive(scope, conn))
                self.assertEqual((conn.spoke, conn.chatted, conn.closed),
                                 ([], [], 1) if silent else (["limit", "bind"], ["说再见吧"], 0), mode)
                scope, conn = {}, Conn()
                exec(compile(patched["listenMessageHandler.py"], "l.py", "exec"), scope)
                asyncio.run(scope["handle"](conn, "[device_call]任何文字"))
                self.assertEqual(conn.spoke, [] if silent else ["任何文字"], mode)
                # #12: the board's wake text goes to the guard (startToChat) instead of
                # upstream's instant stt + start + stop, with VAD muted like a greeting.
                asyncio.run(scope["handle"](conn, "你好小智", True))
                self.assertEqual((conn.spoke, conn.chatted),
                                 ([], ["你好小智"]) if silent else (["任何文字", "swallowed"], []), mode)
                self.assertEqual(getattr(conn, "just_woken_up", False), silent, mode)
                scope = {}
                exec(compile(patched["connection.py"], "c.py", "exec"), scope)
                handler = scope["ConnectionHandler"]()
                handler.spoke = []
                self.assertEqual(handler.chat("x"), None if silent else True)
                self.assertEqual(handler.spoke, [] if silent else ["x"], mode)

    @unittest.skipUnless(os.environ.get("XZ_UPSTREAM_SRC"), "set XZ_UPSTREAM_SRC to a pinned upstream checkout")
    def test_tts_egress_patches_apply_to_the_pinned_upstream(self):
        source = Path(os.environ["XZ_UPSTREAM_SRC"])
        for name, parts in TTS_EGRESS_PATHS.items():
            overlay_tts_egress(name, source.joinpath(*parts).read_text(encoding="utf-8"))

    def test_child_config_closes_the_config_driven_tts_sources(self):
        import yaml
        from services.pet_learning.xz_checklist import CHILD_ERROR_RESPONSE, check_xiaozhi_config
        config = yaml.safe_load((ROOT / "deploy" / "xiaozhi" / "child.config.yaml").read_text(encoding="utf-8"))
        self.assertIs(config["enable_greeting"], False)
        self.assertIs(config["end_prompt"]["enable"], False)
        self.assertIs(config["enable_stop_tts_notify"], False)
        self.assertEqual(config["close_connection_no_voice_time"], 180)
        self.assertEqual(config["system_error_response"], CHILD_ERROR_RESPONSE)
        self.assertEqual(guard.NO_SERVICE, CHILD_ERROR_RESPONSE)
        for key, value in (("enable_greeting", True), ("end_prompt", {"enable": True}),
                           ("enable_stop_tts_notify", True), ("system_error_response", "主人，小智现在有点忙")):
            problems = check_xiaozhi_config({**config, key: value})
            self.assertTrue(any(key in p for p in problems), (key, problems))

    def test_s6_audit_document_lists_every_source(self):
        doc = (ROOT / "docs" / "SERVER_S6.md").read_text(encoding="utf-8")
        for needle in ("#1 ", "#15", "#16", "#18", "#19", "#20", "8f2dda09"):
            self.assertIn(needle, doc)

    def test_tool_name_matches_upstream_sanitising(self):
        # core.utils.util.sanitize_tool_name at the pinned commit.
        import re as _re
        self.assertEqual(guard.PET_REQUEST_TOOL,
                         _re.sub(r"[^a-zA-Z0-9_\-\u4e00-\u9fff]", "_", "self.pet.request"))

    async def test_invalid_url_is_a_fixed_error_not_an_exception(self):
        spoken, patched = self.fake_tts()
        with patched, patch.object(guard.httpx, "AsyncClient", side_effect=httpx.InvalidURL("bad")):
            self.assertTrue(await guard.handle_child_guard(self.conn, "你好"))
        self.assertEqual(spoken[-1][1], guard.NO_SERVICE)
        self.conn.child_guard_unclear = 0  # an independent case, not a second turn in a row
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
        # The device tool is the only action path, and it carries just the name.
        self.assertEqual(source.count("call_mcp_tool("), 1)
        self.assertIn('{"action": action}', source)

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
