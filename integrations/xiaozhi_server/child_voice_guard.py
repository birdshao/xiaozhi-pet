"""Opt-in child ASR guard for xinnan-tech/xiaozhi-esp32-server.

Installed into upstream plugins_func/functions/child_voice_guard.py by the
local overlay script. Non-child mode is an exact no-op. Child mode MUST
consume every ASR utterance, including errors, so arbitrary transcripts
never fall through to normal LLM streaming and unreviewed TTS.
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import ipaddress
import json
import os
import re
import uuid
from urllib.parse import urlsplit
import httpx

REVIEWED_FALLBACK = "我们可以看看绘本，或者请家人一起聊聊这个问题。"
NO_SERVICE = "现在还不能语音聊天，我们可以先用按键和小宠物玩。"
NOT_BOUND = "请让家长帮忙连接儿童模式。"
LOCAL_ACTION_HINT = "我听到了，我们用设备上的按键完成这个动作吧。"

# local_command: the device's MCP tool self.pet.request(action) decides (ADR-008:
# the fixed-command matcher proposes, PetCore on the board executes). Upstream keys
# device tools by their sanitised name (core.utils.util.sanitize_tool_name).
PET_REQUEST_TOOL = "self_pet_request"
DEVICE_TIMEOUT_S = 3
_ACTION_RE = re.compile(r"[A-Z_]{1,15}")
# What the pet answered decides the sentence: the server's reviewed sentence only
# when the device accepted; otherwise one of these fixed sentences.
DEVICE_OK = "好的。"
DEVICE_PENDING = "好的，我们试试看。"
DEVICE_ALREADY = "我们已经在这里啦。"
DEVICE_QUIET = "现在是安静时间，我们等一会儿再玩吧。"
DEVICE_LOW_BATTERY = "团团有点没电了，请大人帮忙充上电吧。"
DEVICE_NOT_IN_READER = "我们先打开绘本，再来翻页吧。"
DEVICE_NOT_NOW = "现在还不行哦，我们等一会儿再试吧。"
_ACCEPTED = ("done", "deferred")
_DEVICE_SENTENCES = {
    "pending": DEVICE_PENDING, "already": DEVICE_ALREADY, "quiet": DEVICE_QUIET,
    "low_battery": DEVICE_LOW_BATTERY, "not_in_reader": DEVICE_NOT_IN_READER,
    "busy": DEVICE_NOT_NOW, "no_book": DEVICE_NOT_NOW, "rejected": DEVICE_NOT_NOW,
    "parent": DEVICE_NOT_NOW, "off": DEVICE_NOT_NOW, "ignored": DEVICE_NOT_NOW,
    "expired": DEVICE_NOT_NOW, "no_clock": DEVICE_NOT_NOW, "no_battery": DEVICE_NOT_NOW,
}


def child_mode_enabled() -> bool:
    return os.getenv("KID_PET_CHILD_MODE", "").strip() == "1"


_ALLOWED_KINDS = ("fixed", "approved_content", "fallback")
_LOOPBACK_HOSTS = ("127.0.0.1", "localhost", "::1")


def _approved_local_url() -> str | None:
    """Loopback, or a literal private/link-local IP (e.g. a pinned compose address).

    No hostnames beyond loopback: a name could resolve to any LAN device, and
    configuration must not be able to send child text or the token elsewhere.
    """
    raw = os.getenv("KID_PET_API_URL", "http://127.0.0.1:8765").rstrip("/")
    try:
        parsed = urlsplit(raw)
        host = (parsed.hostname or "").lower()
        parsed.port  # noqa: B018 - raises ValueError on a malformed port
    except ValueError:
        return None
    if host not in _LOOPBACK_HOSTS:
        try:
            address = ipaddress.ip_address(host)
        except ValueError:
            return None
        if not (address.is_private or address.is_link_local) or address.is_unspecified:
            return None
    if (parsed.scheme != "http" or parsed.username or parsed.password
            or parsed.query or parsed.fragment or parsed.path not in ("", "/")):
        return None
    return raw


class _Command:
    """A signed local_command: the action name and the server's reviewed sentence."""

    def __init__(self, action: str, spoken):
        self.action = action
        self.spoken = spoken if isinstance(spoken, str) and 0 < len(spoken) <= 320 else None


async def _voice_reply(conn, text: str):
    """Forward the ASR text to PetLearning and return the sentence it chose.

    All decisions (commands, page Q&A, quiet hours, fallback) live server-side;
    this function holds no intent logic and never triggers a device action
    itself: a local_command comes back as a _Command for _carry_out().
    """
    upstream_id = os.getenv("KID_PET_UPSTREAM_DEVICE_ID", "")
    business_id = os.getenv("KID_PET_BUSINESS_DEVICE_ID", "")
    token = os.getenv("KID_PET_VOICE_TOKEN", "")
    base = _approved_local_url()
    if (not upstream_id or not business_id or len(token) < 32
            or not base or not getattr(conn, "device_id", None)
            or conn.device_id != upstream_id):
        return NOT_BOUND
    if not isinstance(text, str):
        return REVIEWED_FALLBACK
    turn_id = uuid.uuid4().hex
    try:
        async with httpx.AsyncClient(timeout=httpx.Timeout(3.0, connect=1.0),
                                     follow_redirects=False, trust_env=False) as client:
            response = await client.post(
                base + "/v1/voice/" + business_id + "/turn",
                headers={"Authorization": "Bearer " + token},
                json={"text": text, "turn_id": turn_id},
            )
        response.raise_for_status()
        # The reply must be signed with the shared voice token over turn_id + body,
        # so only the real PetLearning service can make the pet speak.
        expected = hmac.new(token.encode("utf-8"), turn_id.encode("ascii") + b"\n" + response.content,
                            hashlib.sha256).hexdigest()
        if not hmac.compare_digest(expected, response.headers.get("x-voice-signature", "")):
            return NO_SERVICE
        data = response.json()
        if not isinstance(data, dict):
            return NO_SERVICE
        kind, spoken = data.get("kind"), data.get("tts_text")
        if kind == "local_command":
            action = data.get("action")
            if not isinstance(action, str) or not _ACTION_RE.fullmatch(action):
                return NO_SERVICE
            return _Command(action, spoken)
        if kind in _ALLOWED_KINDS and isinstance(spoken, str) and 0 < len(spoken) <= 320:
            return spoken
    except (httpx.HTTPError, httpx.InvalidURL, ValueError, TypeError):
        pass
    return NO_SERVICE


async def _device_result(conn, action: str) -> str | None:
    """Ask the board's self.pet.request(action); its result token, or None."""
    client = getattr(conn, "mcp_client", None)
    if client is None or not client.has_tool(PET_REQUEST_TOOL):
        return None
    from core.providers.tools.device_mcp.mcp_handler import call_mcp_tool
    try:
        raw = await call_mcp_tool(conn, client, PET_REQUEST_TOOL, {"action": action},
                                  timeout=DEVICE_TIMEOUT_S)
        answer = json.loads(raw)
    except Exception:  # noqa: BLE001 - not ready, no tool, device error, timeout, bad JSON
        return None
    if not isinstance(answer, dict) or answer.get("action") != action:
        return None
    result = answer.get("result")
    return result if result in _ACCEPTED or result in _DEVICE_SENTENCES else None


async def _carry_out(conn, command: _Command) -> None:
    """Run on its own task: the device's reply arrives on the websocket loop."""
    result = await _device_result(conn, command.action)
    if result in _ACCEPTED:
        answer = command.spoken or DEVICE_OK
        if result == "deferred":
            # A game / care screen opens once XiaoZhi is idle: end the chat after this.
            conn.close_after_chat = True
    else:
        answer = _DEVICE_SENTENCES.get(result, LOCAL_ACTION_HINT)
    _speak(conn, answer)


def _speak(conn, answer: str) -> None:
    # Imported lazily to avoid circular import when upstream loads the hook.
    from core.handle.intentHandler import speak_txt
    conn.sentence_id = uuid.uuid4().hex
    speak_txt(conn, answer)


async def handle_child_guard(conn, text: str) -> bool:
    """Called before normal XiaoZhi LLM intent/chat path.

    True => ASR consumed; upstream must NOT forward raw text to ordinary LLM.
    False => only when child mode is explicitly disabled.
    """
    if not child_mode_enabled():
        return False
    answer = await _voice_reply(conn, text)
    if isinstance(answer, _Command):
        # Manual listen mode awaits the ASR inside the websocket receive loop, which
        # is also where the device's tool reply arrives: never wait for it here.
        conn.child_guard_task = asyncio.get_running_loop().create_task(_carry_out(conn, answer))
        return True
    _speak(conn, answer)
    return True
