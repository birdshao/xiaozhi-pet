"""Opt-in child ASR guard for xinnan-tech/xiaozhi-esp32-server.

Installed into upstream plugins_func/functions/child_voice_guard.py by the
local overlay script. Non-child mode is an exact no-op. Child mode MUST
consume every ASR utterance, including errors, so arbitrary transcripts
never fall through to normal LLM streaming and unreviewed TTS.
"""
from __future__ import annotations

import hashlib
import hmac
import ipaddress
import os
import uuid
from urllib.parse import urlsplit
import httpx

REVIEWED_FALLBACK = "我们可以看看绘本，或者请家人一起聊聊这个问题。"
NO_SERVICE = "现在还不能语音聊天，我们可以先用按键和小宠物玩。"
NOT_BOUND = "请让家长帮忙连接儿童模式。"
LOCAL_ACTION_HINT = "我听到了，我们用设备上的按键完成这个动作吧。"


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


async def _voice_reply(conn, text: str) -> str:
    """Forward the ASR text to PetLearning and return the sentence it chose.

    All decisions (commands, page Q&A, quiet hours, fallback) live server-side;
    this function holds no intent logic and never triggers a device action.
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
            # Until self.pet.request is verified on the pinned XiaoZhi, point at the buttons.
            return LOCAL_ACTION_HINT
        if kind in _ALLOWED_KINDS and isinstance(spoken, str) and 0 < len(spoken) <= 320:
            return spoken
    except (httpx.HTTPError, httpx.InvalidURL, ValueError, TypeError):
        pass
    return NO_SERVICE


async def handle_child_guard(conn, text: str) -> bool:
    """Called before normal XiaoZhi LLM intent/chat path.

    True => ASR consumed; upstream must NOT forward raw text to ordinary LLM.
    False => only when child mode is explicitly disabled.
    """
    if not child_mode_enabled():
        return False
    answer = await _voice_reply(conn, text)
    # Imported lazily to avoid circular import when upstream loads the hook.
    from core.handle.intentHandler import speak_txt
    conn.sentence_id = uuid.uuid4().hex
    speak_txt(conn, answer)
    return True
