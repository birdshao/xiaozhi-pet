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
import logging
import os
import re
import uuid
from urllib.parse import urlsplit
import httpx

REVIEWED_FALLBACK = "我们可以看看绘本，或者请家人一起聊聊这个问题。"
NO_SERVICE = "现在还不能语音聊天，我们可以先用按键和小宠物玩。"
NOT_BOUND = "请让家长帮忙连接儿童模式。"
LOCAL_ACTION_HINT = "我听到了，我们用设备上的按键完成这个动作吧。"
GOODBYE = "我们下次再聊吧，拜拜。"
# Ends a run of unclear turns: also tells the child how to get the pet back.
UNCLEAR_GOODBYE = "想找我就再叫我一声哦，拜拜。"
# PetLearning may wait up to 6 s for the open-chat model (S6); fixed answers come back at once.
SERVICE_TIMEOUT_S = 9.0

# local_command: the device's MCP tool self.pet.request(action) decides (ADR-008:
# the fixed-command matcher proposes, PetCore on the board executes). Upstream keys
# device tools by their sanitised name (core.utils.util.sanitize_tool_name).
PET_REQUEST_TOOL = "self_pet_request"
DEVICE_TIMEOUT_S = 3
# Upstream drops audio whose sentence_id is not the current one: wait for a
# sentence still playing before starting ours (bounded).
SPEAK_WAIT_S = 4.0
SPEAK_WAIT_STEP_S = 0.1
# Room noise keeps an auto-listening board sending ASR text, and each one used to get
# "没听清" back, so the chat never ended (2026-11-09 serial log). After this many
# no-intent replies in a row the guard says goodbye and ends the chat. no_match is also
# a real question the pet cannot answer (open chat off), so a child gets two of those
# and a third try before the goodbye (docs/SERVER_S5.md, "会话结束").
UNCLEAR_LIMIT = 3
# PetLearning reasons that mean "no valid intent" (noise, mis-heard speech). Other
# fallback kinds (quiz / story prompts, open-chat errors) are real turns and do not count.
_UNCLEAR_REASONS = ("no_match", "text_invalid")
_log = logging.getLogger(__name__)
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


# open_chat: a GLM answer PetLearning already passed through its local hard rules (S6, ADR-015).
_ALLOWED_KINDS = ("fixed", "approved_content", "fallback", "open_chat")
# Upstream's no-voice timeout feeds this text to chat as if the child said it
# (receiveAudioHandle.no_voice_close_connect). The overlay ends the chat silently in
# child mode and the config disables it; this is the third line of defence.
_UPSTREAM_END_PROMPT = "请你以```时间过得真快```未来头，用富有感情、依依不舍的话来结束这场对话吧。！"
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


class _Unclear(str):
    """A sentence that answered no valid intent (fallback, service down, not bound)."""


class _Closing(str):
    """A valid reply after which the chat ends (quiet hours: every utterance gets it)."""


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
        return _Unclear(NOT_BOUND)
    if not isinstance(text, str):
        return _Unclear(REVIEWED_FALLBACK)
    turn_id = uuid.uuid4().hex
    try:
        async with httpx.AsyncClient(timeout=httpx.Timeout(SERVICE_TIMEOUT_S, connect=1.0),
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
            return _Unclear(NO_SERVICE)
        data = response.json()
        if not isinstance(data, dict):
            return _Unclear(NO_SERVICE)
        kind, spoken = data.get("kind"), data.get("tts_text")
        if kind == "local_command":
            action = data.get("action")
            if not isinstance(action, str) or not _ACTION_RE.fullmatch(action):
                return _Unclear(NO_SERVICE)
            return _Command(action, spoken)
        if kind in _ALLOWED_KINDS and isinstance(spoken, str) and 0 < len(spoken) <= 320:
            if kind == "fallback" and data.get("reason") in _UNCLEAR_REASONS:
                return _Unclear(spoken)
            if data.get("reason") == "quiet_hours":
                return _Closing(spoken)
            return spoken
    except (httpx.HTTPError, httpx.InvalidURL, ValueError, TypeError):
        pass
    return _Unclear(NO_SERVICE)


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
    except (TimeoutError, asyncio.TimeoutError):
        # The request went out: the board may have acted or deferred it.
        return "pending"
    except Exception:  # noqa: BLE001 - not ready, no tool, device error (isError), bad JSON
        return None
    if not isinstance(answer, dict) or answer.get("action") != action:
        return None
    result = answer.get("result")
    return result if result in _ACCEPTED or result in _DEVICE_SENTENCES else None


async def _carry_out(conn, command: _Command) -> None:
    """Run on its own task: the device's reply arrives on the websocket loop."""
    try:
        await _carry_out_unsafe(conn, command)
    except Exception:  # noqa: BLE001 - never leave an unhandled task error behind
        _log.exception("child guard: local_command %s failed", command.action)


def _closed(conn) -> bool:
    stop = getattr(conn, "stop_event", None)
    return bool(stop is not None and stop.is_set())


async def _carry_out_unsafe(conn, command: _Command) -> None:
    result = await _device_result(conn, command.action)
    if result in _ACCEPTED:
        answer = command.spoken or DEVICE_OK
        if result == "deferred":
            # A game / care screen opens once XiaoZhi is idle: end the chat after this.
            conn.close_after_chat = True
    else:
        answer = _DEVICE_SENTENCES.get(result, LOCAL_ACTION_HINT)
        if result == "pending":
            # The board may still defer it (a busy pet_input): only an idle XiaoZhi lets it run.
            conn.close_after_chat = True
    waited = 0.0
    while getattr(conn, "client_is_speaking", False) and waited < SPEAK_WAIT_S and not _closed(conn):
        await asyncio.sleep(SPEAK_WAIT_STEP_S)
        waited += SPEAK_WAIT_STEP_S
    if _closed(conn):
        return
    await _speak(conn, answer)


async def _speak(conn, answer: str) -> None:
    """Speak one reviewed sentence as its own turn: tts start, audio, tts stop.

    Upstream sends {"type":"tts","state":"start"} only from send_stt_message, i.e. on
    the normal chat path that the guard short-circuits; speak_txt alone sends just
    sentence_start + audio + stop. The board (xiaozhi-esp32 v2.5.2) enters speaking
    only on "start" and decodes downlink audio only while speaking, so without it every
    reply was shown as text, played silently, and the board stayed listening.
    """
    # Imported lazily to avoid circular import when upstream loads the hook.
    from core.handle.intentHandler import speak_txt
    from core.handle.sendAudioHandle import send_tts_message
    # The child interrupted (abort) while the guard waited for PetLearning or the board:
    # this sentence answers an utterance that was cut off, so it is dropped.
    if getattr(conn, "client_abort", False):
        return
    conn.sentence_id = uuid.uuid4().hex
    await send_tts_message(conn, "start")
    if getattr(conn, "client_abort", False):
        # The abort raced the start: upstream's abort "stop" may have gone out first, and
        # its TTS threads now drop our sentence (no stop of their own). Leave the board
        # out of speaking ourselves.
        await conn.websocket.send(json.dumps(
            {"type": "tts", "state": "stop", "session_id": getattr(conn, "session_id", None)}))
        conn.clearSpeakStatus()
        return
    conn.client_is_speaking = True  # as send_stt_message does; cleared by the "stop"
    speak_txt(conn, str(answer))


def _end_unclear_run(conn, answer):
    """Count no-intent replies in a row; the UNCLEAR_LIMIT-th says goodbye and ends the chat."""
    if not isinstance(answer, _Unclear):
        conn.child_guard_unclear = 0
        if isinstance(answer, _Closing):
            # Quiet hours answer every utterance alike: say it once, then end the chat
            # (room noise at night would otherwise repeat it every few seconds).
            conn.close_after_chat = True
        return answer
    count = getattr(conn, "child_guard_unclear", 0) + 1
    if count < UNCLEAR_LIMIT:
        conn.child_guard_unclear = count
        return answer
    conn.child_guard_unclear = 0
    # Upstream closes the connection after this sentence's stop (sendAudioMessage, LAST).
    conn.close_after_chat = True
    _log.info("child guard: %d unclear turns in a row, ending the chat", count)
    return UNCLEAR_GOODBYE


def _is_end_prompt(conn, text) -> bool:
    if not isinstance(text, str):
        return False
    config = getattr(conn, "config", None)
    end = (config.get("end_prompt") if isinstance(config, dict) else None) or {}
    prompts = {_UPSTREAM_END_PROMPT, str(end.get("prompt") or "").strip()} - {""}
    return text.strip() in prompts


async def handle_child_guard(conn, text: str) -> bool:
    """Called before normal XiaoZhi LLM intent/chat path.

    True => ASR consumed; upstream must NOT forward raw text to ordinary LLM.
    False => only when child mode is explicitly disabled.
    """
    if not child_mode_enabled():
        return False
    # A newer utterance wins: the previous command's sentence must neither play
    # over this one nor end the chat after it.
    previous = getattr(conn, "child_guard_task", None)
    if previous is not None and not previous.done():
        previous.cancel()
    conn.child_guard_task = None
    # A new turn: clear an earlier abort at the same point upstream's chat path does
    # (startToChat aborts a playing reply, then clears client_abort before answering).
    # An abort that arrives while this turn waits stays set and drops its sentence.
    conn.client_abort = False
    if _is_end_prompt(conn, text):
        # Not the child speaking: never forward upstream's model prompt to PetLearning.
        await _speak(conn, GOODBYE)
        return True
    answer = _end_unclear_run(conn, await _voice_reply(conn, text))
    if isinstance(answer, _Command):
        # Manual listen mode awaits the ASR inside the websocket receive loop, which
        # is also where the device's tool reply arrives: never wait for it here.
        conn.child_guard_task = asyncio.get_running_loop().create_task(_carry_out(conn, answer))
        return True
    await _speak(conn, answer)
    return True
