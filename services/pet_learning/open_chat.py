"""S6 open chat (V5_V6 plan §6.4/§6.5 as revised by ADR-015/016).

Reached only from the voice turn after fixed commands and page Q&A missed, and
only while every gate is open: the code-level gate (policy.OPEN_CHAT_SELECTABLE,
set once the TTS egress audit was done), a configured provider (GLM key) and the
parent's `policies.open_chat`. Then:

    input hard rules -> GLM (persona + minimal summary + current page, tool =
    self_pet_request limited to the parent's subset) -> output hard rules -> TTS

A tool call never speaks model text: it becomes the same signed local_command
the fixed-command matcher produces, with that action's reviewed sentence, and
the guard picks the final sentence from the device's answer (#114). Voice still
writes no memory and grants no reward; the board decides.

Breaker: 3 consecutive model errors within 10 minutes, or 5 output blocks in a
local day, turn open_chat off with a reason and an audit row; only the parent
can turn it back on (which starts the count afresh).
"""
from __future__ import annotations

import logging
import sqlite3
import time
from datetime import datetime
from typing import Any, Callable
from zoneinfo import ZoneInfo

from . import chat_rules, commands, persona, policy
from .chat_provider import ProviderError, TOOL_NAME
from .db import connect
from .intent import ACTION_REPLIES, ACTIONS

log = logging.getLogger("pet_learning.open_chat")

# Offered on the panel, in this order. Labels are what the parent sees and what the
# persona lists for the model.
LLM_ACTIONS: dict[str, str] = {
    "OPEN_READER": "讲故事", "PAGE_PREV": "上一页", "PAGE_NEXT": "下一页", "OPEN_GAME": "玩游戏",
    "GO_HOME": "回首页", "PET_FEED": "喂团团（只打开选菜画面）", "PET_GROOM": "梳毛", "PLAY_SONG": "唱歌",
    "START_BRUSH": "刷牙", "START_WASH": "洗手", "SLEEP_REQUEST": "睡觉", "ASK_NEEDS": "看看团团饿不饿",
    "TELL_JOKE": "讲笑话",
}
DEFAULT_LLM_ACTIONS = policy.DEFAULT_LLM_ACTIONS
parse_actions = policy.parse_actions
assert tuple(LLM_ACTIONS) == policy.LLM_ACTIONS and set(LLM_ACTIONS) <= ACTIONS
assert "SLEEP_REQUEST" not in DEFAULT_LLM_ACTIONS

ERROR_STREAK = 3
ERROR_WINDOW_S = 600
BLOCKS_PER_DAY = 5
OUTCOME_RETENTION_S = 2 * 86400
REASON_ERRORS = "model_errors"
REASON_BLOCKS = "output_blocked"

CHAT_ERROR = "团团现在答不上来，我们可以问问爸爸妈妈。"
BLOCKED = "这个问题我们去问问爸爸妈妈吧。"
PRIVATE_INPUT = "家里的电话和地址，只告诉爸爸妈妈就好。"


def reply(kind: str, tts_text: str, reason: str, action: str | None = None) -> dict[str, Any]:
    return {"kind": kind, "tts_text": tts_text, "action": action, "reason": reason}


def tool_schema(actions: list[str]) -> list[dict[str, Any]]:
    if not actions:
        return []
    meaning = "；".join(f"{a}={LLM_ACTIONS[a]}" for a in actions)
    return [{"type": "function", "function": {
        "name": TOOL_NAME,
        "description": "请团团的屏幕做一个动作。板子会按规则决定做不做，你不要说已经做了。可选动作：" + meaning,
        "parameters": {"type": "object", "properties": {"action": {"type": "string", "enum": list(actions)}},
                       "required": ["action"]},
    }}]


class OpenChat:
    def __init__(self, filename, settings, provider: Callable[[], Any], *,
                 summary: Callable[[str], dict[str, Any]] | None = None,
                 clock: Callable[[], float] = time.time) -> None:
        self.filename, self.settings, self.clock = filename, settings, clock
        self._provider = provider
        self.summary = summary

    @property
    def provider(self):
        return self._provider()

    # -- gate -------------------------------------------------------------

    def _state(self, device_id: str) -> tuple[bool, list[str]]:
        with connect(self.filename) as db:
            row = db.execute("SELECT p.open_chat, p.llm_actions_json, d.status FROM policies p "
                             "JOIN devices d USING(device_id) WHERE p.device_id=?", (device_id,)).fetchone()
        if row is None or row["status"] != "active":
            return False, []
        return bool(row["open_chat"]), parse_actions(row["llm_actions_json"])

    # -- turn -------------------------------------------------------------

    def answer(self, device_id: str, text: str, page_text: Callable[[], str | None],
               joke: Callable[[], str]) -> dict[str, Any] | None:
        """None when open chat is off for this device (the caller speaks its fallback).

        page_text is called only once every gate is open: an off switch never reads pages.
        """
        if not policy.OPEN_CHAT_SELECTABLE:
            return None
        provider = self.provider
        if provider is None or not provider.configured:
            return None
        enabled, actions = self._state(device_id)
        if not enabled:
            return None
        blocked = chat_rules.check_input(text)
        if blocked:
            return reply("fixed", PRIVATE_INPUT if blocked in ("phone", "address") else BLOCKED,
                         "open_chat_input_blocked")
        system = persona.system_prompt(self._summary(device_id), self._page(page_text),
                                       [LLM_ACTIONS[a] for a in actions])
        try:
            result = provider.complete(system, text, tool_schema(actions))
        except Exception as error:  # noqa: BLE001 - any provider failure counts towards the breaker
            log.warning("open_chat provider_error kind=%s",
                        error.kind if isinstance(error, ProviderError) else "unexpected")
            self._note(device_id, "error")
            return reply("fallback", CHAT_ERROR, "open_chat_error")
        if result.filtered == "input":
            return reply("fixed", BLOCKED, "open_chat_input_blocked")
        if result.filtered:
            self._note(device_id, "block")
            return reply("fallback", BLOCKED, "open_chat_output_blocked")
        if result.tool_action is not None:
            action = result.tool_action
            if action not in actions:
                # Out of contract: the schema only offered `actions`.
                log.warning("open_chat tool_outside_subset")
                self._note(device_id, "error")
                return reply("fallback", CHAT_ERROR, "open_chat_error")
            self._note(device_id, "ok")
            spoken = joke() if action == "TELL_JOKE" else ACTION_REPLIES[action]
            return reply("local_command", spoken, "open_chat_tool", action)
        spoken, why = chat_rules.check_output(result.text or "", truncated=result.truncated)
        if spoken is None:
            if why in chat_rules.FORMAT_REASONS:  # unusable output is a model error, not content
                log.warning("open_chat bad_output rule=%s", why)
                self._note(device_id, "error")
                return reply("fallback", CHAT_ERROR, "open_chat_error")
            log.info("open_chat output_blocked rule=%s", why)
            self._note(device_id, "block")
            return reply("fallback", BLOCKED, "open_chat_output_blocked")
        self._note(device_id, "ok")
        return reply("open_chat", spoken, "open_chat")

    @staticmethod
    def _page(page_text: Callable[[], str | None]) -> str | None:
        try:
            return page_text()
        except Exception:  # noqa: BLE001 - the persona works without the page
            return None

    def _summary(self, device_id: str) -> dict[str, Any]:
        if self.summary is None:
            return {}
        try:
            return self.summary(device_id)
        except Exception:  # noqa: BLE001 - the persona works without memory
            return {}

    # -- breaker ----------------------------------------------------------

    def _day_start(self, now: int) -> int:
        local = datetime.fromtimestamp(now, ZoneInfo(self.settings.timezone))
        return int(local.replace(hour=0, minute=0, second=0, microsecond=0).timestamp())

    def _note(self, device_id: str, outcome: str) -> None:
        now = int(self.clock())
        try:
            with connect(self.filename) as db:
                db.execute("BEGIN IMMEDIATE")
                db.execute("DELETE FROM open_chat_outcomes WHERE at<?", (now - OUTCOME_RETENTION_S,))
                db.execute("INSERT INTO open_chat_outcomes(device_id,at,outcome) VALUES(?,?,?)",
                           (device_id, now, outcome))
                reason = None
                if outcome == "error":
                    last = db.execute("SELECT outcome, at FROM open_chat_outcomes WHERE device_id=? AND "
                                      "outcome IN ('ok','error') ORDER BY rowid DESC LIMIT ?",
                                      (device_id, ERROR_STREAK)).fetchall()
                    if (len(last) == ERROR_STREAK and all(r["outcome"] == "error" for r in last)
                            and now - last[-1]["at"] <= ERROR_WINDOW_S):
                        reason = REASON_ERRORS
                elif outcome == "block":
                    count = db.execute("SELECT COUNT(*) FROM open_chat_outcomes WHERE device_id=? AND "
                                       "outcome='block' AND at>=?", (device_id, self._day_start(now))).fetchone()[0]
                    if count >= BLOCKS_PER_DAY:
                        reason = REASON_BLOCKS
                if reason and db.execute(
                        "UPDATE policies SET open_chat=0, open_chat_disabled_reason=?, open_chat_disabled_at=? "
                        "WHERE device_id=? AND open_chat=1", (reason, now, device_id)).rowcount:
                    commands.audit(db, now, "server", "open_chat.breaker", device_id, reason)
                    log.warning("open_chat breaker_tripped reason=%s", reason)
                db.commit()
        except sqlite3.Error:
            log.warning("open_chat outcome_not_recorded")
