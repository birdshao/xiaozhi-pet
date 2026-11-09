"""Voice turn pipeline (V5_V6_SERVER_PLAN §6.1-§6.3).

Order: text checks -> quiet hours -> wake-word-only greeting -> fixed commands -> page Q&A -> open chat
(S6, only when every gate is open, see open_chat.py) -> fixed fallback. Every
failure lands on a fixed sentence.
Raw ASR text lives only in this call's locals: it is never stored or logged.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import logging
import sqlite3
import threading
import time
import unicodedata
import zlib
from dataclasses import dataclass
from datetime import datetime
from typing import Any, Callable
from zoneinfo import ZoneInfo

from .db import connect
from .intent import QUIET_OK, is_page_question, is_wake_only, match_fixed
from .jokes import JOKES
from .store import BOOK_RE, DEVICE_RE

log = logging.getLogger("pet_learning.voice")

MAX_TEXT = 120
MAX_BODY = 16 * 1024
CONTEXT_TTL = 120.0
CONTEXT_MAX_ENTRIES = 256
BATTERY_MAX_AGE = 600
TURN_ID_CHARS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")

# One short sentence (about 2 s spoken): a child waits through every syllable.
BAD_TEXT = "没听清，再说一遍好吗？"
QUIET = "现在是安静时间，我们轻轻休息吧。"
NO_BATTERY = "看看屏幕右上角的电池吧。"
NO_PAGE = "我们先翻到书里再问吧。"
NOT_BOUND = "请让家长帮忙连接儿童模式。"
FALLBACK = "没听清，再说一遍好吗？"
WAKE_GREETING = "我在呢。"


def reply(kind: str, tts_text: str, reason: str, action: str | None = None) -> dict[str, Any]:
    return {"kind": kind, "tts_text": tts_text, "action": action, "reason": reason}


@dataclass(frozen=True)
class PageContext:
    scene: str
    page: int
    content_id: int | None = None
    revision: int | None = None
    header_crc: str | None = None
    book_id: str | None = None


class ContextCache:
    """Per-device reader context: memory only, 120 s TTL, never persisted."""

    def __init__(self, clock: Callable[[], float] = time.monotonic, ttl: float = CONTEXT_TTL):
        self._clock, self._ttl, self._items = clock, ttl, {}
        # PUT runs in a worker thread, turns on the event loop.
        self._lock = threading.Lock()

    def put(self, device_id: str, context: PageContext) -> None:
        with self._lock:
            now = self._clock()
            self._items = {k: v for k, v in self._items.items() if v[0] > now}
            if len(self._items) >= CONTEXT_MAX_ENTRIES and device_id not in self._items:
                self._items.pop(min(self._items, key=lambda k: self._items[k][0]))
            self._items[device_id] = (now + self._ttl, context)

    def get(self, device_id: str) -> PageContext | None:
        with self._lock:
            entry = self._items.get(device_id)
            if entry is None:
                return None
            if entry[0] <= self._clock():
                del self._items[device_id]
                return None
            return entry[1]


class LegacyPageSource:
    """Approved text from the pre-S4 `pages` table, keyed by the context's book_id.

    S4 replaces this with a lookup of (content_id, revision, header_crc) in
    `content_pages`; until then a context without book_id resolves to nothing.
    """

    def __init__(self, store):
        self.store = store

    def page_text(self, context: PageContext) -> str | None:
        if not context.book_id or not BOOK_RE.fullmatch(context.book_id):
            return None
        item = self.store.page(context.book_id, context.page)
        text = item["text"] if item else None
        return text if isinstance(text, str) and 0 < len(text) <= 320 else None


class DeviceStateBattery:
    """Reads the S2 `device_state` snapshot row if that table exists, else None."""

    def __init__(self, filename):
        self.filename = filename

    def latest(self, device_id: str) -> tuple[int, int] | None:
        try:
            with connect(self.filename) as db:
                row = db.execute("SELECT battery_pct, received_at FROM device_state "
                                 "WHERE device_id=?", (device_id,)).fetchone()
        except sqlite3.Error:
            return None
        if row is None or type(row[0]) is not int or type(row[1]) is not int:
            return None
        return row[0], row[1]


class VoiceTurns:
    def __init__(self, store, filename, settings, *, clock: Callable[[], float] = time.time,
                 pages=None, battery=None, contexts: ContextCache | None = None, open_chat=None):
        self.store, self.filename, self.settings, self.clock = store, filename, settings, clock
        self.pages = pages or LegacyPageSource(store)
        self.battery = battery or DeviceStateBattery(filename)
        self.contexts = contexts or ContextCache()
        self.open_chat = open_chat

    def authorized(self, token: str | None) -> bool:
        expected = self.settings.voice_token
        if not expected or not token or len(token) > 256:
            return False
        return hmac.compare_digest(hashlib.sha256(token.encode("utf-8")).digest(),
                                   hashlib.sha256(expected.encode("utf-8")).digest())

    @staticmethod
    def turn_id_of(body: bytes) -> str:
        """The caller's turn_id if well formed, else "" (binds the response signature)."""
        try:
            value = json.loads(body).get("turn_id")
        except (ValueError, AttributeError):
            return ""
        if isinstance(value, str) and 8 <= len(value) <= 64 and set(value) <= TURN_ID_CHARS:
            return value
        return ""

    def sign(self, turn_id: str, payload: bytes) -> str:
        """HMAC-SHA256 over turn_id + newline + payload, keyed by the voice token."""
        key = self.settings.voice_token.encode("utf-8")
        return hmac.new(key, turn_id.encode("ascii") + b"\n" + payload, hashlib.sha256).hexdigest()

    def turn(self, device_id: str, body: bytes) -> dict[str, Any]:
        if not DEVICE_RE.fullmatch(device_id):
            # No lookup and no audit row for ids that can never be a device.
            log.info("voice_turn kind=fallback reason=unknown_device ms=0")
            return reply("fallback", NOT_BOUND, "unknown_device")
        started = time.monotonic()
        try:
            result = self._decide(device_id, body)
        except Exception:  # every failure is a fixed sentence, never an LLM
            result = reply("fallback", BAD_TEXT, "internal_error")
        self._record(device_id, result, started)
        return result

    def _decide(self, device_id: str, body: bytes) -> dict[str, Any]:
        text = self._parse(body)
        if text is None:
            return reply("fallback", BAD_TEXT, "text_invalid")
        try:
            quiet = self.store.snapshot(device_id)["quiet"]
        except ValueError:
            return reply("fallback", NOT_BOUND, "unknown_device")
        match = match_fixed(text)
        if quiet and not (match is not None and match.kind == "local_command"
                          and match.action in QUIET_OK):
            return reply("fixed", QUIET, "quiet_hours")
        if is_wake_only(text):
            # The board sends the wake word itself as the first utterance of a session.
            return reply("fixed", WAKE_GREETING, "wake_greeting")
        if match is not None:
            if match.kind == "local_command":
                tts_text = match.tts_text
                if match.action == "TELL_JOKE":
                    tts_text = self._joke(body)
                return reply("local_command", tts_text, match.reason, match.action)
            if match.kind == "time":
                return self._time()
            return self._battery(device_id)
        if is_page_question(text):
            return self._page_answer(device_id)
        if self.open_chat is not None:
            answer = self.open_chat.answer(device_id, text, lambda: self._current_page(device_id),
                                           lambda: self._joke(body))
            if answer is not None:
                return answer
        return reply("fallback", FALLBACK, "no_match")

    def _current_page(self, device_id: str) -> str | None:
        """The approved text of the page the child is looking at, if any (§6.3)."""
        context = self.contexts.get(device_id)
        if context is None or context.scene != "READER":
            return None
        return self.pages.page_text(context)

    @staticmethod
    def _parse(body: bytes) -> str | None:
        if len(body) > MAX_BODY:
            return None
        try:
            data = json.loads(body)
        except ValueError:
            return None
        if not isinstance(data, dict) or set(data) - {"text", "turn_id"}:
            return None
        turn_id = data.get("turn_id")
        if turn_id is not None and (not isinstance(turn_id, str) or not 8 <= len(turn_id) <= 64
                                    or not set(turn_id) <= TURN_ID_CHARS):
            return None
        text = data.get("text")
        if not isinstance(text, str):
            return None
        text = text.strip()
        if not 0 < len(text) <= MAX_TEXT:
            return None
        if any(unicodedata.category(c) in ("Cc", "Cf", "Cs", "Co", "Cn") for c in text):
            return None
        return text

    def _joke(self, body: bytes) -> str:
        """A reviewed joke picked by a pure function of the turn (never generated)."""
        turn_id = self.turn_id_of(body)
        seed = zlib.crc32(turn_id.encode("ascii")) if turn_id else int(self.clock())
        return JOKES[seed % len(JOKES)]

    def _time(self) -> dict[str, Any]:
        local = datetime.fromtimestamp(self.clock(), ZoneInfo(self.settings.timezone))
        if local.minute == 0:
            return reply("fixed", f"现在是{local.hour}点整。", "tell_time")
        return reply("fixed", f"现在是{local.hour}点{local.minute}分。", "tell_time")

    def _battery(self, device_id: str) -> dict[str, Any]:
        snapshot = self.battery.latest(device_id)
        if snapshot is None:
            return reply("fixed", NO_BATTERY, "battery_unavailable")
        pct, received = snapshot
        age = self.clock() - received
        if not 0 <= pct <= 100 or age > BATTERY_MAX_AGE or age < -60:
            return reply("fixed", NO_BATTERY, "battery_stale")
        return reply("fixed", f"还有百分之{pct}的电。", "tell_battery")

    def _page_answer(self, device_id: str) -> dict[str, Any]:
        context = self.contexts.get(device_id)
        if context is None or context.scene != "READER":
            return reply("fallback", NO_PAGE, "no_page_context")
        text = self.pages.page_text(context)
        if text is None:
            return reply("fallback", NO_PAGE, "page_not_found")
        return reply("approved_content", text, "page_qa")

    def _record(self, device_id: str, result: dict[str, Any], started: float) -> None:
        """Audit + log kind/reason only: never the utterance or a reply body."""
        elapsed_ms = int((time.monotonic() - started) * 1000)
        log.info("voice_turn kind=%s reason=%s ms=%d", result["kind"], result["reason"], elapsed_ms)
        try:
            with connect(self.filename) as db:
                db.execute("INSERT INTO audit_log(at,actor,action,target,result,ip) VALUES(?,?,?,?,?,NULL)",
                           (int(self.clock()), "voice", "voice.turn", device_id,
                            f"{result['kind']}:{result['reason']}"))
        except sqlite3.Error:
            log.warning("voice_turn audit_failed")
